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


struct MagicCodeManager
{
    virtual ~MagicCodeManager();
    virtual QSharedPointer<qtng::BaseRequestHandler> create(const QByteArray &magicCode, QSharedPointer<qtng::SocketLike> request, qtng::BaseStreamServer *server);
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
    float timeout() const;
    void setTimeout(float timeout);
    quint32 maxPacketSize() const;
    void setMaxPacketSize(quint32 maxPacketSize);
    QString myPeerName() const;
    QSharedPointer<Serialization> serialization() const;
    QSharedPointer<MagicCodeManager> magicCodeManager() const;
    QSharedPointer<HeaderCallback> headerCallback() const;
    void setHeaderCallback(QSharedPointer<HeaderCallback> headerCallback);

    QList<bool> startServers(const QStringList &addresses, bool blocking = true);
    bool startServer(const QString &address, bool blocking = true);
    QList<bool> stopServers(const QStringList &addresses = QStringList());
    bool stopServer(const QString &address);
    void shutdown();

    QSharedPointer<qtng::SocketLike> makeRawSocket(const QString &peerName, QByteArray *connectionId);
    QSharedPointer<qtng::SocketLike> getRawSocket(const QString &peerName, const QByteArray &connectionId);
    QSharedPointer<Peer> connect(const QString &peerNameOrAddess);
    QSharedPointer<Peer> get(const QString &peerName) const;
    QList<QSharedPointer<Peer>> getAllPeers() const;

    bool isConnected(const QString &peerName) const;
    bool isConnecting(const QString &peerAddress) const;
    QString address(const QString &peerName) const;
    void setAddress(const QString &peerName, const QString &peerAddress);
    QPointer<Peer> getCurrentPeer();
    QVariantMap getRpcHeader();

    QSharedPointer<Peer> preparePeer(const QSharedPointer<qtng::DataChannel> &channel, const QString &name, const QString &address);    friend class Transport;
public:
    static RpcBuilder builder(SerializationType serialization);
private:
    Q_DECLARE_PRIVATE_D(dd_ptr, Rpc)
    RpcPrivate * const dd_ptr;
private:
    friend class PeerPrivate;
    friend class RpcBuilder;
};


class RpcBuilder
{
public:
    RpcBuilder(SerializationType serialization);
public:
    RpcBuilder &sslConfiguration(const qtng::SslConfiguration &sslConfiguration);
    RpcBuilder &headerCallback(QSharedPointer<HeaderCallback> headerCallback);
    RpcBuilder &loggingCallback(QSharedPointer<LoggingCallback> loggingCallback);
    RpcBuilder &magicCodeManager(QSharedPointer<MagicCodeManager> magicCodeManager);
    RpcBuilder &timeout(float timeout);
    RpcBuilder &maxPacketSize(qint32 maxPacketSize);
    RpcBuilder &myPeerName(const QString &myPeerName);
    RpcBuilder &httpRootDir(const QDir &rootDir);

    QSharedPointer<Rpc> create();
private:
    QSharedPointer<Rpc> rpc;
};

END_LAFRPC_NAMESPACE

#endif //LAFRPC_RPC_H
