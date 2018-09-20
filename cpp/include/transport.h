#ifndef LAFRPC_TRANSPORT_H
#define LAFRPC_TRANSPORT_H

#include <QtCore/QDateTime>
#include "../qtnetworkng/qtnetworkng.h"
#include "../qtnetworkng/contrib/data_channel.h"
#include "utils.h"

BEGIN_LAFRPC_NAMESPACE

class Rpc;
class Transport
{
public:
    Transport(const QPointer<Rpc> &rpc)
        :rpc(rpc) {}
    virtual ~Transport();
public:
    virtual QSharedPointer<qtng::DataChannel> connect(const QString &address, float timeout = 0.0) = 0;
    virtual bool startServer(const QString &address) = 0;
    virtual QSharedPointer<qtng::SocketLike> makeRawSocket(const QString &address, QByteArray *connectionId) = 0;
    virtual QSharedPointer<qtng::SocketLike> getRawSocket(const QByteArray &connectionId) = 0;
    virtual bool canHandle(const QString &address) = 0;
protected:
    QPointer<Rpc> rpc;
};


struct RawSocket
{
    RawSocket() {}
    RawSocket(QSharedPointer<qtng::SocketLike> connection, const QDateTime &timeStamp)
        :connection(connection), timeStamp(timeStamp) {}
    QSharedPointer<qtng::SocketLike> connection;
    QDateTime timeStamp;
};


class TcpTransport: public Transport
{
public:
    TcpTransport(const QPointer<Rpc> &rpc);
    virtual ~TcpTransport() override;
public:
    virtual QSharedPointer<qtng::DataChannel> connect(const QString &address, float timeout = 0.0) override;
    virtual bool startServer(const QString &address) override;
    virtual QSharedPointer<qtng::SocketLike> makeRawSocket(const QString &address, QByteArray *connectionId) override;
    virtual QSharedPointer<qtng::SocketLike> getRawSocket(const QByteArray &connectionId) override;
    virtual bool canHandle(const QString &address) override;
protected:
    void handleRequest(QSharedPointer<qtng::SocketLike> request);
    virtual bool makeSocket(const QString &address, QSharedPointer<qtng::SocketLike> *socket, QHostAddress *host, quint16 *port);
private:
    QMap<QByteArray, RawSocket> rawConnections;
    qtng::CoroutineGroup *operations;
};


class SslTransport: public TcpTransport
{
public:
    SslTransport(const QPointer<Rpc> &rpc)
        : TcpTransport(rpc) {}
public:
    void setSslConfiguration(const qtng::SslConfiguration &config) {this->config = config; }
protected:
    virtual bool makeSocket(const QString &address, QSharedPointer<qtng::SocketLike> *socket, QHostAddress *host, quint16 *port) override;
    virtual bool canHandle(const QString &address) override;
private:
    qtng::SslConfiguration config;
};

END_LAFRPC_NAMESPACE

#endif //LAFRPC_TRANSPORT_H
