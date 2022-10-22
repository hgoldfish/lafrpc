#ifndef LAFRPC_TRANSPORT_H
#define LAFRPC_TRANSPORT_H

#include <QtCore/QDateTime>
#include "qtnetworkng.h"
#include "utils.h"

BEGIN_LAFRPC_NAMESPACE


struct RawSocket
{
    RawSocket() {}
    RawSocket(QSharedPointer<qtng::SocketLike> connection, const QDateTime &timeStamp)
        :connection(connection), timeStamp(timeStamp) {}
    QSharedPointer<qtng::SocketLike> connection;
    QDateTime timeStamp;
};


class Rpc;
class Peer;
class Transport
{
public:
    explicit Transport(QPointer<Rpc> rpc);
    virtual ~Transport();
public:
    virtual QString name() const = 0;
    virtual QSharedPointer<qtng::DataChannel> connect(const QString &address);
    virtual bool startServer(const QString &address);
    virtual QSharedPointer<qtng::SocketLike> makeRawSocket(const QString &address, QByteArray &connectionId);
    virtual QSharedPointer<qtng::SocketLike> takeRawSocket(const QByteArray &connectionId);
    virtual bool canHandle(const QString &address) = 0;
    bool handleRequest(QSharedPointer<qtng::SocketLike> request, QByteArray &rpcHeader);
protected:
    virtual QSharedPointer<qtng::SocketLike> createConnection(const QString &address, const QString &host, quint16 port,
                                                              QSharedPointer<qtng::SocketDnsCache> dnsCache) = 0;
    virtual QSharedPointer<qtng::BaseStreamServer> createServer(const QString &address, const qtng::HostAddress &host, quint16 port) = 0;
    virtual QString getAddressTemplate() = 0;
    virtual bool parseAddress(const QString &address, QString &host, quint16 &port);
private:
    void setupChannel(QSharedPointer<qtng::SocketLike> request, QSharedPointer<qtng::SocketChannel> channel);
public:
    QMap<QByteArray, RawSocket> rawConnections;
    QPointer<Rpc> rpc;
};


class TcpTransport: public Transport
{
public:
    explicit TcpTransport(QPointer<Rpc> rpc)
        : Transport(rpc) {}
public:
    virtual QString name() const override;
    virtual bool canHandle(const QString &address) override;
protected:
    virtual QSharedPointer<qtng::SocketLike> createConnection(const QString &address, const QString &host, quint16 port, QSharedPointer<qtng::SocketDnsCache> dnsCache) override;
    virtual QSharedPointer<qtng::BaseStreamServer> createServer(const QString &address, const qtng::HostAddress &host, quint16 port) override;
    virtual QString getAddressTemplate() override;
private:
    friend class TcpTransportRequestHandler;
};


class SslTransport: public TcpTransport
{
public:
    explicit SslTransport(QPointer<Rpc> rpc)
        : TcpTransport(rpc) {}
public:
    virtual QString name() const override;
    virtual bool canHandle(const QString &address) override;
protected:
    virtual QSharedPointer<qtng::SocketLike> createConnection(const QString &address, const QString &host, quint16 port, QSharedPointer<qtng::SocketDnsCache> dnsCache) override;
    virtual QSharedPointer<qtng::BaseStreamServer> createServer(const QString &address, const qtng::HostAddress &host, quint16 port) override;
    virtual QString getAddressTemplate() override;
public:
    qtng::SslConfiguration sslConfig;
};


class KcpTransport: public TcpTransport
{
public:
    explicit KcpTransport(QPointer<Rpc> rpc)
        : TcpTransport(rpc) {}
public:
    virtual QString name() const override;
    virtual bool canHandle(const QString &address) override;
protected:
    virtual QSharedPointer<qtng::SocketLike> createConnection(const QString &address, const QString &host, quint16 port, QSharedPointer<qtng::SocketDnsCache> dnsCache) override;
    virtual QSharedPointer<qtng::BaseStreamServer> createServer(const QString &address, const qtng::HostAddress &host, quint16 port) override;
    virtual QString getAddressTemplate() override;
};


class KcpSslTransport: public TcpTransport
{
public:
    explicit KcpSslTransport(QPointer<Rpc> rpc)
        : TcpTransport(rpc) {}
public:
    virtual QString name() const override;
    virtual bool canHandle(const QString &address) override;
protected:
    virtual QSharedPointer<qtng::SocketLike> createConnection(const QString &address, const QString &host, quint16 port, QSharedPointer<qtng::SocketDnsCache> dnsCache) override;
    virtual QSharedPointer<qtng::BaseStreamServer> createServer(const QString &address, const qtng::HostAddress &host, quint16 port) override;
    virtual QString getAddressTemplate() override;
public:
    qtng::SslConfiguration sslConfig;
};


class HttpTransport: public Transport
{
public:
    explicit HttpTransport(QPointer<Rpc> rpc)
        : Transport(rpc), session(new qtng::HttpSession()), rootDir(QDir::current()) {}
public:
    virtual QString name() const override;
    virtual bool canHandle(const QString &address) override;
protected:
    virtual QSharedPointer<qtng::SocketLike> createConnection(const QString &address, const QString &host, quint16 port, QSharedPointer<qtng::SocketDnsCache> dnsCache) override;
    virtual QSharedPointer<qtng::BaseStreamServer> createServer(const QString &address, const qtng::HostAddress &host, quint16 port) override;
    virtual QString getAddressTemplate() override;
    virtual bool parseAddress(const QString &address, QString &host, quint16 &port) override;
public:
    QSharedPointer<qtng::HttpSession> session;
    QDir rootDir;
};


class HttpsTransport: public HttpTransport
{
public:
    explicit HttpsTransport(QPointer<Rpc> rpc)
        : HttpTransport(rpc) {}
public:
    virtual QString name() const override;
    virtual bool canHandle(const QString &address) override;
protected:
    virtual QSharedPointer<qtng::BaseStreamServer> createServer(const QString &address, const qtng::HostAddress &host, quint16 port) override;
    virtual QString getAddressTemplate() override;
    virtual bool parseAddress(const QString &address, QString &host, quint16 &port) override;
public:
    qtng::SslConfiguration sslConfig;
};


class HttpSslTransport: public HttpTransport
{
public:
    explicit HttpSslTransport(QPointer<Rpc> rpc)
        : HttpTransport(rpc) {}
public:
    virtual QString name() const override;
    virtual bool canHandle(const QString &address) override;
protected:
    virtual QSharedPointer<qtng::SocketLike> createConnection(const QString &address, const QString &host, quint16 port, QSharedPointer<qtng::SocketDnsCache> dnsCache) override;
    virtual QSharedPointer<qtng::BaseStreamServer> createServer(const QString &address, const qtng::HostAddress &host, quint16 port) override;
    virtual QString getAddressTemplate() override;
public:
    qtng::SslConfiguration sslConfig;
};

END_LAFRPC_NAMESPACE

#endif //LAFRPC_TRANSPORT_H
