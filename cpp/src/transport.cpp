#include <QtCore/qdatetime.h>
#include <QtCore/qurl.h>
#include <QtCore/qloggingcategory.h>
#include "../include/transport.h"
#include "../include/rpc.h"
#include "../include/peer.h"
#include "../include/tran_crypto.h"

static Q_LOGGING_CATEGORY(logger, "logger.transport")

BEGIN_LAFRPC_NAMESPACE


Transport::~Transport() {}


TcpTransport::TcpTransport(QPointer<Rpc> rpc)
    :Transport(rpc), operations(new qtng::CoroutineGroup)
{

}


TcpTransport::~TcpTransport()
{
    delete operations;
}


bool TcpTransport::makeSocket(const QString &address, QSharedPointer<qtng::SocketLike> *socket, QHostAddress *host, quint16 *port)
{
    QUrl u(address);
    if (!u.isValid() || u.scheme() != "tcp" || u.port() <= 0) {
        return false;
    }
    *host = QHostAddress(u.host());
    *port = static_cast<quint16>(u.port());
    if(host->isNull()) {
        return false;
    }
    *socket = qtng::SocketLike::rawSocket(new qtng::Socket);
    return true;
}


bool TcpTransport::startServer(const QString &address)
{
    QSharedPointer<qtng::SocketLike> serverSocket;
    QHostAddress host;
    quint16 port;
    bool valid = makeSocket(address, &serverSocket, &host, &port);
    if(!valid) {
        return false;
    }

    bool success;
    success = serverSocket->setOption(qtng::Socket::AddressReusable, true);
    if(!success) {
        qCDebug(logger) << "can not set address reusable option.";
        return false;
    }
    success = serverSocket->bind(host, port);
    if(!success) {
        qCDebug(logger) << "can not bind to" << host << port;
        return false;
    }
    success = serverSocket->listen(50);
    if(!success) {
        qCDebug(logger) << "can not listen.";
        return false;
    }
    while(true) {
        QSharedPointer<qtng::SocketLike> request = serverSocket->accept();
        if (request.isNull()) {
            break;
        }
        qCDebug(logger) << "got new request.";
        operations->spawn([request, this] {
            this->handleRequest(request);
        });
    }
    return true;
}


void TcpTransport::handleRequest(QSharedPointer<qtng::SocketLike> request)
{
    request->setOption(qtng::Socket::LowDelayOption, true);
    const QByteArray header = request->recvall(2);
    if(header == QByteArray("\x4e\x67")) {
        if(rpc.isNull()) {
            qCDebug(logger) << "rpc is gone.";
            return;
        }
        QSharedPointer<qtng::DataChannel> channel(new qtng::SocketChannel(request, qtng::NegativePole));
        setupChannel(request, channel);
        QString address = getAddressTemplate();
        const QHostAddress peerAddress = request->peerAddress();
        if (peerAddress.protocol() == QAbstractSocket::IPv6Protocol) {
            address = address.arg(QStringLiteral("[%1]").arg(peerAddress.toString()));
        } else {
            address = address.arg(peerAddress.toString());
        }
        address = address.arg(request->peerPort());
        qCDebug(logger) << "got request from:" << address;
        QSharedPointer<Peer> peer = rpc->preparePeer(channel, QString(), address);
    } else if(header == QByteArray("\x33\x74")) {
        const QByteArray &connectionId = request->recvall(16);
        if(request->sendall("\xf3\x97") != 2) {
            qCDebug(logger) << "handshaking is failed in server side.";
            return;
        }
        qCDebug(logger) << "got raw socket:" << connectionId;
        rawConnections.insert(connectionId, RawSocket(request, QDateTime::currentDateTime()));
    } else {
        if (!rpc.isNull()) {
            QSharedPointer<MagicCodeManager> magicCodeManager = rpc.data()->magicCodeManager();
            if (magicCodeManager.isNull()) {
                qCDebug(logger) << "invalid incoming request, does not match magic number." << header;
            } else {
                QSharedPointer<qtng::BaseRequestHandler> handler = magicCodeManager->create(header, request, nullptr);
                handler->run();
            }
        }
    }
}


void TcpTransport::setupChannel(QSharedPointer<qtng::SocketLike>, QSharedPointer<qtng::DataChannel>)
{
}


QString TcpTransport::getAddressTemplate()
{
    return QStringLiteral("tcp://%1:%2");
}


