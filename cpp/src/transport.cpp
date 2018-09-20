#include <QtCore/QDateTime>
#include "../include/transport.h"
#include "../include/rpc.h"
#include "../include/tran_crypto.h"

BEGIN_LAFRPC_NAMESPACE


Transport::~Transport() {}


TcpTransport::TcpTransport(const QPointer<Rpc> &rpc)
    :Transport(rpc), operations(new qtng::CoroutineGroup)
{

}


TcpTransport::~TcpTransport()
{
    delete operations;
}


bool TcpTransport::makeSocket(const QString &address, QSharedPointer<qtng::SocketLike> *socket, QHostAddress *host, quint16 *port)
{
    if (!address.startsWith("tcp://")) {
        return false;
    }
    QString payload = address;
    payload.remove(0, 6); // remove tcp://
    
    int pos = payload.indexOf(QChar(':'));
    if (pos <= 0) {
        return false;
    }
    QString hostStr = payload.left(pos);
    QString portStr = payload.mid(pos + 1);
    bool ok;
    *host = QHostAddress(hostStr);
    *port = portStr.toUInt(&ok);
    if(!ok || host->isNull()) {
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
        qDebug() << "can not set address reusable option.";
        return false;
    }
    success = serverSocket->bind(host, port);
    if(!success) {
        qDebug() << "can not bind to" << host << port;
        return false;
    }
    success = serverSocket->listen(50);
    if(!success) {
        qDebug() << "can not listen.";
        return false;
    }
    while(true) {
        QSharedPointer<qtng::SocketLike> request = serverSocket->accept();
        operations->spawn([request, this] {
            this->handleRequest(request);
        });
    }
}


void TcpTransport::handleRequest(QSharedPointer<qtng::SocketLike> request)
{
    request->setOption(qtng::Socket::LowDelayOption, true);
    const QByteArray header = request->recvall(2);
    if(header == QByteArray("\x4e\x67")) {
        if(rpc.isNull()) {
            qDebug() << "rpc is gone.";
            return;
        }
        QSharedPointer<qtng::DataChannel> channel(new qtng::SocketChannel(request, qtng::NegativePole));
        QString address;
        if (canHandle("ssl://")) {
            address = QStringLiteral("ssl://%1:%2");
        } else {
            address = QStringLiteral("tcp://%1:%2");
        }
        address = address.arg(request->peerAddress().toString());
        address = address.arg(request->peerPort());
        qDebug() << "got request from:" << address;
        rpc->preparePeer(channel, QString(), address);
    } else if(header == QByteArray("\x33\x74")) {
        const QByteArray &connectionId = request->recvall(16);
        if(request->sendall("\xf3\x97") != 2) {
            qDebug() << "handshaking is failed in server side.";
            return;
        }
        qDebug() << "got raw socket:" << connectionId;
        rawConnections.insert(connectionId, RawSocket(request, QDateTime::currentDateTime()));
    } else {
        qDebug() << "invalid incoming request, does not match magic number." << header;
    }
}


QSharedPointer<qtng::DataChannel> TcpTransport::connect(const QString &address, float timeout)
{
    if(timeout == 0.0) {
        timeout = 5.0;
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
        qDebug() << "handshaking is failed in client side.";
        return QSharedPointer<qtng::DataChannel>();
    }
    QSharedPointer<qtng::DataChannel> channel(new qtng::SocketChannel(request, qtng::PositivePole));
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
        qDebug() << "raw socket handshake finished.";
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
    if (!address.startsWith("ssl://")) {
        return false;
    }
    QString payload = address;
    payload.remove(0, 6); // remove ssl://
    
    int pos = payload.indexOf(QChar(':'));
    if (pos <= 0) {
        return false;
    }
    QString hostStr = payload.left(pos);
    QString portStr = payload.mid(pos + 1);
    bool ok;
    *host = QHostAddress(hostStr);
    *port = portStr.toUInt(&ok);
    if(!ok || host->isNull()) {
        return false;
    }
    *socket = qtng::SocketLike::sslSocket(new qtng::SslSocket(qtng::Socket::AnyIPProtocol, config));
    return true;
}


bool SslTransport::canHandle(const QString &address)
{
    return address.startsWith("ssl://");
}


END_LAFRPC_NAMESPACE
