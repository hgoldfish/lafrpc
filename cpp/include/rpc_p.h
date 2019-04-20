#ifndef LAFRPC_RPC_P_H
#define LAFRPC_RPC_P_H
#include "rpc.h"

BEGIN_LAFRPC_NAMESPACE

struct PeerAndHeader
{
    QPointer<Peer> peer;
    QVariantMap header;
};


class Transport;
class TcpTransport;
class RpcPrivate
{
public:
    RpcPrivate(const QSharedPointer<Serialization> &serialization, Rpc *parent);
    ~RpcPrivate();

    bool setSslConfiguration(const qtng::SslConfiguration &config);
    bool setHttpRootDir(const QDir &rootDir);
    QList<bool> startServers(const QStringList &addresses, bool blocking);
    QList<bool> stopServers(const QStringList &addresses);
    void shutdown();
    QSharedPointer<Peer> connect(const QString &peerName);
    QSharedPointer<qtng::SocketLike> makeRawSocket(const QString &peerName, QByteArray *connectionId);
    QSharedPointer<qtng::SocketLike> getRawSocket(const QString &peerName, const QByteArray &connectionId);
    bool isConnected(const QString &peerName) const;
    bool isConnecting(const QString &peerName) const;
    QVariantMap getRpcHeader();
    QPointer<Peer> getCurrentPeer();
    QSharedPointer<Peer> preparePeer(const QSharedPointer<qtng::DataChannel> &channel, const QString &peerName, const QString &peerAddress);
    inline QSharedPointer<Transport> findTransport(const QString &address);
    void setCurrentPeerAndHeader(const QPointer<Peer> &peer, const QVariantMap &header);
    void deleteCurrentPeerAndHeader();
    void removePeer(const QString &peerName);
public:
    QString myPeerName;
    float timeout;
    QMap<QString, QSharedPointer<Peer>> peers;
    QMap<QString, QSharedPointer<qtng::Event>> waiters;
    QSharedPointer<HeaderCallback> headerCallback;
    QSharedPointer<LoggingCallback> loggingCallback;
    QSharedPointer<Serialization> serialization;
    QSharedPointer<Crypto> crypto;
    QSharedPointer<MagicCodeManager> magicCodeManager;
    QList<QSharedPointer<Transport>> transports;
    QStringList serverAddressList;
    QMap<QString, QString> knownAddresses;
    QMap<QString, QSharedPointer<qtng::Event>> connectingEvents;
    QMap<quintptr, PeerAndHeader> localStore;
    qtng::CoroutineGroup *operations;
private:
    Rpc * const q_ptr;
    Q_DECLARE_PUBLIC(Rpc)
    friend class RpcBuilder;

};

END_LAFRPC_NAMESPACE

#endif // LAFRPC_RPC_P_H
