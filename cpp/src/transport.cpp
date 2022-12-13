#include <QtCore/qdatetime.h>
#include <QtCore/qurl.h>
#include <QtCore/qloggingcategory.h>
#include "../include/transport.h"
#include "../include/rpc.h"
#include "../include/rpc_p.h"
#include "../include/peer.h"

static Q_LOGGING_CATEGORY(logger, "lafrpc.transport");

using namespace qtng;

BEGIN_LAFRPC_NAMESPACE

Transport::Transport(QPointer<Rpc> rpc)
    : rpc(rpc)
{
}

Transport::~Transport() { }

bool Transport::parseAddress(const QString &address, QString &host, quint16 &port)
{
    if (!canHandle(address)) {
        return false;
    }
    QUrl u(address);
    if (!u.isValid() || u.port() <= 0) {
        return false;
    }
    host = u.host();
    if (host.isEmpty()) {
        return false;
    }
    port = static_cast<quint16>(u.port());
    return port > 0;
}

bool Transport::startServer(const QString &address)
{
    QString hostStr;
    quint16 port;
    bool valid = parseAddress(address, hostStr, port);
    if (!valid) {
        qCWarning(logger) << address << "is invalid url.";
        return false;
    }

    HostAddress host(hostStr);
    if (host.isNull()) {
        const QList<HostAddress> &l = RpcPrivate::getPrivateHelper(rpc.data())->dnsCache->resolve(hostStr);
        if (l.isEmpty()) {
            qCWarning(logger) << "require ip address to start server.";
            return false;
        }
        host = l.first();
    }

    QSharedPointer<BaseStreamServer> server = createServer(address, host, port);
    if (server.isNull()) {
        qCWarning(logger) << "can not create server for" << address;
        return false;
    }
    return server->serveForever();
}

QSharedPointer<DataChannel> Transport::connect(const QString &address)
{
    QString host;
    quint16 port;
    bool valid = parseAddress(address, host, port);
    if (!valid) {
        qCWarning(logger) << address << "is invalid url.";
        return QSharedPointer<DataChannel>();
    }

    QSharedPointer<SocketLike> request =
            createConnection(address, host, port, RpcPrivate::getPrivateHelper(rpc.data())->dnsCache);
    if (request.isNull()) {
        return QSharedPointer<DataChannel>();
    }

    request->setOption(Socket::LowDelayOption, true);
    qint64 sentBytes = request->sendall("\x4e\x67");
    if (sentBytes != 2) {
        qCDebug(logger) << "handshaking is failed in client side.";
        return QSharedPointer<DataChannel>();
    }
    QSharedPointer<SocketChannel> channel(new SocketChannel(request, PositivePole));
    setupChannel(request, channel);
    return channel;
}

QSharedPointer<SocketLike> Transport::makeRawSocket(const QString &address, QByteArray &connectionId)
{
    QString host;
    quint16 port;
    bool valid = parseAddress(address, host, port);
    if (!valid) {
        qCWarning(logger) << address << "is invalid url.";
        return QSharedPointer<SocketLike>();
    }

    QSharedPointer<SocketLike> request =
            createConnection(address, host, port, RpcPrivate::getPrivateHelper(rpc.data())->dnsCache);
    if (request.isNull()) {
        return request;
    }

    connectionId = randomBytes(16);
    QByteArray packet = connectionId;
    packet.prepend("\x33\x74");
    qint64 sentBytes = request->sendall(packet);
    if (sentBytes != packet.size()) {
        connectionId.clear();
        qCDebug(logger) << "handshaking is failed in client side.";
        return QSharedPointer<SocketLike>();
    }
    if (request->recvall(2) != "\xf3\x97") {
        connectionId.clear();
        return QSharedPointer<SocketLike>();
    } else {
        qCDebug(logger) << "raw socket handshake finished.";
    }
    return request;
}

QSharedPointer<SocketLike> Transport::takeRawSocket(const QByteArray &connectionId)
{
    const RawSocket &rawConnection = rawConnections[connectionId];
    return rawConnection.connection;
}