QSharedPointer<qtng::DataChannel> TcpTransport::connect(const QString &address, float timeout)
{
    if(timeout == 0.0f) {
        timeout = 5.0f;
    }
    QSharedPointer<qtng::SocketLike> request;
    QHostAddress host;
    quint16 port;
    bool valid = makeSocket(address, &request, &host, &port);
    if(!valid) {
        return QSharedPointer<qtng::DataChannel>();
    }
    request->setOption(qtng::Socket::LowDelayOption, true);
    bool success = request->connect(host, port);
    if(!success) {
        return QSharedPointer<qtng::DataChannel>();
    }
    qint64 sentBytes = request->sendall("\x4e\x67");
    if(sentBytes != 2) {
        qCDebug(logger) << "handshaking is failed in client side.";
        return QSharedPointer<qtng::DataChannel>();
    }
    QSharedPointer<qtng::DataChannel> channel(new qtng::SocketChannel(request, qtng::PositivePole));
    if (canHandle("ssl://")) {
        QSharedPointer<qtng::SslSocket> sslSocket = qtng::convertSocketLikeToSslSocket(request);
        if (!sslSocket.isNull()) {
            const QByteArray &certPEM = sslSocket->peerCertificate().save(qtng::Ssl::Pem);
            const QByteArray &certHash = sslSocket->peerCertificate().digest(qtng::MessageDigest::Sha256);
            if (!certPEM.isEmpty() && !certHash.isEmpty()) {
                channel->setProperty("peer_certificate", certPEM);
                channel->setProperty("peer_certificate_hash", certHash);
            }
        }
    }
    return channel;
}


QSharedPointer<qtng::SocketLike> TcpTransport::makeRawSocket(const QString &address, QByteArray *connectionId)
{
    QSharedPointer<qtng::SocketLike> request;
    QHostAddress host;
    quint16 port;
    bool valid = makeSocket(address, &request, &host, &port);
    if(!valid) {
        return QSharedPointer<qtng::SocketLike>();
    }
    *connectionId = urandom(16);
    QByteArray packet = *connectionId;
    packet.prepend("\x33\x74");
    if (!request->connect(host, port)) {
        return QSharedPointer<qtng::SocketLike>();
    }
    qint64 sentBytes = request->sendall(packet);
    if(sentBytes != packet.size()) {
        return QSharedPointer<qtng::SocketLike>();
    }
    if (request->recvall(2) != "\xf3\x97") {
        return QSharedPointer<qtng::SocketLike>();
    } else {
        qCDebug(logger) << "raw socket handshake finished.";
    }
    return request;
}


QSharedPointer<qtng::SocketLike> TcpTransport::getRawSocket(const QByteArray &connectionId)
{
    const RawSocket &rawConnection = rawConnections[connectionId];
    return rawConnection.connection;
}


bool TcpTransport::canHandle(const QString &address)
{
    return address.startsWith("tcp://");
}


bool SslTransport::makeSocket(const QString &address, QSharedPointer<qtng::SocketLike> *socket, QHostAddress *host, quint16 *port)
{
    QUrl u(address);
    if (!u.isValid() || u.scheme() != "ssl" || u.port() <= 0) {
        return false;
    }
    *host = QHostAddress(u.host());
    *port = static_cast<quint16>(u.port());
    if(host->isNull()) {
        return false;
    }
    *socket = qtng::SocketLike::sslSocket(new qtng::SslSocket(qtng::Socket::AnyIPProtocol, config));
    return true;
}


bool SslTransport::canHandle(const QString &address)
{
    return address.startsWith("ssl://");
}


void SslTransport::setupChannel(QSharedPointer<qtng::SocketLike> request, QSharedPointer<qtng::DataChannel> channel)
{
    QSharedPointer<qtng::SslSocket> sslSocket = qtng::convertSocketLikeToSslSocket(request);
    if (!sslSocket.isNull()) {
        const QByteArray &certPEM = sslSocket->peerCertificate().save(qtng::Ssl::Pem);
        const QByteArray &certHash = sslSocket->peerCertificate().digest(qtng::MessageDigest::Sha256);
        if (!certPEM.isEmpty() && !certHash.isEmpty()) {
            channel->setProperty("peer_certificate", certPEM);
            channel->setProperty("peer_certificate_hash", certHash);
        }
    }
}


QString SslTransport::getAddressTemplate()
{
    return QStringLiteral("ssl://%1:%2");
}


