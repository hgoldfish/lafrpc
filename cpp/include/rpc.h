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
    virtual void calling(Peer *peer, const QString &methodName, const QVariantList &args,
                         const QVariantMap &kwargs) = 0;
    virtual void success(Peer *peer, const QString &methodName, const QVariantList &args, const QVariantMap &kwargs,
                         const QVariant &result) = 0;
    virtual void failed(Peer *peer, const QString &methodName, const QVariantList &args, const QVariantMap &kwargs) = 0;
};

struct KcpFilter
{
    virtual ~KcpFilter();
    virtual bool filter(qtng::KcpSocket *socket, char *data, qint32 *len, qtng::HostAddress *addr, quint16 *port) = 0;
};

enum SerializationType {
    MessagePack,
    Json,
    DataStream,
};

class RpcPrivate;
class Serialization;
class RpcBuilder;
class Rpc : public RegisterServiceMixin<QObject>
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
    quint32 payloadSizeHint() const;
    void setPayloadSizeHint(quint32 payloadSizeHint);
    float keepaliveTimeout() const;
    void setKeepaliveTimeout(float keepaliveTimeout);
    qtng::KcpSocket::Mode kcpMode() const;
    void setKcpMode(qtng::KcpSocket::Mode mode);
    QString myPeerName() const;
    QSharedPointer<Serialization> serialization() const;
    QSharedPointer<HeaderCallback> headerCallback() const;
    QSharedPointer<KcpFilter> kcpFilter() const;
    void setHeaderCallback(QSharedPointer<HeaderCallback> headerCallback);

    QSharedPointer<qtng::BaseStreamServer> createServer(const QString &address);
    QList<bool> startServers(const QStringList &addresses, bool blocking = true);
    bool startServer(const QString &address, bool blocking = true);
    QList<bool> stopServers(const QStringList &addresses = QStringList());
    bool stopServer(const QString &address);
    void shutdown();
    bool waitServers();

    // connect to the peer, got raw socket.
    QSharedPointer<qtng::SocketLike> makeRawSocket(const QString &peerName, QByteArray &connectionId);

    // the other peer was connected to me, so take the raw socket with the connection id.
    QSharedPointer<qtng::SocketLike> takeRawSocket(const QString &peerName, const QByteArray &connectionId);

    // connect to peer name or address, if disconnected, do reconnect.
    QSharedPointer<Peer> connect(const QString &peerNameOrAddress);

    // get peer for name, if disconnected, return nullptr.
    QSharedPointer<Peer> get(const QString &peerName) const;

    // get all peers with the peer name. some one may disconnected.
    QList<QSharedPointer<Peer>> getAll(const QString &peerName) const;

    // get all peer names, some one may disconnected.
    QStringList getAllPeerNames() const;

    // get all peers, some one may disconnected.
    QList<QSharedPointer<Peer>> getAllPeers() const;

    // is the peer connected?
    bool isConnected(const QString &peerName) const;

    // am I connecting to the peer with peer address?
    bool isConnecting(const QString &peerAddress) const;

    // get the address of peer
    QString address(const QString &peerName) const;

    // set the address of peer.
    void setAddress(const QString &peerName, const QString &peerAddress);

    // get the current peer, always using this in the service method. or return nullptr.
    QPointer<Peer> getCurrentPeer();

    // get the current rpc header. like getCurrentPeer().
    QVariantMap getRpcHeader();

    // turn a socket connection into rpc peer or raw socket.
    bool handleRequest(QSharedPointer<qtng::SocketLike> connection, const QString &address);

    // turn a data channel into rpc peer.
    QSharedPointer<Peer> preparePeer(QSharedPointer<qtng::DataChannel> channel, const QString &name,
                                     const QString &address);
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
    RpcBuilder &kcpMode(qtng::KcpSocket::Mode kcpMode);
    RpcBuilder &maxPacketSize(quint32 maxPacketSize);
    RpcBuilder &payloadSizeHint(quint32 payloadSizeHint);
    RpcBuilder &keepaliveTimeout(float keepaliveTimeout);
    RpcBuilder &myPeerName(const QString &myPeerName);
    RpcBuilder &httpRootDir(const QDir &rootDir);
    RpcBuilder &httpSession(const QSharedPointer<qtng::HttpSession> session);

    QSharedPointer<Rpc> create();
private:
    QSharedPointer<Rpc> rpc;
};

END_LAFRPC_NAMESPACE

#endif  // LAFRPC_RPC_H
