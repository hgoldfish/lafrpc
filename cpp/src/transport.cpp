#include <QtCore/qdatetime.h>
#include <QtCore/qurl.h>
#include <QtCore/qloggingcategory.h>
#include "../include/transport.h"
#include "../include/rpc.h"
#include "../include/rpc_p.h"
#include "../include/peer.h"

static Q_LOGGING_CATEGORY(logger, "logger.transport")

using namespace qtng;

BEGIN_LAFRPC_NAMESPACE


Transport::Transport(QPointer<Rpc> rpc)
    : rpc(rpc) {}


Transport::~Transport() {}


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

    QSharedPointer<qtng::BaseStreamServer> server = createServer(address, host, port);
    if (server.isNull()) {
        qCWarning(logger) << "can not create server for" << address;
        return false;
    }
    return server->serveForever();
}


QSharedPointer<qtng::DataChannel> Transport::connect(const QString &address, float timeout)
{
    if (timeout == 0.0f) {
        timeout = 5.0f;
    }
    QString host;
    quint16 port;
    bool valid = parseAddress(address, host, port);
    if (!valid) {
        qCWarning(logger) << address << "is invalid url.";
        return QSharedPointer<qtng::DataChannel>();
    }

    QSharedPointer<SocketLike> request = createConnection(address, host, port, RpcPrivate::getPrivateHelper(rpc.data())->dnsCache);
    if (request.isNull()) {
        return QSharedPointer<qtng::DataChannel>();
    }

    request->setOption(qtng::Socket::LowDelayOption, true);
    qint64 sentBytes = request->sendall("\x4e\x67");
    if (sentBytes != 2) {
        qCDebug(logger) << "handshaking is failed in client side.";
        return QSharedPointer<qtng::DataChannel>();
    }
    QSharedPointer<qtng::DataChannel> channel(new qtng::SocketChannel(request, qtng::PositivePole));
    setupChannel(request, channel);
    return channel;
}


QSharedPointer<qtng::SocketLike> Transport::makeRawSocket(const QString &address, QByteArray &connectionId)
{
    QString host;
    quint16 port;
    bool valid = parseAddress(address, host, port);
    if (!valid) {
        qCWarning(logger) << address << "is invalid url.";
        return QSharedPointer<qtng::SocketLike>();
    }

    QSharedPointer<SocketLike> request = createConnection(address, host, port, RpcPrivate::getPrivateHelper(rpc.data())->dnsCache);
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
        return QSharedPointer<qtng::SocketLike>();
    }
    if (request->recvall(2) != "\xf3\x97") {
        connectionId.clear();
        return QSharedPointer<qtng::SocketLike>();
    } else {
        qCDebug(logger) << "raw socket handshake finished.";
    }
    return request;
}


QSharedPointer<qtng::SocketLike> Transport::takeRawSocket(const QByteArray &connectionId)
{
    const RawSocket &rawConnection = rawConnections[connectionId];
    return rawConnection.connection;
}


void Transport::setupChannel(QSharedPointer<qtng::SocketLike> request, QSharedPointer<qtng::DataChannel> channel)
{
    if (rpc.isNull()) {
        return;
    }
    channel->setMaxPacketSize(rpc->maxPacketSize());

    QSharedPointer<qtng::SslSocket> ssl = qtng::convertSocketLikeToSslSocket(request);
    QSharedPointer<qtng::KcpSocket> kcp;
    if (!ssl.isNull()) {
        const QByteArray &certPEM = ssl->peerCertificate().save(qtng::Ssl::Pem);
        const QByteArray &certHash = ssl->peerCertificate().digest(qtng::MessageDigest::Sha256);
        if (!certPEM.isEmpty() && !certHash.isEmpty()) {
            channel->setProperty("peer_certificate", certPEM);
            channel->setProperty("peer_certificate_hash", certHash);
        }
        kcp = qtng::convertSocketLikeToKcpSocket(ssl->backend());
    } else {
        kcp = qtng::convertSocketLikeToKcpSocket(request);
    }

    if (!kcp.isNull()) {
        channel->setPayloadSizeHint(kcp->payloadSizeHint());
    }
}


