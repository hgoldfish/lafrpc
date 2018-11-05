#include "../include/rpc_p.h"
#include "../include/serialization.h"
#include "../include/tran_crypto.h"
#include "../include/transport.h"
#include "../include/peer.h"
#include "../include/sendfile.h"

#define PEER_VERSION 1
#define KEY_SIZE 64
#define DEUBG_RPC_PROTOCOL

BEGIN_LAFRPC_NAMESPACE

AuthCallback::~AuthCallback() {}
MakeKeyCallback::~MakeKeyCallback() {}
HeaderCallback::~HeaderCallback() {}
LoggingCallback::~LoggingCallback() {}

RpcPrivate::RpcPrivate(const QString &myPeerName, const QSharedPointer<Serialization> &serialization, Rpc *parent)
    :myPeerName(myPeerName), timeout(10.0), serialization(serialization), operations(new qtng::CoroutineGroup), q_ptr(parent)
{
    if(this->serialization.isNull()) {
        this->serialization.reset(new JsonSerialization());
    }
    if(crypto.isNull()) {
        crypto.reset(new DummyCrypto());
    }
    transports.append(QSharedPointer<Transport>(new TcpTransport(parent)));
    transports.append(QSharedPointer<Transport>(new SslTransport(parent)));
    Serialization::registerClass<RpcRemoteException>();
    Serialization::registerClass<RpcFile>();
}


RpcPrivate::~RpcPrivate()
{
    shutdown();
    delete operations;
}


bool RpcPrivate::setSslConfiguration(const qtng::SslConfiguration &config)
{
    QSharedPointer<SslTransport> transport = transports.at(1).dynamicCast<SslTransport>();
    if (!transport.isNull()) {
        transport->setSslConfiguration(config);
        return true;
    }
    return false;
}

inline QSharedPointer<Transport> RpcPrivate::findTransport(const QString &address)
{
    for (QSharedPointer<Transport> transport: transports) {
        if (transport->canHandle(address)) {
            return transport;
        }
    }
    return QSharedPointer<Transport>();
}


QString makeWorkerName(const QString &address)
{
    return QString::fromLatin1("server_") + QString::number(qHash(address));
}


QList<bool> RpcPrivate::startServers(const QStringList &addresses, bool blocking)
{
    QList<bool> result;
    QList<QSharedPointer<qtng::Coroutine>> coroutines;
    for (QString address: addresses) {
        if(serverAddressList.contains(address)) {
            result.append(true);
            continue;
        }
        QSharedPointer<Transport> transport = findTransport(address);
        if(transport.isNull()) {
            qWarning() << "rpc does not support transport for" << address;
            result.append(false);
        }
        const QString &workerName = makeWorkerName(address);
        QSharedPointer<qtng::Coroutine> coroutine = operations->spawnWithName(workerName, [transport, address] {
            transport->startServer(address);
        });
        coroutines.append(coroutine);
        serverAddressList.append(address);
        result.append(true);
    }
    if(blocking) {
        for (QSharedPointer<qtng::Coroutine> coroutine: coroutines) {
            coroutine->join();
        }
    }
    return result;
}

QList<bool> RpcPrivate::stopServers(const QStringList &addresses)
{
    QList<QString> serverAddressList;
    QList<bool> result;

    if(addresses.isEmpty()) {
        serverAddressList = this->serverAddressList;
    } else {
        serverAddressList = addresses;
    }
    for (const QString &address: serverAddressList) {
        const QString &workerName = makeWorkerName(address);
        bool success = operations->kill(workerName);
        result.append(success);
        this->serverAddressList.removeAll(address);
    }
    return result;
}


void RpcPrivate::shutdown()
{
    stopServers(QStringList());
    for (QSharedPointer<Peer> peer: this->peers.values()) {
        peer->close();
    }
    peers.clear();
    operations->killall();
}

