#ifndef LAFRPC_BASE_H
#define LAFRPC_BASE_H

#include "qtnetworkng.h"
#include "utils.h"
#include <QtCore/qsharedpointer.h>
#include <QtCore/qstring.h>
#include <QtCore/qvariant.h>
#include <functional>

BEGIN_LAFRPC_NAMESPACE

// thrown by Peer::call()
class RpcException
{
public:
    RpcException() { }
    RpcException(const RpcException &other);
    RpcException(RpcException &&other);
    RpcException(const QString &message)
        : message(message)
    {
    }
    virtual ~RpcException();
public:
    virtual QString what() const;
    virtual void raise();
    QString message;
};

class RpcInternalException : public RpcException
{
public:
    RpcInternalException()
        : RpcException()
    {
    }
    RpcInternalException(const QString &message)
        : RpcException(message)
    {
    }
public:
    virtual QString what() const;
    virtual void raise();
};

class RpcDisconnectedException : public RpcException
{
public:
    RpcDisconnectedException()
        : RpcException()
    {
    }
    RpcDisconnectedException(const QString &message)
        : RpcException(message)
    {
    }
public:
    virtual QString what() const;
    virtual void raise();
};

class RpcRemoteException : public RpcException
{
public:
    RpcRemoteException()
        : RpcException()
    {
    }
    RpcRemoteException(const RpcRemoteException &other)
        : RpcRemoteException(other.message)
    {
    }
    RpcRemoteException(const QString &message)
        : RpcException(message)
    {
    }
public:
    virtual QString what() const override;
    virtual void raise() override;
    virtual QVariant clone();
public:
    QVariantMap saveState() const;
    bool restoreState(const QVariantMap &state);
    static QString lafrpcKey() { return "RpcRemoteException"; }
};

class RpcSerializationException : public RpcException
{
public:
    RpcSerializationException()
        : RpcException()
    {
    }
    RpcSerializationException(const QString &message)
        : RpcException(message)
    {
    }
public:
    virtual QString what() const;
    virtual void raise();
};

class Callable : public QObject
{
public:
    virtual QVariant call(const QString &methodName, const QVariantList &args, const QVariantMap &kwargs) = 0;
};

class Serialization;
class Rpc;
struct UseStream
{
    enum Place {
        ServerSide = 1,
        ClientSide = 2,
        ParamInRequest = 4,
        ValueOfResponse = 8,
    };
    Q_DECLARE_FLAGS(Places, Place)
    QSharedPointer<qtng::VirtualChannel> channel;
    QFlags<Place> place;
    bool preferRawSocket;
    QSharedPointer<qtng::SocketLike> rawSocket;
    qtng::Event ready;

    UseStream()
        : place(ServerSide | ValueOfResponse)
        , preferRawSocket(false)
    {
    }

    virtual ~UseStream() { }
};
Q_DECLARE_OPERATORS_FOR_FLAGS(UseStream::Places)

struct Request
{
    QByteArray id;
    QString methodName;
    QVariantList args;
    QVariantMap kwargs;
    QVariantMap header;
    quint32 channel;
    QByteArray rawSocket;
    bool oneway = false;

    Request()
        : channel(0)
    {
    }

    bool isOk() const { return !id.isEmpty() && !methodName.isEmpty(); }
};

struct Response
{
    QByteArray id;
    QVariant result;
    QVariant exception;
    quint32 channel;
    QByteArray rawSocket;

    Response()
        : channel(0)
    {
    }

    bool isOk() const { return !id.isEmpty(); }
};

END_LAFRPC_NAMESPACE

Q_DECLARE_METATYPE(QSharedPointer<LAFRPC_NAMESPACE::RpcRemoteException>)

#endif  // LAFRPC_BASE_H