void Transport::setupChannel(QSharedPointer<SocketLike> request, QSharedPointer<SocketChannel> channel)
{
    if (rpc.isNull()) {
        return;
    }
    channel->setMaxPacketSize(rpc->maxPacketSize());
    channel->setPayloadSizeHint(rpc->payloadSizeHint());
    channel->setKeepaliveTimeout(rpc->keepaliveTimeout());

    QSharedPointer<SslSocket> ssl = convertSocketLikeToSslSocket(request);
    QSharedPointer<KcpSocket> kcp;
    if (!ssl.isNull()) {
        const QByteArray &certPEM = ssl->peerCertificate().save(Ssl::Pem);
        const QByteArray &certHash = ssl->peerCertificate().digest(MessageDigest::Sha256);
        if (!certPEM.isEmpty() && !certHash.isEmpty()) {
            channel->setProperty("peer_certificate", certPEM);
            channel->setProperty("peer_certificate_hash", certHash);
        }
        kcp = convertSocketLikeToKcpSocket(ssl->backend());
    } else {
        kcp = convertSocketLikeToKcpSocket(request);
    }

    if (!kcp.isNull()) {
        kcp->setMode(rpc->kcpMode());
        channel->setPayloadSizeHint(kcp->payloadSizeHint());
    }
}

bool Transport::handleRequest(QSharedPointer<SocketLike> request, QByteArray &rpcHeader)
{
    if (rpc.isNull()) {
        qCDebug(logger) << "rpc is gone.";
        return false;
    }

    request->setOption(Socket::LowDelayOption, true);
    rpcHeader = request->recvall(2);
    if (rpcHeader == QByteArray("\x4e\x67")) {
        QSharedPointer<SocketChannel> channel(new SocketChannel(request, NegativePole));
        setupChannel(request, channel);
        QString address = getAddressTemplate();
        const HostAddress &peerAddress = request->peerAddress();
        if (peerAddress.protocol() == HostAddress::IPv6Protocol) {
            address = address.arg(QString::fromLatin1("[%1]").arg(peerAddress.toString()));
        } else {
            address = address.arg(peerAddress.toString());
        }
        address = address.arg(request->peerPort());
        // qCDebug(logger) << "got request from:" << address;
        rpc->preparePeer(channel, QString(), address);
    } else if (rpcHeader == QByteArray("\x33\x74")) {
        const QByteArray &connectionId = request->recvall(16);
        if (request->sendall("\xf3\x97") != 2) {
            qCDebug(logger) << "handshaking is failed in server side.";
            return false;
        }
        qCDebug(logger) << "got raw socket:" << connectionId;
        rawConnections.insert(connectionId, RawSocket(request, QDateTime::currentDateTime()));
    } else {
        return false;
    }
    return true;
}

class TcpTransportRequestHandler : public BaseRequestHandler
{
protected:
    virtual void handle() override
    {
        QByteArray rpcHeader;
        userData<TcpTransport>()->handleRequest(request, rpcHeader);
    }
    virtual void finish() override { }
};

QSharedPointer<SocketLike> TcpTransport::createConnection(const QString &, const QString &host, quint16 port,
                                                          QSharedPointer<SocketDnsCache> dnsCache)
{
    QSharedPointer<Socket> s(Socket::createConnection(host, port, nullptr, dnsCache));
    if (!s.isNull()) {
        return asSocketLike(s);
    } else {
        return QSharedPointer<SocketLike>();
    }
}

QSharedPointer<BaseStreamServer> TcpTransport::createServer(const QString &, const HostAddress &host, quint16 port)
{
    QSharedPointer<BaseStreamServer> server(new TcpServer<TcpTransportRequestHandler>(host, port));
    server->setUserData(this);
    return server;
}

QString TcpTransport::getAddressTemplate()
{
    return QStringLiteral("tcp://%1:%2");
}

QString TcpTransport::name() const
{
    return QString::fromUtf8("TcpTransport");
}

bool TcpTransport::canHandle(const QString &address)
{
    return address.startsWith("tcp://", Qt::CaseInsensitive);
}

QSharedPointer<SocketLike> SslTransport::createConnection(const QString &, const QString &host, quint16 port,
                                                          QSharedPointer<SocketDnsCache> dnsCache)
{
    QSharedPointer<SslSocket> ssl(SslSocket::createConnection(host, port, sslConfig, nullptr, dnsCache));
    if (!ssl.isNull()) {
        return asSocketLike(ssl);
    } else {
        return QSharedPointer<SocketLike>();
    }
}

QSharedPointer<BaseStreamServer> SslTransport::createServer(const QString &, const HostAddress &host, quint16 port)
{
    QSharedPointer<BaseStreamServer> server(new SslServer<TcpTransportRequestHandler>(host, port, sslConfig));
    server->setUserData(this);
    return server;
}

QString SslTransport::name() const
{
    return QString::fromUtf8("SslTransport");
}

