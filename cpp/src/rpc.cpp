#include <QtCore/qloggingcategory.h>
#include "../include/rpc_p.h"
#include "../include/serialization.h"
#include "../include/transport.h"
#include "../include/peer.h"
#include "../include/sendfile.h"
#include "../include/senddir.h"

static Q_LOGGING_CATEGORY(logger, "lafrpc.rpc")

#define PEER_VERSION 1
#define KEY_SIZE 64
// #define DEUBG_RPC_PROTOCOL

BEGIN_LAFRPC_NAMESPACE

HeaderCallback::~HeaderCallback() {}
LoggingCallback::~LoggingCallback() {}
KcpFilter::~KcpFilter() {}


RpcPrivate::RpcPrivate(const QSharedPointer<Serialization> &serialization, Rpc *parent)
    : maxPacketSize(1024 * 1024 * 64)
    , keepaliveTimeout(1000 * 20)
    , kcpMode(qtng::KcpSocket::Internet)
    , serialization(serialization)
    , operations(new qtng::CoroutineGroup)
    , dnsCache(new qtng::SocketDnsCache())
    , q_ptr(parent)
{
    myPeerName = createUuidAsString();
    if (this->serialization.isNull()) {
        this->serialization.reset(new MessagePackSerialization());
    }

    transports.append(QSharedPointer<Transport>(new TcpTransport(parent)));
    transports.append(QSharedPointer<Transport>(new SslTransport(parent)));
    transports.append(QSharedPointer<Transport>(new KcpTransport(parent)));
    transports.append(QSharedPointer<Transport>(new KcpSslTransport(parent)));
    transports.append(QSharedPointer<Transport>(new HttpTransport(parent)));
    transports.append(QSharedPointer<Transport>(new HttpsTransport(parent)));
    transports.append(QSharedPointer<Transport>(new HttpSslTransport(parent)));

    registerClass<RpcRemoteException>();
    registerClass<RpcFile>();
    registerClass<RpcDir>();
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
        sslTransport->sslConfig = config;
    } else {
        ok = false;
    }
    QSharedPointer<KcpSslTransport> kcpSslTransport = transports.at(3).dynamicCast<KcpSslTransport>();
    if (!kcpSslTransport.isNull()) {
        kcpSslTransport->sslConfig = config;
    } else {
        ok = false;
    }
    QSharedPointer<HttpsTransport> httpsTransport = transports.at(5).dynamicCast<HttpsTransport>();
    if (!httpsTransport.isNull()) {
        httpsTransport->sslConfig = config;
    } else {
        ok = false;
    }
    QSharedPointer<HttpSslTransport> httpSslTransport = transports.at(6).dynamicCast<HttpSslTransport>();
    if (!httpSslTransport.isNull()) {
        httpSslTransport->sslConfig = config;
    } else {
        ok = false;
    }
    return ok;
}


bool RpcPrivate::setHttpRootDir(const QDir &rootDir)
{
    bool ok = true;
    QSharedPointer<HttpTransport> httpTransport = transports.at(4).dynamicCast<HttpTransport>();
    if (!httpTransport.isNull()) {
        httpTransport->rootDir = rootDir;
    } else {
        ok = false;
    }
    QSharedPointer<HttpsTransport> httpsTransport = transports.at(5).dynamicCast<HttpsTransport>();
    if (!httpsTransport.isNull()) {
        httpsTransport->rootDir = rootDir;
    } else {
        ok = false;
    }
    QSharedPointer<HttpSslTransport> httpSslTransport = transports.at(6).dynamicCast<HttpSslTransport>();
    if (!httpSslTransport.isNull()) {
        httpSslTransport->rootDir = rootDir;
    } else {
        ok = false;
    }
    return ok;
}