bool KcpTransport::makeSocket(const QString &address, QSharedPointer<qtng::SocketLike> *socket, QHostAddress *host, quint16 *port)
{
    QUrl u(address);
    if (!u.isValid() || u.scheme() != "kcp" || u.port() <= 0) {
        return false;
    }
    *host = QHostAddress(u.host());
    *port = static_cast<quint16>(u.port());
    if(host->isNull()) {
        return false;
    }
    *socket = qtng::SocketLike::kcpSocket(new qtng::KcpSocket(qtng::Socket::AnyIPProtocol));
    return true;
}


bool KcpTransport::canHandle(const QString &address)
{
    return address.startsWith("kcp://");
}


void KcpTransport::setupChannel(QSharedPointer<qtng::SocketLike> request, QSharedPointer<qtng::DataChannel> channel)
{
}


QString KcpTransport::getAddressTemplate()
{
    return QStringLiteral("kcp://%1:%2");
}



bool KcpSslTransport::makeSocket(const QString &address, QSharedPointer<qtng::SocketLike> *socket, QHostAddress *host, quint16 *port)
{
    QUrl u(address);
    if (!u.isValid() || u.scheme() != "kcp+ssl" || u.port() <= 0) {
        return false;
    }
    *host = QHostAddress(u.host());
    *port = static_cast<quint16>(u.port());
    if(host->isNull()) {
        return false;
    }
    QSharedPointer<qtng::SocketLike> kcp = qtng::SocketLike::kcpSocket(new qtng::KcpSocket(qtng::Socket::AnyIPProtocol));
    *socket = qtng::SocketLike::sslSocket(new qtng::SslSocket(kcp, config));
    return true;
}


bool KcpSslTransport::canHandle(const QString &address)
{
    return address.startsWith("kcp+ssl://");
}


void KcpSslTransport::setupChannel(QSharedPointer<qtng::SocketLike> request, QSharedPointer<qtng::DataChannel> channel)
{
    QSharedPointer<qtng::SslSocket> sslSocket = qtng::convertSocketLikeToSslSocket(request);
    if (!sslSocket.isNull()) {
        const QByteArray &certPEM = sslSocket->peerCertificate().save(qtng::Ssl::Pem);
        const QByteArray &certHash = sslSocket->peerCertificate().digest(qtng::MessageDigest::Sha256);
        if (!certPEM.isEmpty() && !certHash.isEmpty()) {
            channel->setProperty("peer_certificate", certPEM);
            channel->setProperty("peer_certificate_hash", certHash);
        }
    }
}


QString KcpSslTransport::getAddressTemplate()
{
    return QStringLiteral("kcp+ssl://%1:%2");
}


class LafrpcHttpServer: public qtng::BaseStreamServer
{
public:
    LafrpcHttpServer(const QHostAddress &serverAddress, quint16 serverPort, HttpTransport *transport,
                     const QString &rpcPath, const QDir &rootDir)
        :qtng::BaseStreamServer(serverAddress, serverPort)
        , transport(transport), rpcPath(rpcPath), rootDir(rootDir) {}
protected:
    virtual void processRequest(QSharedPointer<qtng::SocketLike> request) override;
public:
    HttpTransport *transport;
    QString rpcPath;
    QDir rootDir;
};

class LafrpcHttpsServer: public qtng::BaseSslStreamServer
{
public:
    LafrpcHttpsServer(const QHostAddress &serverAddress, quint16 serverPort, const qtng::SslConfiguration &configuration,
                      HttpTransport *transport, const QString &rpcPath, const QDir &rootDir)
        :qtng::BaseSslStreamServer(serverAddress, serverPort, configuration)
        , transport(transport), rpcPath(rpcPath), rootDir(rootDir) {}
protected:
    virtual void processRequest(QSharedPointer<qtng::SocketLike> request) override;
public:
    HttpTransport *transport;
    QString rpcPath;
    QDir rootDir;
};