bool SslTransport::canHandle(const QString &address)
{
    return address.startsWith("ssl://", Qt::CaseInsensitive) || address.startsWith("ssl+tcp://", Qt::CaseInsensitive);
}

QString SslTransport::getAddressTemplate()
{
    return QStringLiteral("ssl://%1:%2");
}

class KcpSocketWithFilter : public KcpSocket
{
public:
    KcpSocketWithFilter(HostAddress::NetworkLayerProtocol protocol, QPointer<Rpc> rpc);
    virtual bool filter(char *data, qint32 *len, HostAddress *addr, quint16 *port) override;
    QPointer<Rpc> rpc;
};

KcpSocketWithFilter::KcpSocketWithFilter(HostAddress::NetworkLayerProtocol protocol, QPointer<Rpc> rpc)
    : KcpSocket(protocol)
    , rpc(rpc)
{
}

bool KcpSocketWithFilter::filter(char *data, qint32 *len, HostAddress *addr, quint16 *port)
{
    if (rpc.isNull() || rpc->kcpFilter().isNull()) {
        return false;
    }
    return rpc->kcpFilter()->filter(this, data, len, addr, port);
}

class KcpServerWithFilter : public KcpServer<TcpTransportRequestHandler>
{
public:
    KcpServerWithFilter(const HostAddress &serverAddress, quint16 serverPort)
        : KcpServer<TcpTransportRequestHandler>(serverAddress, serverPort)
    {
    }
protected:
    virtual QSharedPointer<SocketLike> serverCreate() override;
};

QSharedPointer<SocketLike> KcpServerWithFilter::serverCreate()
{
    QPointer<Rpc> rpc = static_cast<KcpTransport *>(userData())->rpc;
    return asSocketLike(createServer<KcpSocketWithFilter>(
            serverAddress(), serverPort(), 0,
            [rpc](HostAddress::NetworkLayerProtocol family) { return new KcpSocketWithFilter(family, rpc); }));
}

QSharedPointer<SocketLike> KcpTransport::createConnection(const QString &, const QString &host, quint16 port,
                                                          QSharedPointer<SocketDnsCache> dnsCache)
{
    QSharedPointer<KcpSocket> kcp(qtng::createConnection<KcpSocket>(
            host, port, nullptr, dnsCache, HostAddress::AnyIPProtocol,
            [this](HostAddress::NetworkLayerProtocol protocol) { return new KcpSocketWithFilter(protocol, rpc); }));
    if (kcp) {
        return asSocketLike(kcp);
    } else {
        return QSharedPointer<SocketLike>();
    }
}

QSharedPointer<BaseStreamServer> KcpTransport::createServer(const QString &, const HostAddress &host, quint16 port)
{
    QSharedPointer<BaseStreamServer> server(new KcpServerWithFilter(host, port));
    server->setUserData(this);
    return server;
}

QString KcpTransport::name() const
{
    return QString::fromUtf8("KcpTransport");
}

bool KcpTransport::canHandle(const QString &address)
{
    return address.startsWith("kcp://", Qt::CaseInsensitive);
}

QString KcpTransport::getAddressTemplate()
{
    return QStringLiteral("kcp://%1:%2");
}

typedef WithSsl<KcpServerWithFilter> SslKcpServerWithFilter;

QSharedPointer<SocketLike> KcpSslTransport::createConnection(const QString &, const QString &host, quint16 port,
                                                             QSharedPointer<SocketDnsCache> dnsCache)
{
    QSharedPointer<KcpSocket> kcp(qtng::createConnection<KcpSocket>(
            host, port, nullptr, dnsCache, HostAddress::AnyIPProtocol,
            [this](HostAddress::NetworkLayerProtocol protocol) { return new KcpSocketWithFilter(protocol, rpc); }));
    if (kcp) {
        QSharedPointer<SslSocket> ssl(new SslSocket(asSocketLike(kcp), sslConfig));
        if (!ssl->handshake(false)) {
            return QSharedPointer<SocketLike>();
        }
        return asSocketLike(ssl);
    } else {
        return QSharedPointer<SocketLike>();
    }
}

QSharedPointer<BaseStreamServer> KcpSslTransport::createServer(const QString &, const HostAddress &host, quint16 port)
{
    QSharedPointer<BaseStreamServer> server(new SslKcpServerWithFilter(host, port, sslConfig));
    server->setUserData(this);
    return server;
}

QString KcpSslTransport::name() const
{
    return QString::fromUtf8("KcpSslTransport");
}