void Transport::handleRequest(QSharedPointer<qtng::SocketLike> request, QByteArray &rpcHeader, bool &done)
{
    if (rpc.isNull()) {
        qCDebug(logger) << "rpc is gone.";
        done = false;
        return;
    }

    request->setOption(qtng::Socket::LowDelayOption, true);
    rpcHeader = request->recvall(2);
    if (rpcHeader == QByteArray("\x4e\x67")) {
        done = true;
        QSharedPointer<qtng::DataChannel> channel(new qtng::SocketChannel(request, qtng::NegativePole));
        setupChannel(request, channel);
        QString address = getAddressTemplate();
        const HostAddress &peerAddress = request->peerAddress();
        if (peerAddress.protocol() == HostAddress::IPv6Protocol) {
            address = address.arg(QString::fromLatin1("[%1]").arg(peerAddress.toString()));
        } else {
            address = address.arg(peerAddress.toString());
        }
        address = address.arg(request->peerPort());
        qCDebug(logger) << "got request from:" << address;
        rpc->preparePeer(channel, QString(), address);
    } else if (rpcHeader == QByteArray("\x33\x74")) {
        done = true;
        const QByteArray &connectionId = request->recvall(16);
        if (request->sendall("\xf3\x97") != 2) {
            qCDebug(logger) << "handshaking is failed in server side.";
            return;
        }
        qCDebug(logger) << "got raw socket:" << connectionId;
        rawConnections.insert(connectionId, RawSocket(request, QDateTime::currentDateTime()));
    } else {
        done = false;
    }
}


class TcpTransportRequestHandler: public BaseRequestHandler
{
protected:
    virtual void handle() override
    {
        bool done;
        QByteArray rpcHeader;
        userData<TcpTransport>()->handleRequest(request, rpcHeader, done);
    }
    virtual void finish() override {}
};


QSharedPointer<qtng::SocketLike> TcpTransport::createConnection(const QString &, const QString &host, quint16 port, QSharedPointer<qtng::SocketDnsCache> dnsCache)
{
    QSharedPointer<Socket> s(Socket::createConnection(host, port, nullptr, dnsCache));
    if (!s.isNull()) {
        return asSocketLike(s);
    } else {
        return QSharedPointer<qtng::SocketLike>();
    }
}


QSharedPointer<qtng::BaseStreamServer> TcpTransport::createServer(const QString &, const qtng::HostAddress &host, quint16 port)
{
    QSharedPointer<qtng::BaseStreamServer> server(new TcpServer<TcpTransportRequestHandler>(host, port));
    server->setUserData(this);
    return server;
}



QString TcpTransport::getAddressTemplate()
{
    return QStringLiteral("tcp://%1:%2");
}


bool TcpTransport::canHandle(const QString &address)
{
    return address.startsWith("tcp://", Qt::CaseInsensitive);
}


QSharedPointer<qtng::SocketLike> SslTransport::createConnection(const QString &, const QString &host, quint16 port, QSharedPointer<qtng::SocketDnsCache> dnsCache)
{
    QSharedPointer<SslSocket> ssl(SslSocket::createConnection(host, port, config, nullptr, dnsCache));
    if (ssl) {
        return asSocketLike(ssl);
    } else {
        return QSharedPointer<qtng::SocketLike>();
    }
}


QSharedPointer<qtng::BaseStreamServer> SslTransport::createServer(const QString &, const qtng::HostAddress &host, quint16 port)
{
    QSharedPointer<qtng::BaseStreamServer> server(new SslServer<TcpTransportRequestHandler>(host, port, config));
    server->setUserData(this);
    return server;
}


bool SslTransport::canHandle(const QString &address)
{
    return address.startsWith("ssl://", Qt::CaseInsensitive);
}


QString SslTransport::getAddressTemplate()
{
    return QStringLiteral("ssl://%1:%2");
}


class KcpSocketWithFilter: public qtng::KcpSocket
{
public:
    KcpSocketWithFilter(qtng::HostAddress::NetworkLayerProtocol protocol, QPointer<Rpc> rpc);
    virtual bool filter(char *data, qint32 *len, HostAddress *addr, quint16 *port) override;
    QPointer<Rpc> rpc;
};


KcpSocketWithFilter::KcpSocketWithFilter(qtng::HostAddress::NetworkLayerProtocol protocol, QPointer<Rpc> rpc)
    : qtng::KcpSocket(protocol)
    , rpc(rpc) {}


bool KcpSocketWithFilter::filter(char *data, qint32 *len, HostAddress *addr, quint16 *port)
{
    if (rpc.isNull() || rpc->kcpFilter().isNull()) {
        return false;
    }
    return rpc->kcpFilter()->filter(this, data, len, addr, port);
}


class KcpServerWithFilter: public KcpServer<TcpTransportRequestHandler>
{
public:
    KcpServerWithFilter(const HostAddress &serverAddress, quint16 serverPort)
        : KcpServer<TcpTransportRequestHandler>(serverAddress, serverPort) {}
protected:
    virtual QSharedPointer<SocketLike> serverCreate() override;
};