class LafrpcHttpRequestHandler: public qtng::SimpleHttpRequestHandler
{
public:
    LafrpcHttpRequestHandler(QSharedPointer<qtng::SocketLike> request, qtng::BaseStreamServer *server)
        :qtng::SimpleHttpRequestHandler(request, server), transport(nullptr), closeRequest(true)
    {
        if (server->isSecure()) {
            LafrpcHttpsServer *lafrpcHttpsServer = dynamic_cast<LafrpcHttpsServer*>(server);
            if (lafrpcHttpsServer) {
                transport = lafrpcHttpsServer->transport;
                rpc = transport->rpc;
                rpcPath = lafrpcHttpsServer->rpcPath;
                rootDir = lafrpcHttpsServer->rootDir;
            }
        } else {
            LafrpcHttpServer *lafrpcHttpServer = dynamic_cast<LafrpcHttpServer*>(server);
            if (lafrpcHttpServer) {
                transport = lafrpcHttpServer->transport;
                rpc = transport->rpc;
                rpcPath = lafrpcHttpServer->rpcPath;
                rootDir = lafrpcHttpServer->rootDir;
            }
        }
    }
protected:
    virtual void doPOST() override;
    virtual void finish() override;
    virtual QByteArray tryToHandleMagicCode(bool *done) override;
private:
    HttpTransport *transport;
    QPointer<Rpc> rpc;
    QString rpcPath;
    bool closeRequest;
};

void LafrpcHttpServer::processRequest(QSharedPointer<qtng::SocketLike> request)
{
    LafrpcHttpRequestHandler handler(request, this);
    handler.run();
}

void LafrpcHttpsServer::processRequest(QSharedPointer<qtng::SocketLike> request)
{
    LafrpcHttpRequestHandler handler(request, this);
    handler.run();
}