QSharedPointer<Peer> RpcPrivate::connect(const QString &peerName)
{
    if(peers.contains(peerName)) {
        QSharedPointer<Peer> peer = peers.value(peerName);
        if(!peer.isNull()) {
            return peer;
        }
    }
    QString peerAddress;
    if(knownAddresses.contains(peerName)) {
        peerAddress = knownAddresses.value(peerName);
    } else if (peerName.contains(QString::fromLatin1("//"))){
        peerAddress = peerName;
        for (QSharedPointer<Peer> peer: peers.values()) {
            if (peer->address() == peerAddress) {
                return peer;
            }
        }
    } else {
        qDebug() << "Rpc::connect() -> unknown address:" << peerName;
        return QSharedPointer<Peer>();
    }

    QSharedPointer<Transport> transport = findTransport(peerAddress);
    if(transport.isNull()) {
        qDebug() << "Rpc::connect() -> address is not supported." << peerAddress;
        return QSharedPointer<Peer>();
    }
#ifdef DEUBG_RPC_PROTOCOL
    qDebug() << "Rpc::connect() -> connecting to" << peerAddress;
#endif

    QSharedPointer<qtng::Event> event;
    if (connectingEvents.contains(peerAddress)) {
        event= connectingEvents.value(peerAddress);
        event->wait();
        for (QSharedPointer<Peer> peer: peers.values()) {
            if (peer->address() == peerAddress) {
                return peer;
            }
        }
        // fail to connect.
        return QSharedPointer<Peer>();
    } else {
        event.reset(new qtng::Event);
        connectingEvents.insert(peerAddress, event);
    }
    try {
        QSharedPointer<qtng::DataChannel> channel = transport->connect(peerAddress);
        if(channel.isNull()) {
            qDebug() << "Rpc::connect() -> can not connect to" << peerAddress;
            event->set();
            connectingEvents.remove(peerAddress);
            return QSharedPointer<Peer>();
        }
        QSharedPointer<Peer> peer = preparePeer(channel, knownAddresses.contains(peerName) ? peerName : QString(), peerAddress);
        event->set();
        connectingEvents.remove(peerAddress);
        if (!peer.isNull()) {
            knownAddresses[peer->name()] = peerAddress;
        }
        return peer;
    } catch (...) {
        event->set();
        connectingEvents.remove(peerAddress);
        throw;
    }
}


QSharedPointer<qtng::SocketLike> RpcPrivate::makeRawSocket(const QString &peerName, QByteArray *connectionId)
{
    const QString &address = knownAddresses.value(peerName);
    if(address.isEmpty()) {
        qDebug() << QStringLiteral("the address of %1 is not known.").arg(peerName);
        return QSharedPointer<qtng::SocketLike>();
    }

    QSharedPointer<Transport> transport = findTransport(address);
    if(transport.isNull()) {
        qDebug() << "address is not supported." << address;
        return QSharedPointer<qtng::SocketLike>();
    }

    return transport->makeRawSocket(address, connectionId);
}


QSharedPointer<qtng::SocketLike> RpcPrivate::getRawSocket(const QString &peerName, const QByteArray &connectionId)
{
    Q_UNUSED(peerName);
    for (QSharedPointer<Transport> transport: transports) {
        QSharedPointer<qtng::SocketLike> rawSocket = transport->getRawSocket(connectionId);
        if (!rawSocket.isNull()) {
            return rawSocket;
        }
    }
    return QSharedPointer<qtng::SocketLike>();
}


bool RpcPrivate::isConnected(const QString &peerName) const
{
    return peers.contains(peerName) && !peers.value(peerName).isNull();
}


bool RpcPrivate::isConnecting(const QString &peerAddress) const
{
    return connectingEvents.contains(peerAddress);
}


QVariantMap RpcPrivate::getRpcHeader()
{
    quintptr coroutineId = qtng::Coroutine::current()->id();
    if(!localStore.contains(coroutineId)) {
        return QVariantMap();
    }
    const PeerAndHeader &t = localStore[qtng::Coroutine::current()->id()];
    return t.header;
}


QPointer<Peer> RpcPrivate::getCurrentPeer()
{
    quintptr coroutineId = qtng::Coroutine::current()->id();
    if(!localStore.contains(coroutineId)) {
        return QPointer<Peer>();
    }
    const PeerAndHeader &t = localStore[coroutineId];
    return t.peer;
}


