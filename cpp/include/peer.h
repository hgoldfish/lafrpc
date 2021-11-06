#ifndef LAFRPC_PEER_H
#define LAFRPC_PEER_H

#include <QtCore/qobject.h>
#include <QtCore/qlist.h>
#include <QtCore/qmap.h>
#include <QtCore/qvariant.h>
#include <QtCore/qsharedpointer.h>
#include "utils.h"


namespace qtng {
class DataChannel;
class VirtualChannel;
}

BEGIN_LAFRPC_NAMESPACE

class PeerPrivate;
class Rpc;
class Peer: public RegisterServiceMixin<QObject>
{
    Q_OBJECT
public:
    Peer(const QString &name, const QSharedPointer<qtng::DataChannel> &channel, const QPointer<Rpc> &rpc);
    virtual ~Peer();
public:
    void shutdown();
    void close() { shutdown(); }
    bool isOk() const;   // peer is connected.
    bool isActive() const;  // is making calls
    QString name() const;
    void setName(const QString &name);
    QString address() const;
    void setAddress(const QString &address);
    /* call() throws RpcException */
    QVariant call(const QString &method, const QVariantList &args = QVariantList(),
                  const QVariantMap &kwargs = QVariantMap());
    QVariant call(const QString &method, const QVariant &arg1);
    QVariant call(const QString &method, const QVariant &arg1, const QVariant &arg2);
    QVariant call(const QString &method, const QVariant &arg1, const QVariant &arg2,
                  const QVariant &arg3);
    QVariant call(const QString &method, const QVariant &arg1, const QVariant &arg2,
                  const QVariant &arg3, const QVariant &arg4);
    QVariant call(const QString &method, const QVariant &arg1, const QVariant &arg2,
                  const QVariant &arg3, const QVariant &arg4, const QVariant &arg5);
    QVariant call(const QString &method, const QVariant &arg1, const QVariant &arg2,
                  const QVariant &arg3, const QVariant &arg4, const QVariant &arg5,
                  const QVariant &arg6);
    QVariant call(const QString &method, const QVariant &arg1, const QVariant &arg2,
                  const QVariant &arg3, const QVariant &arg4, const QVariant &arg5,
                  const QVariant &arg6, const QVariant &arg7);
    QVariant call(const QString &method, const QVariant &arg1, const QVariant &arg2,
                  const QVariant &arg3, const QVariant &arg4, const QVariant &arg5,
                  const QVariant &arg6, const QVariant &arg7, const QVariant &arg8);
    QVariant call(const QString &method, const QVariant &arg1, const QVariant &arg2,
                  const QVariant &arg3, const QVariant &arg4, const QVariant &arg5,
                  const QVariant &arg6, const QVariant &arg7, const QVariant &arg8,
                  const QVariant &arg9);

    QSharedPointer<qtng::VirtualChannel> makeChannel();
    QSharedPointer<qtng::VirtualChannel> takeChannel(quint32 channelNumber);
signals:
    void disconnected(Peer *self);
private:
    Q_DECLARE_PRIVATE(Peer)
    PeerPrivate * const d_ptr;
};


END_LAFRPC_NAMESPACE

#endif //LAFRPC_PEER_H
