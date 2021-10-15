#ifndef LAFRPC_RPC_H
#define LAFRPC_RPC_H

#include "qtnetworkng.h"
#include "utils.h"

BEGIN_LAFRPC_NAMESPACE

class Peer;

struct HeaderCallback
{
    virtual ~HeaderCallback();
    virtual QVariantMap make(Peer *peer, const QString &methodName) = 0;
    virtual bool auth(Peer *peer, const QString &methodName, const QVariantMap &header) = 0;
};


struct LoggingCallback
{
    virtual ~LoggingCallback();
    virtual void calling(Peer *peer, const QString &methodName, const QVariantList &args, const QVariantMap &kwargs) = 0;
    virtual void success(Peer *peer, const QString &methodName, const QVariantList &args, const QVariantMap &kwargs, const QVariant &result) = 0;
    virtual void failed(Peer *peer, const QString &methodName , const QVariantList &args, const QVariantMap &kwargs) = 0;
};


struct KcpFilter
{
    virtual ~KcpFilter();
    virtual bool filter(qtng::KcpSocket *socket, char *data, qint32 *len, qtng::HostAddress *addr, quint16 *port) = 0;
};


enum SerializationType{
    MessagePack,
    Json,
    DataStream,
};


class RpcPrivate;
class Serialization;
class RpcBuilder;
class Rpc: public RegisterServiceMixin<QObject>
{
    Q_OBJECT
public:
    Rpc(const QSharedPointer<Serialization> &serialization);
    virtual ~Rpc();
signals:
    void newPeer(QSharedPointer<Peer> peer);
public:
    quint32 maxPacketSize() const;
    void setMaxPacketSize(quint32 maxPacketSize);
    QString myPeerName() const;
    QSharedPointer<Serialization> serialization() const;
    QSharedPointer<HeaderCallback> headerCallback() const;
    QSharedPointer<KcpFilter> kcpFilter() const;
    void setHeaderCallback(QSharedPointer<HeaderCallback> headerCallback);

    QList<bool> startServers(const QStringList &addresses, bool blocking = true);
    bool startServer(const QString &address, bool blocking = true);
    QList<bool> stopServers(const QStringList &addresses = QStringList());
    bool stopServer(const QString &address);
    void shutdown();

    QSharedPointer<qtng::SocketLike> makeRawSocket(const QString &peerName, QByteArray &connectionId);
    QSharedPointer<qtng::SocketLike> takeRawSocket(const QString &peerName, const QByteArray &connectionId);
    QSharedPointer<Peer> connect(const QString &peerNameOrAddress);
    QSharedPointer<Peer> get(const QString &peerName) const;
    QList<QSharedPointer<Peer>> getAll(const QString &peerName) const;
    QStringList getAllPeerNames() const;
    QList<QSharedPointer<Peer>> getAllPeers() const;

    bool isConnected(const QString &peerName) const;
    bool isConnecting(const QString &peerAddress) const;
    QString address(const QString &peerName) const;
    void setAddress(const QString &peerName, const QString &peerAddress);
    QPointer<Peer> getCurrentPeer();
    QVariantMap getRpcHeader();

    QSharedPointer<Peer> preparePeer(const QSharedPointer<qtng::DataChannel> &channel, const QString &name, const QString &address);
public:
    static RpcBuilder builder(SerializationType serialization);
private:
    Q_DECLARE_PRIVATE_D(dd_ptr, Rpc)
    RpcPrivate * const dd_ptr;
private:
    friend class PeerPrivate;
    friend class RpcBuilder;
    friend class Transport;
};


class RpcBuilder
{
public:
    RpcBuilder(SerializationType serialization);
public:
    RpcBuilder &sslConfiguration(const qtng::SslConfiguration &sslConfiguration);
    RpcBuilder &headerCallback(QSharedPointer<HeaderCallback> headerCallback);
    RpcBuilder &loggingCallback(QSharedPointer<LoggingCallback> loggingCallback);
    RpcBuilder &kcpFilter(QSharedPointer<KcpFilter> kcpFilter);
    RpcBuilder &maxPacketSize(quint32 maxPacketSize);
    RpcBuilder &myPeerName(const QString &myPeerName);
    RpcBuilder &httpRootDir(const QDir &rootDir);

    QSharedPointer<Rpc> create();
private:
    QSharedPointer<Rpc> rpc;
};

END_LAFRPC_NAMESPACE

#endif //LAFRPC_RPC_H