QSharedPointer<Peer> RpcPrivate::preparePeer(const QSharedPointer<qtng::DataChannel> &channel, const QString &peerName, const QString &peerAddress)
{
    Q_Q(Rpc);

    QSharedPointer<Peer> empty;
    QVariantMap myHeader;
    myHeader.insert(QString::fromUtf8("peer_name"), myPeerName);
    myHeader.insert(QString::fromUtf8("version"), PEER_VERSION);
    const QByteArray &data = serialization->pack(myHeader);
#ifdef DEUBG_RPC_PROTOCOL
    qDebug() << "Rpc::preparePeer() -> my header." << myHeader;
#endif
    bool success = channel->sendPacket(data);
    if(!success) {
        qDebug() << "Rpc::preparePeer() -> can not send header.";
        return empty;
    }
#ifdef DEUBG_RPC_PROTOCOL
    qDebug() << "Rpc::preparePeer() -> receiving its header.";
#endif
    const QByteArray &packet = channel->recvPacket();
    if(packet.isNull()) {
        qDebug() << "Rpc::preparePeer() -> can not receive header.";
        return empty;
    }

    QVariantMap itsHeader = serialization->unpack(packet).toMap();
    if(itsHeader.isEmpty()) {
        qDebug() << "Rpc::preparePeer() -> can not deserialize header.";
        return empty;
    }

#ifdef DEUBG_RPC_PROTOCOL
    qDebug() << "Rpc::preparePeer() -> got its header:" << itsHeader;
#endif
    const QString itsPeerName = itsHeader.value("peer_name").toString();

    if(!peerName.isEmpty() && peerName != itsPeerName) {
        qDebug() << QStringLiteral("Rpc::preparePeer() -> peer %1 return mismatched peer_name: %2").arg(peerName).arg(itsPeerName);
        return empty;
    }

    if (myPeerName == itsPeerName) {
        qDebug() << "Rpc::preparePeer() got a remote peer with the same of my peer name.";
        return empty;
    }

    if(!authCallback.isNull()) {
        const QByteArray &myChallenge = crypto->genkey(64);
#ifdef DEUBG_RPC_PROTOCOL
        qDebug() << "Rpc::preparePeer() -> sending challenge";
#endif
        success = channel->sendPacket(myChallenge);
        if(!success) {
            qDebug() << "Rpc::preparePeer() -> can not auth peer.";
            return empty;
        }
#ifdef DEUBG_RPC_PROTOCOL
        qDebug() << "Rpc::preparePeer() -> receiving challenge";
#endif
        const QByteArray &itsChallenge = channel->recvPacket();
        if(itsChallenge.isEmpty()) {
            qDebug() << "Rpc::preparePeer() -> can not auth peer.";
            return empty;
        }

        const QByteArray &myAnser = authCallback->answer(itsPeerName, itsChallenge);
#ifdef DEUBG_RPC_PROTOCOL
        qDebug() << "Rpc::preparePeer() -> sending my answer.";
#endif
        success = channel->sendPacket(myAnser);
        if (!success) {
            qDebug() << "Rpc::preparePeer() -> can not auth peer.";
            return empty;
        }

#ifdef DEUBG_RPC_PROTOCOL
        qDebug() << "Rpc::preparePeer() -> receiving its answer.";
#endif
        const QByteArray &itsAnswer = channel->recvPacket();
        if(itsAnswer.isEmpty()) {
            qDebug() << "Rpc::preparePeer() -> can not auth peer.";
            return empty;
        }

        if(!authCallback->verify(itsPeerName, myChallenge, itsAnswer)) {
            qDebug() << "Rpc::preparePeer() -> can not verify peer";
            return empty;
        }
    }

    QByteArray key;
    if(!makeKeyCallback.isNull()) {
        if(myPeerName > itsPeerName) {
            key = crypto->genkey(KEY_SIZE);
            const QByteArray &data = makeKeyCallback->encrypt(itsPeerName, key);
#ifdef DEUBG_RPC_PROTOCOL
            qDebug() << "Rpc::preparePeer() -> sending key.";
#endif
            success = channel->sendPacket(data);
            if(!success) {
                qDebug() << "Rpc::preparePeer() -> can not exchange key.";
                return empty;
            }
        } else {
#ifdef DEUBG_RPC_PROTOCOL
            qDebug() << "Rpc::preparePeer() -> receiving key.";
#endif
            const QByteArray &data = channel->recvPacket();
            if(data.isEmpty()) {
                qDebug() << "Rpc::preparePeer() -> can not exchange key.";
                return empty;
            }
            key = makeKeyCallback->decrypt(itsPeerName, data);
            if(key.size() != KEY_SIZE) {
                qDebug() << "Rpc::preparePeer() -> can not exchange key, invalid key.";
                return empty;
            }
        }
#ifdef DEUBG_RPC_PROTOCOL
        qDebug() << "Rpc::preparePeer() -> got key:" << key;
#endif
    }

    // only keep a channel between two peers.
    if(myPeerName > itsPeerName) {
#ifdef DEUBG_RPC_PROTOCOL
        qDebug() << "Rpc::preparePeer() -> I am larger.";
#endif
        if(peers.contains(itsPeerName)) {
            QWeakPointer<Peer> t = peers.value(itsPeerName);
            if(!t.isNull()) {
#ifdef DEUBG_RPC_PROTOCOL
                qDebug() << QString::fromUtf8("Rpc::preparePeer() -> %1 peer is already exists.").arg(itsPeerName);
#endif
                channel->sendPacket(QByteArray("haha"));
                channel->close();
                return t.toStrongRef();
            }
        }
#ifdef DEUBG_RPC_PROTOCOL
        qDebug() << "Rpc::preparePeer() -> sending gaga.";
#endif
        channel->sendPacketAsync(QByteArray("gaga"));
    } else {
#ifdef DEUBG_RPC_PROTOCOL
        qDebug() << "I am smaller.";
#endif
        const QByteArray &flag = channel->recvPacket();
        if(flag.isEmpty()) {
            qDebug() << "Rpc::preparePeer() -> can not initialize peer.";
            return empty;
        } else if(flag == QByteArray("gaga")) {
            // pass
        } else if(flag == QByteArray("haha")) {
#ifdef DEUBG_RPC_PROTOCOL
            qDebug() << "Rpc::preparePeer() -> the other peer decide to close this channel.";
#endif
            channel->close();
            QWeakPointer<Peer> t = peers.value(itsPeerName);
            if(!t.isNull()) {
#ifdef DEUBG_RPC_PROTOCOL
                qDebug() << "Rpc::preparePeer() -> there is already a peer exists.";
#endif
                return t.toStrongRef();
            } else {
#ifdef DEUBG_RPC_PROTOCOL
                qDebug() << "Rpc::preparePeer() -> waiting for another handshake.";
#endif
                QSharedPointer<qtng::Event> waiter(new qtng::Event());
                waiters.insert(itsPeerName, waiter);
                try {
                    qtng::Timeout _(timeout);
                    if(!waiter->wait()) {
#ifdef DEUBG_RPC_PROTOCOL
                        qDebug() << "Rpc::preparePeer() -> timeout to wait for another handshake.";
#endif
                        return empty;
                    }
                } catch (qtng::TimeoutException &) {
                    qDebug() << "Rpc::preparePeer() -> timeout to wait for another handshake.";
                    return empty;
                }
                QSharedPointer<Peer> t = peers.value(itsPeerName);
                if(t.isNull()) {
#ifdef DEUBG_RPC_PROTOCOL
                    qDebug() << "Rpc::preparePeer() -> waiter is triggered without peer.";
#endif
                }
                return t;
            }
        } else {
#ifdef DEUBG_RPC_PROTOCOL
            qDebug() << "Rpc::preparePeer() -> can not initialize peer, receiving invalid packet.";
#endif
            return empty;
        }
    }
    QSharedPointer<Peer> peer(new Peer(itsPeerName, channel, q, key));
    peer->setServices(q->getServices());
    if(!peerAddress.isEmpty()) {
        // XXX only update known addresses in connect() function.
//        knownAddresses[itsPeerName] = peerAddress;
        peer->setAddress(peerAddress);
    }
    if(waiters.contains(itsPeerName)) {
#ifdef DEUBG_RPC_PROTOCOL
        qDebug() << "Rpc::preparePeer() -> some one waint for this handshake, wake it up.";
#endif
        QSharedPointer<qtng::Event> waiter = waiters.value(itsPeerName);
        waiter->set();
    }

    peers[itsPeerName] = peer;
    QPointer<Rpc> self(q);
    qtng::callInEventLoopAsync([peer, self] {
        if (self.isNull()) {
            return;
        }
        emit self.data()->newPeer(peer);
    });
//    emit q->newPeer(peer);
#ifdef DEUBG_RPC_PROTOCOL
    qDebug() << "Rpc::preparePeer() -> now the peer" << itsPeerName << "is ready to used.";
#endif
    return peer;
}