QSharedPointer<SocketLike> KcpServerWithFilter::serverCreate()
{
    QPointer<Rpc> rpc = static_cast<KcpTransport*>(userData())->rpc;
    return asSocketLike(createServer<KcpSocketWithFilter>(serverAddress(), serverPort(), 0,
            [rpc] (HostAddress::NetworkLayerProtocol family) { return new KcpSocketWithFilter(family, rpc); }));
}


QSharedPointer<qtng::SocketLike> KcpTransport::createConnection(const QString &, const QString &host, quint16 port, QSharedPointer<qtng::SocketDnsCache> dnsCache)
{
    QSharedPointer<KcpSocket> kcp(qtng::createConnection<KcpSocket>(host, port, nullptr, dnsCache, HostAddress::AnyIPProtocol,
                [this] (HostAddress::NetworkLayerProtocol protocol) { return new KcpSocketWithFilter(protocol, rpc); }));
    if (kcp) {
        return asSocketLike(kcp);
    } else {
        return QSharedPointer<qtng::SocketLike>();
    }
}


QSharedPointer<qtng::BaseStreamServer> KcpTransport::createServer(const QString &, const qtng::HostAddress &host, quint16 port)
{
    QSharedPointer<qtng::BaseStreamServer> server(new KcpServerWithFilter(host, port));
    server->setUserData(this);
    return server;
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


QSharedPointer<qtng::SocketLike> KcpSslTransport::createConnection(const QString &, const QString &host, quint16 port, QSharedPointer<qtng::SocketDnsCache> dnsCache)
{
    QSharedPointer<KcpSocket> kcp(qtng::createConnection<KcpSocket>(host, port, nullptr, dnsCache, HostAddress::AnyIPProtocol,
                                               [this] (HostAddress::NetworkLayerProtocol protocol) { return new KcpSocketWithFilter(protocol, rpc); }));
    if (kcp) {
        QSharedPointer<SslSocket> ssl(new qtng::SslSocket(asSocketLike(kcp), config));
        if (!ssl->handshake(false)) {
            return QSharedPointer<SocketLike>();
        }
        return qtng::asSocketLike(ssl);
    } else {
        return QSharedPointer<qtng::SocketLike>();
    }
}


QSharedPointer<qtng::BaseStreamServer> KcpSslTransport::createServer(const QString &, const qtng::HostAddress &host, quint16 port)
{
    QSharedPointer<qtng::BaseStreamServer> server(new SslKcpServerWithFilter(host, port, config));
    server->setUserData(this);
    return server;
}


bool KcpSslTransport::canHandle(const QString &address)
{
    return address.startsWith("kcp+ssl://", Qt::CaseInsensitive) || address.startsWith("ssl+kcp://", Qt::CaseInsensitive);
}


QString KcpSslTransport::getAddressTemplate()
{
    return QStringLiteral("kcp+ssl://%1:%2");
}


class LafrpcHttpRequestHandler: public qtng::SimpleHttpRequestHandler
{
public:
    LafrpcHttpRequestHandler()
        : SimpleHttpRequestHandler(), closeRequest(true) {}
protected:
    virtual bool setup() override;
    virtual void doPOST() override;
    virtual void finish() override;
    virtual QByteArray tryToHandleMagicCode(bool &done) override;
private:
    QPointer<Rpc> rpc;
    bool closeRequest;
};


struct LafrpcHttpData
{
    QString rpcPath;
    HttpTransport *transport;
};


bool LafrpcHttpRequestHandler::setup()
{
    rootDir = userData<LafrpcHttpData>()->transport->rootDir;
    return true;
}


void LafrpcHttpRequestHandler::doPOST()
{
    if (path != userData<LafrpcHttpData>()->rpcPath) {
        return qtng::SimpleHttpRequestHandler::doPOST();
    }
    const QByteArray &connectionHeader = header(ConnectionHeader);
    if (connectionHeader.toLower() != "upgrade") {
        sendError(qtng::NotFound);
        return;
    }
    const QByteArray &upgradeHeader = header(UpgradeHeader);
    if (upgradeHeader.toLower() != "lafrpc") {
        sendError(qtng::NotFound);
        return;
    }
    if (rpc.isNull()) {
        sendError(qtng::ServiceUnavailable);
        return;
    }
    closeConnection = Yes;
    sendResponse(qtng::SwitchProtocol);
    if (!endHeader()) {
        return;
    }

    QByteArray rpcHeader;
    bool done;
    userData<LafrpcHttpData>()->transport->handleRequest(request, rpcHeader, done);
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
    userData<LafrpcHttpData>()->transport->handleRequest(request, rpcHeader, done);
    if (done) {
        closeRequest = false;
    }
    return done ? QByteArray() : rpcHeader;
}



QSharedPointer<qtng::SocketLike> httpConnect(qtng::HttpSession &session, const QString &address)
{
    qtng::HttpRequest request("POST", address);
    request.setStreamResponse(true);
    request.addHeader("Connection", "Upgrade");
    request.addHeader("Upgrade", "lafrpc");
    qtng::HttpResponse response = session.send(request);
    if (!response.isOk()) {
        return QSharedPointer<qtng::SocketLike>();
    }
    if (response.statusCode() != qtng::SwitchProtocol) {
        qCDebug(logger) << "server is a plain http server, while does not support lafrpc.";
        return QSharedPointer<qtng::SocketLike>();
    }
    QByteArray leftBytes;
    QSharedPointer<qtng::SocketLike> stream = response.takeStream(&leftBytes);
    if (Q_UNLIKELY(stream.isNull())) {
        qCWarning(logger) << "got invalid stream";
        return QSharedPointer<qtng::SocketLike>();
    }
    if (Q_UNLIKELY(!leftBytes.isEmpty())) {
        qCWarning(logger) << "the server should not send body.";
        return QSharedPointer<qtng::SocketLike>();
    }
    stream->setOption(qtng::Socket::LowDelayOption, true);
    return stream;
}


QSharedPointer<qtng::SocketLike> HttpTransport::createConnection(const QString &address, const QString &, quint16, QSharedPointer<qtng::SocketDnsCache> dnsCache)
{
    qtng::HttpRequest request("POST", address);
    request.setStreamResponse(true);
    request.addHeader("Connection", "Upgrade");
    request.addHeader("Upgrade", "lafrpc");
    session.setDnsCache(dnsCache);
    qtng::HttpResponse response = session.send(request);
    if (!response.isOk()) {
        return QSharedPointer<qtng::SocketLike>();
    }
    if (response.statusCode() != qtng::SwitchProtocol) {
        qCDebug(logger) << "server is a plain http server, while does not support lafrpc.";
        return QSharedPointer<qtng::SocketLike>();
    }
    QByteArray leftBytes;
    QSharedPointer<qtng::SocketLike> stream = response.takeStream(&leftBytes);
    if (Q_UNLIKELY(stream.isNull())) {
        qCWarning(logger) << "got invalid stream";
        return QSharedPointer<qtng::SocketLike>();
    }
    if (Q_UNLIKELY(!leftBytes.isEmpty())) {
        qCWarning(logger) << "the server should not send body.";
        return QSharedPointer<qtng::SocketLike>();
    }
    return stream;
}


class LafrpcHttpServer: public TcpServer<LafrpcHttpRequestHandler>
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


QSharedPointer<qtng::BaseStreamServer> HttpTransport::createServer(const QString &address, const qtng::HostAddress &host, quint16 port)
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


QString HttpTransport::getAddressTemplate()
{
    return QString::fromLatin1("http://%1:%2");
}


bool HttpTransport::canHandle(const QString &address)
{
    return address.startsWith("http://", Qt::CaseInsensitive);
}


class LafrpcHttpsServer: public SslServer<LafrpcHttpRequestHandler>
{
public:
    explicit LafrpcHttpsServer(const HostAddress &serverAddress, quint16 serverPort, const SslConfiguration &configuration)
        : SslServer(serverAddress, serverPort, configuration)
    {
        setUserData(&data);
    }
public:
    LafrpcHttpData data;
};


QSharedPointer<qtng::BaseStreamServer> HttpsTransport::createServer(const QString &address, const qtng::HostAddress &host, quint16 port)
{
    QUrl u(address);
    QString rpcPath = u.path();
    if (rpcPath.isEmpty()) {
        rpcPath = "/";
    }

    QSharedPointer<LafrpcHttpsServer> server(new LafrpcHttpsServer(host, port, config));
    server->data.rpcPath = rpcPath;
    server->data.transport = this;
    return server;
}


QString HttpsTransport::getAddressTemplate()
{
    return QString::fromLatin1("https://%1:%2");
}


bool HttpsTransport::canHandle(const QString &address)
{
    return address.startsWith("https://", Qt::CaseInsensitive);
}


END_LAFRPC_NAMESPACE