bool KcpSslTransport::canHandle(const QString &address)
{
    return address.startsWith("kcp+ssl://", Qt::CaseInsensitive)
            || address.startsWith("ssl+kcp://", Qt::CaseInsensitive);
}

QString KcpSslTransport::getAddressTemplate()
{
    return QStringLiteral("kcp+ssl://%1:%2");
}

class LafrpcHttpRequestHandler : public SimpleHttpRequestHandler
{
public:
    LafrpcHttpRequestHandler()
        : SimpleHttpRequestHandler()
        , closeRequest(true)
    {
    }
protected:
    virtual bool setup() override;
    virtual void doPOST() override;
    virtual void finish() override;
    virtual QByteArray tryToHandleMagicCode(bool &done) override;
private:
    bool closeRequest;
};

struct LafrpcHttpData
{
    QString rpcPath;
    HttpTransport *transport;
    SslConfiguration sslConfig;
};

bool LafrpcHttpRequestHandler::setup()
{
    rootDir = userData<LafrpcHttpData>()->transport->rootDir;
    return true;
}

void LafrpcHttpRequestHandler::doPOST()
{
    if (path != userData<LafrpcHttpData>()->rpcPath) {
        return SimpleHttpRequestHandler::doPOST();
    }
    const QByteArray &connectionHeader = header(ConnectionHeader);
    if (connectionHeader.toLower() != "upgrade") {
        sendError(NotFound);
        return;
    }
    const QByteArray &upgradeHeader = header(UpgradeHeader);
    if (upgradeHeader.toLower() != "lafrpc") {
        sendError(NotFound);
        return;
    }
    QPointer<Rpc> rpc = userData<LafrpcHttpData>()->transport->rpc;
    if (rpc.isNull()) {
        sendError(ServiceUnavailable);
        return;
    }
    closeConnection = Yes;
    sendResponse(SwitchProtocol);
    if (!endHeader()) {
        return;
    }

    QSharedPointer<SocketLike> stream;
    const SslConfiguration &sslConfig = userData<LafrpcHttpData>()->sslConfig;
    if (!sslConfig.isNull()) {
        QSharedPointer<SslSocket> ssl(new SslSocket(request, sslConfig));
        if (!ssl->handshake(true)) {
            return;
        }
        stream = asSocketLike(ssl);
    } else {
        stream = request;
    }

    QByteArray rpcHeader;
    bool done = userData<LafrpcHttpData>()->transport->handleRequest(stream, rpcHeader);
    if (done) {
        closeRequest = false;
    }
}

void LafrpcHttpRequestHandler::finish()
{
    if (closeRequest) {
        request->close();
    }
}

QByteArray LafrpcHttpRequestHandler::tryToHandleMagicCode(bool &done)
{
    QByteArray rpcHeader;
    done = userData<LafrpcHttpData>()->transport->handleRequest(request, rpcHeader);
    if (done) {
        closeRequest = false;
    }
    return done ? QByteArray() : rpcHeader;
}

QSharedPointer<SocketLike> HttpTransport::createConnection(const QString &address, const QString &, quint16,
                                                           QSharedPointer<SocketDnsCache> dnsCache)
{
    HttpRequest request("POST", address);
    request.setStreamResponse(true);
    request.addHeader("Connection", "Upgrade");
    request.addHeader("Upgrade", "lafrpc");
    session->setDnsCache(dnsCache);
    HttpResponse response = session->send(request);
    if (!response.isOk()) {
        return QSharedPointer<SocketLike>();
    }
    if (response.statusCode() != SwitchProtocol) {
        qCDebug(logger) << "server is a plain http server, while does not support lafrpc.";
        return QSharedPointer<SocketLike>();
    }
    QByteArray leftBytes;
    QSharedPointer<SocketLike> stream = response.takeStream(&leftBytes);
    if (Q_UNLIKELY(stream.isNull())) {
        qCWarning(logger) << "got invalid stream";
        return QSharedPointer<SocketLike>();
    }
    if (Q_UNLIKELY(!leftBytes.isEmpty())) {
        qCWarning(logger) << "the server should not send body.";
        return QSharedPointer<SocketLike>();
    }
    return stream;
}

class LafrpcHttpServer : public TcpServer<LafrpcHttpRequestHandler>
{
public:
    explicit LafrpcHttpServer(const HostAddress &serverAddress, quint16 serverPort)
        : TcpServer(serverAddress, serverPort)
    {
        setUserData(&data);
    }
public:
    LafrpcHttpData data;
};