void RpcPrivate::setCurrentPeerAndHeader(const QPointer<Peer> &peer, const QVariantMap &header)
{
    quintptr coroutineId = qtng::Coroutine::current()->id();
    PeerAndHeader &t = localStore[coroutineId];
    t.header = header;
    t.peer = peer;
}


void RpcPrivate::deleteCurrentPeerAndHeader()
{
    quintptr coroutineId = qtng::Coroutine::current()->id();
    localStore.remove(coroutineId);
}


void RpcPrivate::removePeer(const QString &peerName)
{
    peers.remove(peerName);
}


QSharedPointer<Rpc> Rpc::use(const QString &name, const QString &serialization)
{
    QSharedPointer<Serialization> s;
    if(serialization == "json") {
        s.reset(new JsonSerialization());
    } else if (serialization == "datastream") {
        s.reset(new DataStreamSerialization());
    } else if (serialization == "msgpack") {
        s.reset(new MessagePackSerialization());
    } else {
        return QSharedPointer<Rpc>();
    }
    return QSharedPointer<Rpc>::create(name, s);
}


Rpc::Rpc(const QString &myPeerName, const QSharedPointer<Serialization> &serialization)
    :d_ptr(new RpcPrivate(myPeerName, serialization, this))
{
}


Rpc::~Rpc()
{
    delete d_ptr;
}


