#include "../include/base.h"
#include "../include/rpc.h"

BEGIN_LAFRPC_NAMESPACE

RpcException::RpcException(const RpcException &other)
    : message(other.message)
{
}

RpcException::~RpcException() { }

RpcException::RpcException(RpcException &&other)
{
    qSwap(message, other.message);
}

void RpcException::raise()
{
    throw *this;
}

QString RpcException::what() const
{
    if (message.isEmpty()) {
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
    if (message.isEmpty()) {
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
    if (message.isEmpty()) {
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
    if (message.isEmpty()) {
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
    if (message.isEmpty()) {
        return QString::fromUtf8("can not serialize object.");
    } else {
        return message;
    }
}

END_LAFRPC_NAMESPACE
