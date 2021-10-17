#ifndef LAFRPC_TRANSPORT_H
#define LAFRPC_TRANSPORT_H

#include <QtCore/QDateTime>
#include "qtnetworkng.h"
#include "utils.h"

BEGIN_LAFRPC_NAMESPACE

class Rpc;
class Transport
{
public:
    Transport(QPointer<Rpc> rpc)
        :rpc(rpc) {}
    virtual ~Transport();
public:
    virtual QSharedPointer<qtng::DataChannel> connect(const QString &address, float timeout = 0.0f) = 0;
    virtual bool startServer(const QString &address) = 0;
    virtual QSharedPointer<qtng::SocketLike> makeRawSocket(const QString &address, QByteArray &connectionId) = 0;
    virtual QSharedPointer<qtng::SocketLike> takeRawSocket(const QByteArray &connectionId) = 0;
    virtual bool canHandle(const QString &address) = 0;
    virtual void setupChannel(QSharedPointer<qtng::SocketLike> request, QSharedPointer<qtng::DataChannel> channel);
public:
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
    explicit TcpTransport(QPointer<Rpc> rpc);
    virtual ~TcpTransport() override;
public:
    void handleRequest(QSharedPointer<qtng::SocketLike> request);
    virtual QSharedPointer<qtng::DataChannel> connect(const QString &address, float timeout = 0.0f) override;
    virtual bool startServer(const QString &address) override;
    virtual QSharedPointer<qtng::SocketLike> makeRawSocket(const QString &address, QByteArray &connectionId) override;
    virtual QSharedPointer<qtng::SocketLike> takeRawSocket(const QByteArray &connectionId) override;
    virtual bool canHandle(const QString &address) override;
protected:
    virtual QSharedPointer<qtng::SocketLike> createConnection(const QString &host, quint16 port, QSharedPointer<qtng::SocketDnsCache> dnsCache);
    virtual QSharedPointer<qtng::BaseStreamServer> createServer(const qtng::HostAddress &host, quint16 port);
    virtual QString getAddressTemplate();
private:
    bool parseAddress(const QString &address, QString &host, quint16 &port);
private:
    QMap<QByteArray, RawSocket> rawConnections;
    qtng::CoroutineGroup *operations;
    friend class TcpTransportRequestHandler;
};


class SslTransport: public TcpTransport
{
public:
    explicit SslTransport(QPointer<Rpc> rpc)
        : TcpTransport(rpc) {}
public:
    void setSslConfiguration(const qtng::SslConfiguration &config) {this->config = config; }
protected:
    virtual QSharedPointer<qtng::SocketLike> createConnection(const QString &host, quint16 port, QSharedPointer<qtng::SocketDnsCache> dnsCache) override;
    virtual QSharedPointer<qtng::BaseStreamServer> createServer(const qtng::HostAddress &host, quint16 port) override;
    virtual bool canHandle(const QString &address) override;
    virtual QString getAddressTemplate() override;
private:
    qtng::SslConfiguration config;
};


class KcpTransport: public TcpTransport
{
public:
    explicit KcpTransport(QPointer<Rpc> rpc)
        : TcpTransport(rpc) {}
protected:
    virtual QSharedPointer<qtng::SocketLike> createConnection(const QString &host, quint16 port, QSharedPointer<qtng::SocketDnsCache> dnsCache) override;
    virtual QSharedPointer<qtng::BaseStreamServer> createServer(const qtng::HostAddress &host, quint16 port) override;
    virtual bool canHandle(const QString &address) override;
    virtual QString getAddressTemplate() override;
};


class KcpSslTransport: public TcpTransport
{
public:
    explicit KcpSslTransport(QPointer<Rpc> rpc)
        : TcpTransport(rpc) {}
public:
    void setSslConfiguration(const qtng::SslConfiguration &config) {this->config = config; }
protected:
    virtual QSharedPointer<qtng::SocketLike> createConnection(const QString &host, quint16 port, QSharedPointer<qtng::SocketDnsCache> dnsCache) override;
    virtual QSharedPointer<qtng::BaseStreamServer> createServer(const qtng::HostAddress &host, quint16 port) override;
    virtual bool canHandle(const QString &address) override;
    virtual QString getAddressTemplate() override;
private:
    qtng::SslConfiguration config;
};


class HttpTransport: public Transport
{
public:
    explicit HttpTransport(QPointer<Rpc> rpc)
        : Transport(rpc), rootDir(QDir::current()) {}
public:
    void setSslConfiguration(const qtng::SslConfiguration &config) {this->config = config; }
    void setRootDir(const QDir &rootDir) { this->rootDir = rootDir; }
public:
    virtual QSharedPointer<qtng::DataChannel> connect(const QString &address, float timeout = 0.0f) override;
    virtual bool startServer(const QString &address) override;
    virtual QSharedPointer<qtng::SocketLike> makeRawSocket(const QString &address, QByteArray &connectionId) override;
    virtual QSharedPointer<qtng::SocketLike> takeRawSocket(const QByteArray &connectionId) override;
    virtual bool canHandle(const QString &address) override;
private:
    QMap<QByteArray, RawSocket> rawConnections;
    qtng::CoroutineGroup *operations;
    qtng::SslConfiguration config;
    qtng::HttpSession session;
    QDir rootDir;
    friend class LafrpcHttpRequestHandler;
};


END_LAFRPC_NAMESPACE

#endif //LAFRPC_TRANSPORT_H
