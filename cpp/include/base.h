#ifndef LAFRPC_BASE_H
#define LAFRPC_BASE_H

#include <QtCore/qstring.h>
#include <QtCore/qvariant.h>
#include <QtCore/qsharedpointer.h>
#include <functional>
#include "qtnetworkng.h"
#include "utils.h"

BEGIN_LAFRPC_NAMESPACE

// thrown by Peer::call()
class RpcException
{
public:
    RpcException() {}
    RpcException(const RpcException &other);
    RpcException(RpcException &&other);
    RpcException(const QString &message): message(message) {}
    virtual ~RpcException();
public:
    virtual QString what() const;
    virtual void raise();
    QString message;
};

class RpcInternalException: public RpcException
{
public:
    RpcInternalException(): RpcException() {}
    RpcInternalException(const QString &message): RpcException(message) {}
public:
    virtual QString what() const;
    virtual void raise();
};


class RpcDisconnectedException: public RpcException
{
public:
    RpcDisconnectedException(): RpcException() {}
    RpcDisconnectedException(const QString &message): RpcException(message) {}
public:
    virtual QString what() const;
    virtual void raise();
};


class RpcRemoteException: public RpcException
{
public:
    RpcRemoteException(): RpcException() {}
    RpcRemoteException(const RpcRemoteException &other) : RpcRemoteException(other.message) {}
    RpcRemoteException(const QString &message): RpcException(message) {}
public:
    virtual QString what() const override;
    virtual void raise() override;
public:
    // to support serialization.
    QVariantMap saveState() const;
    bool restoreState(const QVariantMap &state);
    static QString lafrpcKey() { return "RpcRemoteException"; }
public:
    // to support user-defined exception.
    virtual QVariant clone();
    static QList<std::function<void(const QVariant &v)>> exceptionHandlers;
    template<typename T> static void registerException();
    static void raise(const QVariant &v);
};

template<typename T>
void handleException(const QVariant &v)
{
    QSharedPointer<T> t = v.value<QSharedPointer<T>>();
    if (!t.isNull()) {
        t->raise();
    }
}


template<typename T>
void RpcRemoteException::registerException()
{
    exceptionHandlers.append(handleException<T>);
}

class RpcSerializationException: public RpcException
{
public:
    RpcSerializationException(): RpcException() {}
    RpcSerializationException(const QString &message): RpcException(message) {}
public:
    virtual QString what() const;
    virtual void raise();
};


class Callable: public QObject
{
public:
    virtual QVariant call(const QString &methodName, const QVariantList &args, const QVariantMap &kwargs) = 0;
};


class Serialization;
class Rpc;
struct UseStream
{
    enum Place
    {
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
    QSharedPointer<qtng::Event> ready;
    QSharedPointer<Serialization> serialization;

    UseStream()
        :place(ServerSide | ValueOfResponse), preferRawSocket(false), ready(new qtng::Event)
    {}

    virtual ~UseStream();

    void init(QPointer<Rpc> &rpc, QFlags<Place> place, QSharedPointer<qtng::VirtualChannel> &channel,
              QSharedPointer<qtng::SocketLike> rawSocket);

    void setReady()
    {
        ready->set();
    }

    void waitForReady()
    {
        ready->wait();
    }

    //support for use defined use-stream
    static QList<std::function<QSharedPointer<UseStream>(const QVariant &)>> handlers;
    static QSharedPointer<UseStream> convert(const QVariant &v);
    template<typename T> static void registerClass();
};
Q_DECLARE_OPERATORS_FOR_FLAGS(UseStream::Places)


template<typename T>
QSharedPointer<UseStream> convert_template(const QVariant &v)
{
    if (v.canConvert<QSharedPointer<T>>()) {
        QSharedPointer<T> p = v.value<QSharedPointer<T>>();
        if (!p.isNull()) {
            return qSharedPointerDynamicCast<UseStream>(p);
//            return p.dynamicCast<UseStream>();
        }
    }
    return QSharedPointer<UseStream>();
}


template<typename T>
void UseStream::registerClass()
{
    handlers.append(convert_template<T>);
}


struct Request
{
    QByteArray id;
    QString methodName;
    QVariantList args;
    QVariantMap kwargs;
    QVariantMap header;
    quint32 channel;
    QByteArray rawSocket;

    Request()
        :channel(0)
    {}

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
        :channel(0)
    {}

    bool isOk() const { return !id.isEmpty(); }
};

END_LAFRPC_NAMESPACE

Q_DECLARE_METATYPE(QSharedPointer<LAFRPC_NAMESPACE::RpcRemoteException>)

#endif // LAFRPC_BASE_H
