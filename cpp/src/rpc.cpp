#include <QtCore/qloggingcategory.h>
#include "../include/rpc_p.h"
#include "../include/serialization.h"
#include "../include/tran_crypto.h"
#include "../include/transport.h"
#include "../include/peer.h"
#include "../include/sendfile.h"

static Q_LOGGING_CATEGORY(logger, "lafrpc.rpc")

#define PEER_VERSION 1
#define KEY_SIZE 64
#define DEUBG_RPC_PROTOCOL

BEGIN_LAFRPC_NAMESPACE

HeaderCallback::~HeaderCallback() {}
LoggingCallback::~LoggingCallback() {}
MagicCodeManager::~MagicCodeManager() {}


QSharedPointer<qtng::BaseRequestHandler> MagicCodeManager::create(const QByteArray &, QSharedPointer<qtng::SocketLike>, qtng::BaseStreamServer *)
{
    return QSharedPointer<qtng::BaseRequestHandler>();
}

RpcPrivate::RpcPrivate(const QSharedPointer<Serialization> &serialization, Rpc *parent)
    :timeout(10.0), serialization(serialization), crypto(new Crypto()), operations(new qtng::CoroutineGroup), q_ptr(parent)
{
    myPeerName = createUuidAsString();
    if(this->serialization.isNull()) {
        this->serialization.reset(new MessagePackSerialization());
    }
    transports.append(QSharedPointer<Transport>(new TcpTransport(parent)));
    transports.append(QSharedPointer<Transport>(new SslTransport(parent)));
    transports.append(QSharedPointer<Transport>(new HttpTransport(parent)));
    transports.append(QSharedPointer<Transport>(new KcpTransport(parent)));
    transports.append(QSharedPointer<Transport>(new KcpSslTransport(parent)));
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
    bool ok = true;
    QSharedPointer<SslTransport> sslTransport = transports.at(1).dynamicCast<SslTransport>();
    if (!sslTransport.isNull()) {
        sslTransport->setSslConfiguration(config);
    } else {
        ok = false;
    }
    QSharedPointer<HttpTransport> httpTransport = transports.at(2).dynamicCast<HttpTransport>();
    if (!httpTransport.isNull()) {
        httpTransport->setSslConfiguration(config);
    } else {
        ok = false;
    }
    QSharedPointer<KcpSslTransport> kcpSslTransport = transports.at(4).dynamicCast<KcpSslTransport>();
    if (!kcpSslTransport.isNull()) {
        kcpSslTransport->setSslConfiguration(config);
    } else {
        ok = false;
    }
    return ok;
}