void LafrpcHttpRequestHandler::doPOST()
{
    if (path != rpcPath) {
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
    if (!transport || rpc.isNull()) {
        sendError(qtng::ServiceUnavailable);
        return;
    }
    closeConnection = true;
    sendResponse(qtng::SwitchProtocol);
    endHeader();

    request->setOption(qtng::Socket::LowDelayOption, true);

    const QByteArray &rpcHeader = request->recvall(2);
    if(rpcHeader == QByteArray("\x4e\x67")) {
        if(rpc.isNull()) {
            qCDebug(logger) << "rpc is gone.";
            return;
        }
        QSharedPointer<qtng::DataChannel> channel(new qtng::SocketChannel(request, qtng::NegativePole));
        QString address;
        if (server->isSecure()) {
            address = QStringLiteral("https://%1:%2");
        } else {
            address = QStringLiteral("http://%1:%2");
        }
        const QHostAddress peerAddress = request->peerAddress();
        if (peerAddress.protocol() == QAbstractSocket::IPv6Protocol) {
            address = address.arg(QStringLiteral("[%1]").arg(peerAddress.toString()));
        } else {
            address = address.arg(peerAddress.toString());
        }
        address = address.arg(request->peerPort());
        qCDebug(logger) << "got request from:" << address;
        QSharedPointer<Peer> peer = rpc->preparePeer(channel, QString(), address);
        if (!peer.isNull()) {
            closeRequest = false;
        }
    } else if(rpcHeader == QByteArray("\x33\x74")) {
        const QByteArray &connectionId = request->recvall(16);
        if(request->sendall("\xf3\x97") != 2) {
            qCDebug(logger) << "handshaking is failed in server side.";
            return;
        }
        qCDebug(logger) << "got raw socket:" << connectionId;
        transport->rawConnections.insert(connectionId, RawSocket(request, QDateTime::currentDateTime()));
        closeRequest = false;
    } else {
        if (!rpc.isNull()) {
            QSharedPointer<MagicCodeManager> magicCodeManager = rpc.data()->magicCodeManager();
            if (magicCodeManager.isNull()) {
                qCInfo(logger) << "invalid incoming request, does not match magic number." << rpcHeader;
                return;
            } else {
                QSharedPointer<qtng::BaseRequestHandler> handler = magicCodeManager->create(rpcHeader, request, server);
                handler->run();
            }
        }
    }
}

void LafrpcHttpRequestHandler::finish()
{
    if (closeRequest) {
        request->close();
    }
}


QByteArray LafrpcHttpRequestHandler::tryToHandleMagicCode(bool *done)
{
    *done = false;
    const QByteArray &rpcHeader = request->recvall(2);
    if(rpcHeader == QByteArray("\x4e\x67")) {
        closeConnection = true;
        if(rpc.isNull()) {
            qCDebug(logger) << "rpc is gone.";
            *done = true;
            return QByteArray();
        }
        QSharedPointer<qtng::DataChannel> channel(new qtng::SocketChannel(request, qtng::NegativePole));
        QString address;
        if (server->isSecure()) {
            address = QStringLiteral("https://%1:%2");
        } else {
            address = QStringLiteral("http://%1:%2");
        }
        const QHostAddress peerAddress = request->peerAddress();
        if (peerAddress.protocol() == QAbstractSocket::IPv6Protocol) {
            address = address.arg(QStringLiteral("[%1]").arg(peerAddress.toString()));
        } else {
            address = address.arg(peerAddress.toString());
        }
        address = address.arg(request->peerPort());
        qCDebug(logger) << "got request from:" << address;
        QSharedPointer<Peer> peer = rpc->preparePeer(channel, QString(), address);
        if (!peer.isNull()) {
            closeRequest = false;
        }
        *done = true;
        return QByteArray();
    } else if(rpcHeader == QByteArray("\x33\x74")) {
        const QByteArray &connectionId = request->recvall(16);
        if(request->sendall("\xf3\x97") != 2) {
            qCDebug(logger) << "handshaking is failed in server side.";
            return QByteArray();
        }
        qCDebug(logger) << "got raw socket:" << connectionId;
        transport->rawConnections.insert(connectionId, RawSocket(request, QDateTime::currentDateTime()));
        closeRequest = false;
        *done = true;
        return QByteArray();
    } else {
        if (!rpc.isNull()) {
            QSharedPointer<MagicCodeManager> magicCodeManager = rpc.data()->magicCodeManager();
            if (!magicCodeManager.isNull()) {
                QSharedPointer<qtng::BaseRequestHandler> handler = magicCodeManager->create(rpcHeader, request, server);
                handler->run();
                *done = true;
                return QByteArray();
            }
        }
    }
    *done = false;
    return rpcHeader;
}

bool HttpTransport::startServer(const QString &address)
{
    QUrl u(address);
    if (!u.isValid()) {
        return false;
    }
    quint16 port;
    QSharedPointer<qtng::BaseStreamServer> server;
    QHostAddress host = QHostAddress(u.host());
    QString rpcPath = u.path();
    if (rpcPath.isEmpty()) {
        rpcPath = "/";
    }
    if (host.isNull()) {
        qCWarning(logger) << "require ip address to start http server.";
        return false;
    }

    if (u.scheme() == "https") {
        port = static_cast<quint16>(u.port(443));
        server.reset(new LafrpcHttpsServer(host, port, config, this, rpcPath, rootDir));
    } else {
        port = static_cast<quint16>(u.port(80));
        server.reset(new LafrpcHttpServer(host, port, this, rpcPath, rootDir));
    }
    return server->serveForever();
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

QSharedPointer<qtng::DataChannel> HttpTransport::connect(const QString &address, float )
{
    QSharedPointer<qtng::SocketLike> stream = httpConnect(session, address);
    if (stream.isNull()) {
        return QSharedPointer<qtng::DataChannel>();
    }
    qint64 sentBytes = stream->sendall("\x4e\x67");
    if(sentBytes != 2) {
        qCDebug(logger) << "handshaking is failed in client side.";
        return QSharedPointer<qtng::DataChannel>();
    }
    QSharedPointer<qtng::DataChannel> channel(new qtng::SocketChannel(stream, qtng::PositivePole));
    return channel;
}

QSharedPointer<qtng::SocketLike> HttpTransport::makeRawSocket(const QString &address, QByteArray *connectionId)
{
    QSharedPointer<qtng::SocketLike> stream = httpConnect(session, address);
    if (stream.isNull()) {
        return QSharedPointer<qtng::SocketLike>();
    }
    *connectionId = urandom(16);
    QByteArray packet = *connectionId;
    packet.prepend("\x33\x74");
    qint64 sentBytes = stream->sendall(packet);
    if(sentBytes != packet.size()) {
        qCDebug(logger) << "handshaking is failed in client side.";
        return QSharedPointer<qtng::SocketLike>();
    }
    if (stream->recvall(2) != "\xf3\x97") {
        return QSharedPointer<qtng::SocketLike>();
    } else {
        qCDebug(logger) << "raw socket handshake finished.";
    }
    return stream;
}

QSharedPointer<qtng::SocketLike> HttpTransport::getRawSocket(const QByteArray &connectionId)
{
    const RawSocket &rawConnection = rawConnections[connectionId];
    return rawConnection.connection;
}

bool HttpTransport::canHandle(const QString &address)
{
    if (address.startsWith("https://", Qt::CaseInsensitive) || address.startsWith("http://", Qt::CaseInsensitive)) {
        return true;
    } else {
        return false;
    }
}

END_LAFRPC_NAMESPACE