QSharedPointer<BaseStreamServer> HttpTransport::createServer(const QString &address, const HostAddress &host,
                                                             quint16 port)
{
    QUrl u(address);
    QString rpcPath = u.path();
    if (rpcPath.isEmpty()) {
        rpcPath = "/";
    }

    QSharedPointer<LafrpcHttpServer> server(new LafrpcHttpServer(host, port));
    server->data.rpcPath = rpcPath;
    server->data.transport = this;
    return server;
}

bool HttpTransport::parseAddress(const QString &address, QString &host, quint16 &port)
{
    if (!canHandle(address)) {
        return false;
    }
    QUrl u(address);
    if (!u.isValid()) {
        return false;
    }
    host = u.host();
    if (host.isEmpty()) {
        return false;
    }
    port = static_cast<quint16>(u.port(80));
    return port > 0;
}

QString HttpTransport::getAddressTemplate()
{
    return QString::fromLatin1("http://%1:%2");
}

QString HttpTransport::name() const
{
    return QString::fromUtf8("HttpTransport");
}

bool HttpTransport::canHandle(const QString &address)
{
    return address.startsWith("http://", Qt::CaseInsensitive);
}

class LafrpcHttpsServer : public SslServer<LafrpcHttpRequestHandler>
{
public:
    explicit LafrpcHttpsServer(const HostAddress &serverAddress, quint16 serverPort,
                               const SslConfiguration &configuration)
        : SslServer(serverAddress, serverPort, configuration)
    {
        setUserData(&data);
    }
public:
    LafrpcHttpData data;
};

QSharedPointer<BaseStreamServer> HttpsTransport::createServer(const QString &address, const HostAddress &host,
                                                              quint16 port)
{
    QUrl u(address);
    QString rpcPath = u.path();
    if (rpcPath.isEmpty()) {
        rpcPath = "/";
    }

    QSharedPointer<LafrpcHttpsServer> server(new LafrpcHttpsServer(host, port, sslConfig));
    server->data.rpcPath = rpcPath;
    server->data.transport = this;
    return server;
}

bool HttpsTransport::parseAddress(const QString &address, QString &host, quint16 &port)
{
    if (!canHandle(address)) {
        return false;
    }
    QUrl u(address);
    if (!u.isValid()) {
        return false;
    }
    host = u.host();
    if (host.isEmpty()) {
        return false;
    }
    port = static_cast<quint16>(u.port(443));
    return port > 0;
}

QString HttpsTransport::getAddressTemplate()
{
    return QString::fromLatin1("https://%1:%2");
}

QString HttpsTransport::name() const
{
    return QString::fromUtf8("HttpsTransport");
}

bool HttpsTransport::canHandle(const QString &address)
{
    return address.startsWith("https://", Qt::CaseInsensitive);
}

QSharedPointer<SocketLike> HttpSslTransport::createConnection(const QString &address, const QString &host, quint16 port,
                                                              QSharedPointer<SocketDnsCache> dnsCache)
{
    QString tmpAddress = address;
    tmpAddress.replace(QString::fromUtf8("http+ssl://"), QString::fromUtf8("http://"));
    QSharedPointer<SocketLike> stream = HttpTransport::createConnection(tmpAddress, host, port, dnsCache);
    if (stream.isNull()) {
        return stream;
    }
    QSharedPointer<SslSocket> ssl(new SslSocket(stream, sslConfig));
    if (!ssl->handshake(false)) {
        return QSharedPointer<SocketLike>();
    }
    return asSocketLike(ssl);
}

QSharedPointer<BaseStreamServer> HttpSslTransport::createServer(const QString &address, const HostAddress &host,
                                                                quint16 port)
{
    QUrl u(address);
    QString rpcPath = u.path();
    if (rpcPath.isEmpty()) {
        rpcPath = "/";
    }

    QSharedPointer<LafrpcHttpServer> server(new LafrpcHttpServer(host, port));
    server->data.rpcPath = rpcPath;
    server->data.transport = this;
    server->data.sslConfig = sslConfig;
    return server;
}

QString HttpSslTransport::getAddressTemplate()
{
    return QString::fromLatin1("http+ssl://%1:%2");
}

QString HttpSslTransport::name() const
{
    return QString::fromUtf8("HttpSslTransport");
}

bool HttpSslTransport::canHandle(const QString &address)
{
    return address.startsWith("http+ssl://", Qt::CaseInsensitive);
}

END_LAFRPC_NAMESPACE