bool RpcPrivate::setHttpSession(QSharedPointer<qtng::HttpSession> session)
{
    bool ok = true;
    QSharedPointer<HttpTransport> httpTransport = transports.at(4).dynamicCast<HttpTransport>();
    if (!httpTransport.isNull()) {
        httpTransport->session = session;
    } else {
        ok = false;
    }
    QSharedPointer<HttpsTransport> httpsTransport = transports.at(5).dynamicCast<HttpsTransport>();
    if (!httpsTransport.isNull()) {
        httpsTransport->session = session;
    } else {
        ok = false;
    }
    QSharedPointer<HttpSslTransport> httpSslTransport = transports.at(6).dynamicCast<HttpSslTransport>();
    if (!httpSslTransport.isNull()) {
        httpSslTransport->session = session;
    } else {
        ok = false;
    }
    return ok;
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
        const QString &workerName = makeWorkerName(address);
        if (serverAddressList.contains(address) && operations->has(workerName)) {
            result.append(true);
            coroutines.append(operations->get(workerName));
        } else {
            QSharedPointer<Transport> transport = findTransport(address);
            if (transport.isNull()) {
                qCWarning(logger) << "rpc does not support transport for" << address;
                result.append(false);
            } else {
                QSharedPointer<qtng::Coroutine> coroutine = operations->spawnWithName(workerName, [transport, address] {
                    transport->startServer(address);
                });
                coroutines.append(coroutine);
                serverAddressList.append(address);
                result.append(true);
            }
        }
    }
    if (blocking) {
        for (QSharedPointer<qtng::Coroutine> coroutine: coroutines) {
            coroutine->join();
        }
    }
    return result;
}


