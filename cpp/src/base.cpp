#include "../include/base.h"
#include "../include/rpc.h"

BEGIN_LAFRPC_NAMESPACE


QList<std::function<void(const QVariant &v)>> RpcRemoteException::exceptionHandlers;
QList<std::function<QSharedPointer<UseStream>(const QVariant &)>> UseStream::handlers;

void RpcException::raise()
{
    throw *this;
}


QString RpcException::what() const
{
    if(message.isEmpty()) {
        return QString::fromUtf8("rpc exception.");
    } else {
        return message;
    }
}


void RpcInternalException::raise()
{
    throw *this;
}


QString RpcInternalException::what() const
{
    if(message.isEmpty()) {
        return QString::fromUtf8("rpc got internal exception.");
    } else {
        return message;
    }
}


void RpcDisconnectedException::raise()
{
    throw *this;
}


QString RpcDisconnectedException::what() const
{
    if(message.isEmpty()) {
        return QString::fromUtf8("rpc is disconnected.");
    } else {
        return message;
    }
}


void RpcRemoteException::raise()
{
    throw *this;
}


QString RpcRemoteException::what() const
{
    if(message.isEmpty()) {
        return QString::fromUtf8("remote peer throw an exception.");
    } else {
        return message;
    }
}


QVariantMap RpcRemoteException::saveState() const
{
    QVariantMap state;
    state.insert("message", message);
    return state;
}


bool RpcRemoteException::restoreState(const QVariantMap &state)
{
    message = state.value("message").toString();
    return true;
}


void RpcRemoteException::raise(const QVariant &v)
{
    QSharedPointer<RpcRemoteException> e = v.value<QSharedPointer<RpcRemoteException>>();
    if (!e.isNull()) {
        e->raise();
    }
    for (std::function<void(const QVariant &v)> func: exceptionHandlers) {
        func(v);
    }
}


QVariant RpcRemoteException::clone()
{
    QSharedPointer<RpcRemoteException> e(new RpcRemoteException(message));
    return QVariant::fromValue(e);
}


void RpcSerializationException::raise()
{
    throw *this;
}


QString RpcSerializationException::what() const
{
    if(message.isEmpty()) {
        return QString::fromUtf8("can not serialize object.");
    } else {
        return message;
    }
}

UseStream::~UseStream() {}

void UseStream::setRpc(const QPointer<Rpc> &rpc)
{
    ready.reset(new qtng::Event);
    serialization = rpc->serialization();
}


QSharedPointer<UseStream> UseStream::convert(const QVariant &v)
{
    for (std::function<QSharedPointer<UseStream>(const QVariant &)> f: handlers) {
        QSharedPointer<UseStream> p = f(v);
        if (!p.isNull()) {
            return p;
        }
    }
    return QSharedPointer<UseStream>();
}


END_LAFRPC_NAMESPACE