bool RpcPrivate::setHttpRootDir(const QDir &rootDir)
{
    QSharedPointer<HttpTransport> httpTransport = transports.at(2).dynamicCast<HttpTransport>();
    if (!httpTransport.isNull()) {
        httpTransport->setRootDir(rootDir);
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
            qCWarning(logger) << "rpc does not support transport for" << address;
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
        qCDebug(logger) << "Rpc::connect() -> unknown address:" << peerName;
        return QSharedPointer<Peer>();
    }

    QSharedPointer<Transport> transport = findTransport(peerAddress);
    if(transport.isNull()) {
        qCDebug(logger) << "Rpc::connect() -> address is not supported." << peerAddress;
        return QSharedPointer<Peer>();
    }
#ifdef DEUBG_RPC_PROTOCOL
    qCDebug(logger) << "Rpc::connect() -> connecting to" << peerAddress;
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
            qCDebug(logger) << "Rpc::connect() -> can not connect to" << peerAddress;
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
        qCDebug(logger) << QStringLiteral("the address of %1 is not known.").arg(peerName);
        return QSharedPointer<qtng::SocketLike>();
    }

    QSharedPointer<Transport> transport = findTransport(address);
    if(transport.isNull()) {
        qCDebug(logger) << "address is not supported." << address;
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
    const QByteArray data = crypto->encrypt(serialization->pack(myHeader));
    if (data.isNull()) {
        qCCritical(logger) << "can not encrypt connection header.";
        return empty;
    }
    qCDebug(logger) << "Rpc::preparePeer() -> my header." << myHeader;
    bool success = channel->sendPacket(data);
    if(!success) {
        qCInfo(logger, "Rpc::preparePeer() -> can not send header.");
        return empty;
    }
    qCDebug(logger) << "Rpc::preparePeer() -> receiving its header.";
    QByteArray packet = channel->recvPacket();
    if(packet.isNull()) {
        qCInfo(logger) << "Rpc::preparePeer() -> can not receive header.";
        return empty;
    }
    packet = crypto->decrypt(packet);
    if (packet.isNull()) {
        qCCritical(logger) << "can not decrypt connection header.";
        return empty;
    }

    QVariantMap itsHeader = serialization->unpack(packet).toMap();
    if(itsHeader.isEmpty()) {
        qCInfo(logger) << "Rpc::preparePeer() -> can not deserialize header.";
        return empty;
    }

    qCDebug(logger) << "Rpc::preparePeer() -> got its header:" << itsHeader;
    const QString itsPeerName = itsHeader.value("peer_name").toString();

    if(!peerName.isEmpty() && peerName != itsPeerName) {
        qCInfo(logger) << QStringLiteral("Rpc::preparePeer() -> peer %1 return mismatched peer_name: %2").arg(peerName).arg(itsPeerName);
        return empty;
    }

    if (myPeerName == itsPeerName) {
        qCInfo(logger) << "Rpc::preparePeer() got a remote peer with the same of my peer name.";
        return empty;
    }

    // only keep a channel between two peers.
    if(myPeerName > itsPeerName) {
        qCDebug(logger) << "Rpc::preparePeer() -> I am larger.";
        if(peers.contains(itsPeerName)) {
            QWeakPointer<Peer> t = peers.value(itsPeerName);
            if(!t.isNull()) {
                qCInfo(logger) << QString::fromUtf8("Rpc::preparePeer() -> %1 peer is already exists.").arg(itsPeerName);
                channel->sendPacket(QByteArray("haha"));
                channel->close();
                return t.toStrongRef();
            }
        }
        qCDebug(logger) << "Rpc::preparePeer() -> sending gaga.";
        channel->sendPacketAsync(QByteArray("gaga"));
    } else {
        qCDebug(logger) << "I am smaller.";
        const QByteArray &flag = channel->recvPacket();
        if(flag.isEmpty()) {
            qCInfo(logger) << "Rpc::preparePeer() -> can not initialize peer.";
            return empty;
        } else if(flag == QByteArray("gaga")) {
            // pass
        } else if(flag == QByteArray("haha")) {
            qCDebug(logger) << "Rpc::preparePeer() -> the other peer decide to close this channel.";
            channel->close();
            QWeakPointer<Peer> t = peers.value(itsPeerName);
            if(!t.isNull()) {
                qCDebug(logger) << "Rpc::preparePeer() -> there is already a peer exists.";
                return t.toStrongRef();
            } else {
                qCDebug(logger) << "Rpc::preparePeer() -> waiting for another handshake.";
                QSharedPointer<qtng::Event> waiter(new qtng::Event());
                waiters.insert(itsPeerName, waiter);
                try {
                    qtng::Timeout _(timeout);
                    if(!waiter->wait()) {
                        qCDebug(logger) << "Rpc::preparePeer() -> timeout to wait for another handshake.";
                        return empty;
                    }
                } catch (qtng::TimeoutException &) {
                    qCDebug(logger) << "Rpc::preparePeer() -> timeout to wait for another handshake.";
                    return empty;
                }
                QSharedPointer<Peer> t = peers.value(itsPeerName);
                if(t.isNull()) {
                    qCDebug(logger) << "Rpc::preparePeer() -> waiter is triggered without peer.";
                }
                return t;
            }
        } else {
            qCDebug(logger) << "Rpc::preparePeer() -> can not initialize peer, receiving invalid packet.";
            return empty;
        }
    }
    QSharedPointer<Peer> peer(new Peer(itsPeerName, channel, q));
    peer->setServices(q->getServices());
    if(!peerAddress.isEmpty()) {
        // XXX only update known addresses in connect() function.
//        knownAddresses[itsPeerName] = peerAddress;
        peer->setAddress(peerAddress);
    }
    if(waiters.contains(itsPeerName)) {
        qCDebug(logger) << "Rpc::preparePeer() -> some one waint for this handshake, wake it up.";
        QSharedPointer<qtng::Event> waiter = waiters.value(itsPeerName);
        waiter->set();
    }

    const QByteArray &certPEM = channel->property("peer_certificate").toByteArray();
    const QByteArray &certHash = channel->property("peer_certificate_hash").toByteArray();
    if (!certPEM.isEmpty() && !certHash.isEmpty()) {
        peer->setProperty("peer_certificate", certPEM);
        peer->setProperty("peer_certificate_hash", certHash);
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
    qCDebug(logger) << "Rpc::preparePeer() -> now the peer" << itsPeerName << "is ready to used.";
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


Rpc::Rpc(const QSharedPointer<Serialization> &serialization)
    :dd_ptr(new RpcPrivate(serialization, this))
{
}


Rpc::~Rpc()
{
    delete dd_ptr;
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


QSharedPointer<MagicCodeManager> Rpc::magicCodeManager() const
{
    Q_D(const Rpc);
    return d->magicCodeManager;
}


QSharedPointer<HeaderCallback> Rpc::headerCallback() const
{
    Q_D(const Rpc);
    return d->headerCallback;
}


void Rpc::setHeaderCallback(QSharedPointer<HeaderCallback> headerCallback)
{
    Q_D(Rpc);
    d->headerCallback = headerCallback;
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


RpcBuilder Rpc::builder(SerializationType serialization)
{
    return RpcBuilder(serialization);
}

RpcBuilder::RpcBuilder(SerializationType serialization)
{
    QSharedPointer<Serialization> s;
    switch(serialization) {
    case Json:
        s.reset(new JsonSerialization());
        break;
    case DataStream:
        s.reset(new DataStreamSerialization());
        break;
    case MessagePack:
        s.reset(new MessagePackSerialization());
        break;
//    default:
//        rpc.clear();
//        return;
    }
    rpc = QSharedPointer<Rpc>::create(s);
}

RpcBuilder &RpcBuilder::sslConfiguration(const qtng::SslConfiguration &config)
{
    if (!rpc.isNull()) {
        rpc->d_func()->setSslConfiguration(config);
    }
    return *this;
}

RpcBuilder &RpcBuilder::headerCallback(QSharedPointer<HeaderCallback> headerCallback)
{
    if (!rpc.isNull()) {
        rpc->d_func()->headerCallback = headerCallback;
    }
    return *this;
}


RpcBuilder &RpcBuilder::loggingCallback(QSharedPointer<LoggingCallback> loggingCallback)
{
    if (!rpc.isNull()) {
        rpc->d_func()->loggingCallback = loggingCallback;
    }
    return *this;
}

RpcBuilder &RpcBuilder::magicCodeManager(QSharedPointer<MagicCodeManager> magicCodeManager)
{
    if (!rpc.isNull()) {
        rpc->d_func()->magicCodeManager = magicCodeManager;
    }
    return *this;
}

RpcBuilder &RpcBuilder::timeout(float timeout)
{
    if (!rpc.isNull()) {
        rpc->d_func()->timeout = timeout;
    }
    return *this;
}

RpcBuilder &RpcBuilder::myPeerName(const QString &myPeerName)
{
    if (!rpc.isNull()) {
        rpc->d_func()->myPeerName = myPeerName;
    }
    return *this;
}


RpcBuilder &RpcBuilder::httpRootDir(const QDir &rootDir)
{
    if (!rpc.isNull()) {
        rpc->d_func()->setHttpRootDir(rootDir);
    }
    return *this;
}


QSharedPointer<Rpc> RpcBuilder::create()
{
    return rpc;
}

END_LAFRPC_NAMESPACE