QList<bool> RpcPrivate::stopServers(const QStringList &addresses)
{
    const QList<QString> *serverAddressList;
    QList<bool> result;

    if (addresses.isEmpty()) {
        serverAddressList = &this->serverAddressList;
    } else {
        serverAddressList = &addresses;
    }
    for (const QString &address: *serverAddressList) {
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
    if (peers.contains(peerName)) {
        QSharedPointer<Peer> peer = peers.value(peerName);
        if (!peer.isNull()) {
            return peer;
        }
    }
    QString peerAddress;
    if (knownAddresses.contains(peerName)) {
        peerAddress = knownAddresses.value(peerName);
    } else if (peerName.contains(QString::fromLatin1("//"))){
        peerAddress = peerName;
        for (QSharedPointer<Peer> peer: peers.values()) {
            if (peer->address() == peerAddress) {
                return peer;
            }
        }
    } else {
#ifdef DEUBG_RPC_PROTOCOL
        qCDebug(logger) << "Rpc::connect() -> unknown address:" << peerName;
#endif
        return QSharedPointer<Peer>();
    }

    QSharedPointer<Transport> transport = findTransport(peerAddress);
    if (transport.isNull()) {
        qCDebug(logger) << "Rpc::connect() -> address is not supported." << peerAddress;
        return QSharedPointer<Peer>();
    }
#ifdef DEUBG_RPC_PROTOCOL
    qCDebug(logger) << "Rpc::connect() -> connecting to" << peerAddress;
#endif

    QSharedPointer<qtng::Event> event;
    if (connectingEvents.contains(peerAddress)) {
        event = connectingEvents.value(peerAddress);
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
        QSharedPointer<Peer> peer;
        if (channel.isNull()) {
#ifdef DEUBG_RPC_PROTOCOL
            qCDebug(logger) << "Rpc::connect() -> can not connect to" << peerAddress;
#endif
        } else {
            peer = preparePeer(channel, knownAddresses.contains(peerName) ? peerName : QString(), peerAddress);
        }
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


QSharedPointer<qtng::SocketLike> RpcPrivate::makeRawSocket(const QString &peerName, QByteArray &connectionId)
{
    const QString &address = knownAddresses.value(peerName);
    if (address.isEmpty()) {
#ifdef DEUBG_RPC_PROTOCOL
        qCDebug(logger) << QStringLiteral("the address of %1 is not known.").arg(peerName);
#endif
        return QSharedPointer<qtng::SocketLike>();
    }

    QSharedPointer<Transport> transport = findTransport(address);
    if (transport.isNull()) {
#ifdef DEUBG_RPC_PROTOCOL
        qCDebug(logger) << "address is not supported." << address;
#endif
        return QSharedPointer<qtng::SocketLike>();
    }

    return transport->makeRawSocket(address, connectionId);
}


QSharedPointer<qtng::SocketLike> RpcPrivate::takeRawSocket(const QString &peerName, const QByteArray &connectionId)
{
    Q_UNUSED(peerName);
    for (QSharedPointer<Transport> transport: transports) {
        QSharedPointer<qtng::SocketLike> rawSocket = transport->takeRawSocket(connectionId);
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
    if (!localStore.contains(coroutineId)) {
        return QVariantMap();
    }
    const PeerAndHeader &t = localStore[qtng::Coroutine::current()->id()];
    return t.header;
}


QPointer<Peer> RpcPrivate::getCurrentPeer()
{
    quintptr coroutineId = qtng::Coroutine::current()->id();
    if (!localStore.contains(coroutineId)) {
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
    if (data.isNull()) {
        qCWarning(logger) << "can not serialize connection header.";
        return empty;
    }
#ifdef DEUBG_RPC_PROTOCOL
    qCDebug(logger) << "Rpc::preparePeer() -> my header." << myHeader;
#endif
    channel->sendPacketAsync(data);
#ifdef DEUBG_RPC_PROTOCOL
    qCDebug(logger) << "Rpc::preparePeer() -> receiving its header.";
#endif
    QByteArray packet = channel->recvPacket();
    if (packet.isNull()) {
#ifdef DEUBG_RPC_PROTOCOL
        qCInfo(logger) << "Rpc::preparePeer() -> can not receive header.";
#endif
        return empty;
    }

    QVariantMap itsHeader = serialization->unpack(packet).toMap();
    if (itsHeader.isEmpty()) {
#ifdef DEUBG_RPC_PROTOCOL
        qCInfo(logger) << "Rpc::preparePeer() -> can not deserialize header.";
#endif
        return empty;
    }

#ifdef DEUBG_RPC_PROTOCOL
    qCDebug(logger) << "Rpc::preparePeer() -> got its header:" << itsHeader;
#endif
    const QString itsPeerName = itsHeader.value("peer_name").toString();

    if (!peerName.isEmpty() && peerName != itsPeerName) {
        qCInfo(logger) << QStringLiteral("Rpc::preparePeer() -> peer %1 return mismatched peer_name: %2").arg(peerName).arg(itsPeerName);
        return empty;
    }

    if (myPeerName == itsPeerName) {
        qCInfo(logger) << "Rpc::preparePeer() got a remote peer with the same of my peer name.";
        return empty;
    }

    QSharedPointer<Peer> peer(new Peer(itsPeerName, channel, q));
    peer->setServices(q->getServices());
    if (!peerAddress.isEmpty()) {
        // XXX only update known addresses in connect() function.
//        knownAddresses[itsPeerName] = peerAddress;
        peer->setAddress(peerAddress);
    }

    const QByteArray &certPEM = channel->property("peer_certificate").toByteArray();
    const QByteArray &certHash = channel->property("peer_certificate_hash").toByteArray();
    if (!certPEM.isEmpty() && !certHash.isEmpty()) {
        peer->setProperty("peer_certificate", certPEM);
        peer->setProperty("peer_certificate_hash", certHash);
    }

    peers.insert(itsPeerName, peer);
    QPointer<Rpc> self(q);
    qtng::callInEventLoopAsync([peer, self] {
        if (self.isNull()) {
            return;
        }
        emit self.data()->newPeer(peer);
    });
//    emit q->newPeer(peer);  // do not make this.
#ifdef DEUBG_RPC_PROTOCOL
    qCDebug(logger) << "Rpc::preparePeer() -> now the peer" << itsPeerName << "is ready to used.";
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


void RpcPrivate::removePeer(const QString &name, Peer *peer)
{
    const QList<QSharedPointer<Peer>> &l = peers.values(name);
    for (QSharedPointer<Peer> p: l) {
        if (p.data() == peer) {
            peers.remove(name, p);
            return;
        }
    }
}


Rpc::Rpc(const QSharedPointer<Serialization> &serialization)
    :dd_ptr(new RpcPrivate(serialization, this))
{
}


Rpc::~Rpc()
{
    delete dd_ptr;
}

quint32 Rpc::maxPacketSize() const
{
    Q_D(const Rpc);
    return d->maxPacketSize;
}


void Rpc::setMaxPacketSize(quint32 maxPacketSize)
{
    Q_D(Rpc);
    d->maxPacketSize = maxPacketSize;
}


float Rpc::keepaliveTimeout() const
{
    Q_D(const Rpc);
    return d->keepaliveTimeout / 1000.0f;
}


void Rpc::setKeepaliveTimeout(float keepaliveTimeout)
{
    Q_D(Rpc);
    d->keepaliveTimeout = static_cast<quint64>(keepaliveTimeout * 1000.0f);
}


qtng::KcpSocket::Mode Rpc::kcpMode() const
{
    Q_D(const Rpc);
    return d->kcpMode;
}


void Rpc::setKcpMode(qtng::KcpSocket::Mode mode)
{
    Q_D(Rpc);
    d->kcpMode = mode;
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


QSharedPointer<HeaderCallback> Rpc::headerCallback() const
{
    Q_D(const Rpc);
    return d->headerCallback;
}


QSharedPointer<KcpFilter> Rpc::kcpFilter() const
{
    Q_D(const Rpc);
    return d->kcpFilter;
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


QSharedPointer<qtng::SocketLike> Rpc::makeRawSocket(const QString &peerName, QByteArray &connectionId)
{
    Q_D(Rpc);
    return d->makeRawSocket(peerName, connectionId);
}


QSharedPointer<qtng::SocketLike> Rpc::takeRawSocket(const QString &peerName, const QByteArray &connectionId)
{
    Q_D(Rpc);
    return d->takeRawSocket(peerName, connectionId);
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
    for (QSharedPointer<Peer> peer: d->peers.values(peerName)) {
        if (peer->isOk()) {
            return peer;
        }
    }
    return QSharedPointer<Peer>();
}


QList<QSharedPointer<Peer>> Rpc::getAll(const QString &peerName) const
{
    Q_D(const Rpc);
    return d->peers.values(peerName);
}


QStringList Rpc::getAllPeerNames() const
{
    Q_D(const Rpc);
    return d->peers.keys();
}


QList<QSharedPointer<Peer>> Rpc::getAllPeers() const
{
    Q_D(const Rpc);
    return d->peers.values();
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


void Rpc::handleRequest(QSharedPointer<qtng::SocketLike> connection, const QString &address)
{
    Q_D(Rpc);
    for (QSharedPointer<Transport> transport: d->transports) {
        if (transport->canHandle(address)) {
            QByteArray rpcHeader;
            bool done;
            transport->handleRequest(connection, rpcHeader, done);
        }
    }
}


QSharedPointer<Peer> Rpc::preparePeer(QSharedPointer<qtng::DataChannel> channel, const QString &name, const QString &address)
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
    switch (serialization) {
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


RpcBuilder &RpcBuilder::kcpFilter(QSharedPointer<KcpFilter> kcpFilter)
{
    if (!rpc.isNull()) {
        rpc->d_func()->kcpFilter = kcpFilter;
    }
    return *this;
}


RpcBuilder &RpcBuilder::kcpMode(qtng::KcpSocket::Mode kcpMode)
{
    if (!rpc.isNull()) {
        rpc->d_func()->kcpMode = kcpMode;
    }
    return *this;
}


RpcBuilder &RpcBuilder::maxPacketSize(quint32 maxPacketSize)
{
    if (!rpc.isNull()) {
        rpc->d_func()->maxPacketSize = maxPacketSize;
    }
    return *this;
}


RpcBuilder &RpcBuilder::keepaliveTimeout(float keepaliveTimeout)
{
    if (!rpc.isNull()) {
        rpc->d_func()->keepaliveTimeout = static_cast<quint64>(keepaliveTimeout * 1000.0f);
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

RpcBuilder &RpcBuilder::httpSession(const QSharedPointer<qtng::HttpSession> session)
{
    if (!rpc.isNull()) {
        rpc->d_func()->setHttpSession(session);
    }
    return *this;
}


QSharedPointer<Rpc> RpcBuilder::create()
{
    return rpc;
}

END_LAFRPC_NAMESPACE