void Rpc::setSslConfiguration(const qtng::SslConfiguration &config)
{
    Q_D(Rpc);
    d->setSslConfiguration(config);
}

float Rpc::timeout() const
{
    Q_D(const Rpc);
    return d->timeout;
}


void Rpc::setTimeout(float timeout)
{
    Q_D(Rpc);
    d->timeout = timeout;
}


QString Rpc::myPeerName() const
{
    Q_D(const Rpc);
    return d->myPeerName;
}


void Rpc::setMyPeerName(const QString &name)
{
    Q_D(Rpc);
    d->myPeerName = name;
}


QSharedPointer<Serialization> Rpc::serialization() const
{
    Q_D(const Rpc);
    return d->serialization;
}


QSharedPointer<Crypto> Rpc::crypto() const
{
    Q_D(const Rpc);
    return d->crypto;
}


void Rpc::setAuthCallback(QSharedPointer<AuthCallback> authCallback)
{
    Q_D(Rpc);
    d->authCallback = authCallback;
}


void Rpc::setMakeKeyCallback(QSharedPointer<MakeKeyCallback> makeKeyCallback)
{
    Q_D(Rpc);
    d->makeKeyCallback = makeKeyCallback;
}


void Rpc::setHeaderCallback(QSharedPointer<HeaderCallback> headerCallback)
{
    Q_D(Rpc);
    d->headerCallback = headerCallback;
}


void Rpc::setLoggingCallback(QSharedPointer<LoggingCallback> loggingCallback)
{
    Q_D(Rpc);
    d->loggingCallback = loggingCallback;
}


QList<bool> Rpc::startServers(const QStringList &addresses, bool blocking)
{
    Q_D(Rpc);
    return d->startServers(addresses, blocking);
}


bool Rpc::startServer(const QString &address, bool blocking)
{
    Q_D(Rpc);
    QList<QString> addresses;
    addresses.append(address);
    QList<bool> result = d->startServers(addresses, blocking);
    return result[0];
}


QList<bool> Rpc::stopServers(const QStringList &addresses)
{
    Q_D(Rpc);
    return d->stopServers(addresses);
}


bool Rpc::stopServer(const QString &address)
{
    Q_D(Rpc);
    QList<QString> addresses;
    addresses.append(address);
    const QList<bool> &result = d->stopServers(addresses);
    return result[0];
}


void Rpc::shutdown()
{
    Q_D(Rpc);
    d->shutdown();
}


QSharedPointer<qtng::SocketLike> Rpc::makeRawSocket(const QString &peerName, QByteArray *connectionId)
{
    Q_D(Rpc);
    return d->makeRawSocket(peerName, connectionId);
}


QSharedPointer<qtng::SocketLike> Rpc::getRawSocket(const QString &peerName, const QByteArray &connectionId)
{
    Q_D(Rpc);
    return d->getRawSocket(peerName, connectionId);
}


QSharedPointer<Peer> Rpc::connect(const QString &peerName)
{
    Q_D(Rpc);
    return d->connect(peerName);
}


bool Rpc::isConnected(const QString &peerName) const
{
    Q_D(const Rpc);
    return d->isConnected(peerName);
}


bool Rpc::isConnecting(const QString &peerAddress) const
{
    Q_D(const Rpc);
    return d->isConnecting(peerAddress);
}

QSharedPointer<Peer> Rpc::get(const QString &peerName) const
{
    Q_D(const Rpc);
    return d->peers.value(peerName);
}


QString Rpc::address(const QString &peerName) const
{
    Q_D(const Rpc);
    return d->knownAddresses.value(peerName);
}


void Rpc::setAddress(const QString &peerName, const QString &peerAddress)
{
    Q_D(Rpc);
    d->knownAddresses.insert(peerName, peerAddress);
}


QList<QSharedPointer<Peer>> Rpc::getAllPeers() const
{
    Q_D(const Rpc);
    return d->peers.values();
}


QPointer<Peer> Rpc::getCurrentPeer()
{
    Q_D(Rpc);
    return d->getCurrentPeer();
}


QVariantMap Rpc::getRpcHeader()
{
    Q_D(Rpc);
    return d->getRpcHeader();
}


QSharedPointer<Peer> Rpc::preparePeer(const QSharedPointer<qtng::DataChannel> &channel, const QString &name, const QString &address)
{
    Q_D(Rpc);
    return d->preparePeer(channel, name, address);
}

END_LAFRPC_NAMESPACE
