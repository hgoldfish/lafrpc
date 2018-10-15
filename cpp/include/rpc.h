#ifndef LAFRPC_RPC_H
#define LAFRPC_RPC_H

#include "qtnetworkng.h"
#include "utils.h"

BEGIN_LAFRPC_NAMESPACE

class Peer;

struct AuthCallback
{
    virtual ~AuthCallback();
    virtual bool verify(const QString &itsPeerName, const QByteArray &myChallenge, const QByteArray &itsAnswer) = 0;
    virtual QByteArray answer(const QString &itsPeerName, const QByteArray &itsChallenge) = 0;
};


struct MakeKeyCallback
{
    virtual ~MakeKeyCallback();
    virtual QByteArray encrypt(const QString &itsPeerName, const QByteArray &key) = 0;
    virtual QByteArray decrypt(const QString &itsPeerName, const QByteArray &data) = 0;
};


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

class RpcPrivate;
class Serialization;
class Crypto;
class Rpc: public RegisterServiceMixin<QObject>
{
    Q_OBJECT
public:
    Rpc(const QString &myPeerName, const QSharedPointer<Serialization> &serialization);
    virtual ~Rpc();
signals:
    void newPeer(QSharedPointer<Peer> peer);
public:
    void setSslConfiguration(const qtng::SslConfiguration &config);
    float timeout() const;
    void setTimeout(float timeout);
    QString myPeerName() const;
    void setMyPeerName(const QString &name);
    QSharedPointer<Serialization> serialization() const;
    QSharedPointer<Crypto> crypto() const;
    void setHeaderCallback(QSharedPointer<HeaderCallback> headerCallback);
    void setAuthCallback(QSharedPointer<AuthCallback> authCallbak);
    void setMakeKeyCallback(QSharedPointer<MakeKeyCallback> makeKeyCallback);
    void setLoggingCallback(QSharedPointer<LoggingCallback> loggingCallback);
    QList<bool> startServers(const QStringList &addresses, bool blocking = true);
    bool startServer(const QString &address, bool blocking = true);
    QList<bool> stopServers(const QStringList &addresses = QStringList());
    bool stopServer(const QString &address);
    void shutdown();
    void close() { shutdown(); }
    QSharedPointer<qtng::SocketLike> makeRawSocket(const QString &peerName, QByteArray *connectionId);
    QSharedPointer<qtng::SocketLike> getRawSocket(const QString &peerName, const QByteArray &connectionId);
    QSharedPointer<Peer> connect(const QString &peerName);
    bool isConnected(const QString &peerName) const;
    bool isConnecting(const QString &peerAddress) const;
    QSharedPointer<Peer> get(const QString &peerName) const;
    QString address(const QString &peerName) const;
    void setAddress(const QString &peerName, const QString &peerAddress);
    QList<QSharedPointer<Peer>> getAllPeers() const;
    QPointer<Peer> getCurrentPeer();
    QVariantMap getRpcHeader();
    QSharedPointer<Peer> preparePeer(const QSharedPointer<qtng::DataChannel> &channel, const QString &name, const QString &address);    friend class Transport;
public:
    static QSharedPointer<Rpc> use(const QString &name, const QString &serialization);
private:
    Q_DECLARE_PRIVATE(Rpc)
    RpcPrivate * const d_ptr;
private:
    friend class PeerPrivate;
};

END_LAFRPC_NAMESPACE

#endif //LAFRPC_RPC_H
