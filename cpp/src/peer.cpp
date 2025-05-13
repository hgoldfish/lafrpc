#include "../include/peer.h"
#include "../include/base.h"
#include "../include/rpc_p.h"
#include "../include/serialization.h"
#include "qtnetworkng.h"
#include <QtCore/qloggingcategory.h>
#include <QtCore/qmetaobject.h>
#include <QtCore/qscopeguard.h>

static Q_LOGGING_CATEGORY(logger, "lafrpc.peer") using namespace qtng;

// #define DEUBG_RPC_PROTOCOL

BEGIN_LAFRPC_NAMESPACE

inline QByteArray packRequest(const QSharedPointer<Serialization> &serialization, const Request &request)
{
    QVariantList l;
    l.append(QVariant::fromValue<int>(1));
    l.append(request.id);
    l.append(request.methodName);
    l.append(QVariant::fromValue<QVariantList>(request.args));
    l.append(request.kwargs);
    l.append(request.header);
    l.append(request.channel);
    l.append(request.rawSocket);
    return serialization->pack(QVariant::fromValue(l));
}

inline QByteArray packResponse(const QSharedPointer<Serialization> &serialization, const Response &response)
{
    QVariantList l;
    l.append(QVariant::fromValue<int>(2));
    l.append(response.id);
    l.append(response.result);
    l.append(response.exception);
    l.append(response.channel);
    l.append(response.rawSocket);
    return serialization->pack(QVariant::fromValue(l));
}

#define GOT_REQUEST 1
#define GOT_RESPONSE 2
#define GOT_NOTHING 3

int unpackRequestOrResponse(const QSharedPointer<Serialization> &serialization, const QByteArray &data,
                            Request *request, Response *response)
{
    QVariant v;
    try {
        v = serialization->unpack(data);
    } catch (RpcSerializationException &) {
        return GOT_NOTHING;
    }

    if (v.type() != QVariant::List) {
        return GOT_NOTHING;
    }
    const QVariantList &l = v.toList();
    bool ok;
    if (l.size() == 8) {
        if (l[0].toInt(&ok) != 1) {
#ifdef DEUBG_RPC_PROTOCOL
            qCDebug(logger) << "the first byte of request is not the number 1.";
#endif
            return GOT_NOTHING;
        }
        request->id = l[1].toByteArray();
        request->methodName = l[2].toString();
        request->args = l[3].toList();
        request->kwargs = l[4].toMap();
        request->header = l[5].toMap();
        request->channel = static_cast<quint32>(l[6].toLongLong(&ok));
        request->rawSocket = l[7].toByteArray();
        if (ok) {
            return GOT_REQUEST;
        } else {
#ifdef DEUBG_RPC_PROTOCOL
            qCDebug(logger) << "got invalid request:" << l;
#endif
            return GOT_NOTHING;
        }
    } else if (l.size() == 6) {
        if (l[0].toInt(&ok) != 2) {
#ifdef DEUBG_RPC_PROTOCOL
            qCDebug(logger) << "the first byte of request is not the number 2.";
#endif
            return GOT_NOTHING;
        }
        response->id = l[1].toByteArray();
        response->result = l[2];
        response->exception = l[3];
        response->channel = static_cast<quint32>(l[4].toLongLong(&ok));
        response->rawSocket = l[5].toByteArray();
        if (ok) {
            return GOT_RESPONSE;
        } else {
#ifdef DEUBG_RPC_PROTOCOL
            qCDebug(logger) << "got invalid response:" << l;
#endif
            return GOT_NOTHING;
        }
    }
    return GOT_NOTHING;
}

class PeerPrivate
{
public:
    typedef ValueEvent<QSharedPointer<Response>> Waiter;

    PeerPrivate(const QString &name, const QSharedPointer<DataChannel> &channel, const QPointer<Rpc> &rpc,
                Peer *parent);
    ~PeerPrivate();
    void shutdown();
    QVariant call(const QString &methodName, const QVariantList &args, const QVariantMap &kwargs);
    void handlePacket();
    void handleRequest(QSharedPointer<Request> request);
    QVariant lookupAndCall(const QString &methodName, const QVariantList &args, const QVariantMap &kwargs,
                           const QVariantMap &header);

    QMap<QByteArray, QSharedPointer<Waiter>> waiters;
    QString name;
    QString address;
    QSharedPointer<DataChannel> channel;
    QPointer<Rpc> rpc;
    CoroutineGroup *operations;
    quint64 nextRequestId;

    Q_DECLARE_PUBLIC(Peer)
    Peer * const q_ptr;

    bool broken;
};

PeerPrivate::PeerPrivate(const QString &name, const QSharedPointer<DataChannel> &channel, const QPointer<Rpc> &rpc,
                         Peer *parent)
    : name(name)
    , channel(channel)
    , rpc(rpc)
    , operations(new CoroutineGroup())
    , nextRequestId(1)
    , q_ptr(parent)
    , broken(false)
{
    operations->spawn([this] { handlePacket(); });
}

PeerPrivate::~PeerPrivate()
{
    shutdown();
    delete operations;
}

void PeerPrivate::shutdown()
{
    // THIS FUNC WOULD BE CALLED IN EVENTLOOP
    Q_Q(Peer);
    if (broken) {
        return;
    }
    broken = true;
    QSharedPointer<Response> emptyResponse(new Response());
    for (QMap<QByteArray, QSharedPointer<Waiter>>::const_iterator itor = waiters.constBegin();
         itor != waiters.constEnd(); ++itor) {
        itor.value()->send(emptyResponse);
    }
    waiters.clear();
    operations->killall();
    channel->abort();
    QPointer<Peer> self(q);
    callInEventLoopAsync([self] {
        if (self.isNull()) {
            return;
        }
        emit self->disconnected(self.data());
        if (self.isNull()) {
            return;
        }
        // xxx this statements shoud not place outside, because it delete this peer instantly.
        if (!self.data()->d_func()->rpc.isNull()) {
            self.data()->d_func()->rpc->d_func()->removePeer(self.data()->d_func()->name, self.data());
        }
    });
    // XXX do not do this.
    // emit q->disconnected(q);
    q->clearServices();
}

static void raiseRpcRemoteException(const QVariant &v)
{
    for (std::function<void(const QVariant &v)> func : detail::exceptionRaisers) {
        func(v);
    }
    QSharedPointer<RpcRemoteException> e = v.value<QSharedPointer<RpcRemoteException>>();
    if (!e.isNull()) {
        e->raise();
    }
}

static QSharedPointer<UseStream> convertUseStream(const QVariant &v)
{
    for (std::function<QSharedPointer<UseStream>(const QVariant &)> f : detail::useStreamConvertors) {
        QSharedPointer<UseStream> p = f(v);
        if (!p.isNull()) {
            return p;
        }
    }
    return QSharedPointer<UseStream>();
}

QVariant PeerPrivate::call(const QString &methodName, const QVariantList &args, const QVariantMap &kwargs)
{
    Q_Q(Peer);
    bool success;

    if (broken || rpc.isNull()) {
        throw RpcDisconnectedException(QString::fromUtf8("rpc is gone."));
    }

    QSharedPointer<UseStream> streamFromClient;
    for (const QVariant &v : args) {
        QSharedPointer<UseStream> p = convertUseStream(v);
        if (!p.isNull()) {
            if (!streamFromClient.isNull()) {
                qCWarning(logger) << "there is two use stream arguments in" << methodName;
                throw RpcInternalException();
            } else {
                streamFromClient = p;
            }
        }
    }
    for (const QVariant &v : kwargs.values()) {
        QSharedPointer<UseStream> p = convertUseStream(v);
        if (!p.isNull()) {
            if (!streamFromClient.isNull()) {
                qCWarning(logger) << "there is two use stream arguments in" << methodName;
                throw RpcInternalException();
            } else {
                streamFromClient = p;
            }
        }
    }

    Request request;
    request.id = createUuid();
    request.methodName = methodName;
    request.args = args;
    request.kwargs = kwargs;
    if (!rpc->dd_ptr->headerCallback.isNull()) {
        request.header = rpc->dd_ptr->headerCallback->make(q, methodName);
        if (broken || rpc.isNull()) {
            throw RpcDisconnectedException(QString::fromUtf8("rpc is gone."));
        }
    }

    auto clean = qScopeGuard([streamFromClient] {
        if (!streamFromClient.isNull()) {
            streamFromClient->ready.set();
        }
    });
    if (!streamFromClient.isNull()) {
        QSharedPointer<VirtualChannel> subChannelFromClient = channel->makeChannel();
        if (subChannelFromClient.isNull()) {
            throw RpcDisconnectedException(QString::fromUtf8("can not make sub channel."));
        }
        if (broken || rpc.isNull()) {
            throw RpcDisconnectedException(QString::fromUtf8("rpc is gone."));
        }
        QByteArray connectionId;
        QSharedPointer<SocketLike> rawSocket;
        if (streamFromClient->preferRawSocket) {
            rawSocket = rpc->makeRawSocket(name, connectionId);
            if (rawSocket.isNull() || connectionId.isEmpty()) {
                qCDebug(logger) << "can not make raw socket to" << name;
            }
            if (broken || rpc.isNull()) {
                throw RpcDisconnectedException(QString::fromUtf8("rpc is gone."));
            }
        }
        streamFromClient->place = UseStream::ClientSide | UseStream::ParamInRequest;
        streamFromClient->channel = subChannelFromClient;
        streamFromClient->rawSocket = rawSocket;
        request.channel = subChannelFromClient->channelNumber();
        request.rawSocket = connectionId;
    }

    QByteArray requestBytes = packRequest(rpc.data()->serialization(), request);
    if (requestBytes.isEmpty()) {
        throw RpcSerializationException(
                QString::fromUtf8("can not serialize request while calling remote method: %1").arg(methodName));
    }
    if (broken || rpc.isNull()) {
        throw RpcDisconnectedException(QString::fromUtf8("rpc is gone."));
    }

    QSharedPointer<Waiter> waiter(new Waiter());
    waiters.insert(request.id, waiter);
#ifdef DEUBG_RPC_PROTOCOL
    qCDebug(logger) << "start wait request" << request.id << methodName;
#endif

    success = channel->sendPacket(requestBytes);
    if (!success) {
        shutdown();
        throw RpcDisconnectedException(QString::fromUtf8("can not send packet."));
    }

    if (broken || rpc.isNull()) {
        throw RpcDisconnectedException(QString::fromUtf8("rpc is gone."));
    }

    clean.dismiss();
    if (!streamFromClient.isNull()) {
        streamFromClient->ready.set();
    }

    QSharedPointer<Response> response;
    try {
        response = waiter->tryWait();
        waiters.remove(request.id);
    } catch (CoroutineException &) {
        waiters.remove(request.id);
        throw;
    } catch (RpcException &) {
        waiters.remove(request.id);
        throw;
    } catch (std::exception &e) {
        waiters.remove(request.id);
        const QString &message =
                QString::fromUtf8("unknown error occurs while waiting response of remote method: `%1`").arg(methodName);
        qCWarning(logger) << message << e.what();
        throw RpcInternalException(message);
    }

    if (response.isNull() || !response->isOk()) {
        const QString &message =
                QString::fromUtf8("got empty response while waiting response of remote method: `%1`").arg(methodName);
        throw RpcDisconnectedException(message);
    }

    if (broken || rpc.isNull()) {
        throw RpcDisconnectedException(QString::fromUtf8("rpc is gone."));
    }

    if (!response->exception.isNull()) {
        raiseRpcRemoteException(response->exception);
        // the upper function do not return if success.
        qCWarning(logger) << "can not raise rpc remote exception.";
        throw RpcInternalException("unknown exception.");
    }

    QSharedPointer<UseStream> streamFromServer = convertUseStream(response->result);
    if (!streamFromServer.isNull()) {
        auto clean = qScopeGuard([streamFromServer] {
            streamFromServer->ready.set();
        });
        if (response->channel == 0) {
            qCWarning(logger) << "the response of" << methodName << "is a use-stream, but has no channel number.";
            throw RpcInternalException();
        }
        QSharedPointer<VirtualChannel> subChannelFromServer = channel->takeChannel(response->channel);
        if (subChannelFromServer.isNull()) {
            qCWarning(logger) << methodName << "returns a channel, but is gone.";
            throw RpcRemoteException();
        }
        QSharedPointer<SocketLike> rawSocket;
        if (!response->rawSocket.isEmpty()) {
            if (!streamFromServer->preferRawSocket) {
                qCWarning(logger) << "the response of" << methodName << "do not prefer raw socket, but got one.";
            }
            rawSocket = rpc->takeRawSocket(name, response->rawSocket);
            if (rawSocket.isNull()) {
                qCWarning(logger) << "the response of" << methodName
                                  << "returns a raw socket, but is gone:" << response->rawSocket;
            }
        }
        streamFromServer->place = UseStream::ClientSide | UseStream::ValueOfResponse;
        streamFromServer->channel = subChannelFromServer;
        streamFromServer->rawSocket = rawSocket;
    }
    return response->result;
}

void PeerPrivate::handlePacket()
{
    Q_Q(Peer);
    if (broken || rpc.isNull()) {
        return;
    }

    while (true) {
        QByteArray packet;
        try {
            packet = channel->recvPacket();
        } catch (CoroutineException &) {
            return shutdown();
        } catch (...) {
            qCWarning(logger) << "got unknown exception while receiving packet.";
            return shutdown();
        }

        if (packet.isEmpty()) {
            qCDebug(logger) << "channel disconnected while receiving packet:" << q->address() << channel->errorString();
            return shutdown();
        }

        if (broken || rpc.isNull()) {
            return shutdown();
        }

        QSharedPointer<Request> request(new Request());
        QSharedPointer<Response> response(new Response());
        int what = unpackRequestOrResponse(rpc->serialization(), packet, request.data(), response.data());
        if (what == GOT_REQUEST && request->isOk()) {
            operations->spawn([this, request] { handleRequest(request); });
        } else if (what == GOT_RESPONSE && response->isOk()) {
#ifdef DEUBG_RPC_PROTOCOL
            qCDebug(logger) << "got response" << response->id << "result:" << response->result;
#endif
            QSharedPointer<ValueEvent<QSharedPointer<Response>>> waiter = waiters.value(response->id);
            if (waiter.isNull()) {
                // qCDebug(logger) << "received a response from server, but waiter is gone: " << response->id;
            } else {
                waiter->send(response);
            }
        } else {
            qCDebug(logger) << "can not handle received packet." << q->address() << packet;
        }
    }
}

void PeerPrivate::handleRequest(QSharedPointer<Request> request)
{
    Q_Q(Peer);
    bool success;
    if (broken || rpc.isNull()) {
        return;
    }
    if (!rpc->dd_ptr->headerCallback.isNull()) {
        bool success = rpc->dd_ptr->headerCallback->auth(q, request->methodName, request->header);
        if (!success) {
#ifdef DEUBG_RPC_PROTOCOL
            qCDebug(logger) << "invalid packet from" << name;
#endif
            return;
        }
    }
    if (broken || rpc.isNull()) {
#ifdef DEUBG_RPC_PROTOCOL
        qCDebug(logger) << "rpc is gone where handling request.";
#endif
        return;
    }

    QSharedPointer<UseStream> streamFromClient;
    for (const QVariant &v : request->args) {
        streamFromClient = convertUseStream(v);
        if (!streamFromClient.isNull()) {
            break;
        }
    }
    if (streamFromClient.isNull()) {
        for (const QVariant &v : request->kwargs.values()) {
            streamFromClient = convertUseStream(v);
            if (!streamFromClient.isNull()) {
                break;
            }
        }
    }

    Response response;
    response.id = request->id;

    if (!streamFromClient.isNull()) {
        auto clean = qScopeGuard([streamFromClient] {
            if (!streamFromClient.isNull()) {
                streamFromClient->ready.set();
            }
        });
        if (request->channel == 0) {
            qCWarning(logger) << "the request of" << request->methodName
                              << "pass a use-stream parameter, but sent no channel.";
            QSharedPointer<RpcRemoteException> e(new RpcRemoteException("bad channel"));
            response.exception.setValue(e);
        } else {
            QSharedPointer<VirtualChannel> subChannelFromClient = channel->takeChannel(request->channel);
            if (subChannelFromClient.isNull()) {
                qCWarning(logger) << "the request of" << request->methodName << "sent a channel, but it is gone.";
                QSharedPointer<RpcRemoteException> e(new RpcRemoteException("bad channel"));
                response.exception.setValue(e);
            } else {
                QSharedPointer<SocketLike> rawSocket;
                if (!request->rawSocket.isEmpty()) {
                    rawSocket = rpc->takeRawSocket(name, request->rawSocket);
                    if (rawSocket.isNull()) {
                        qCWarning(logger) << request->methodName << "sent an use-stream raw socket, but it is gone.";
                    }
                }
                streamFromClient->place = UseStream::ServerSide | UseStream::ParamInRequest;
                streamFromClient->channel = subChannelFromClient;
                streamFromClient->rawSocket = rawSocket;
            }
        }
    }

    if (response.exception.isNull()) {
        try {
            response.result = lookupAndCall(request->methodName, request->args, request->kwargs, request->header);
        } catch (CoroutineException) {
            throw;
        } catch (RpcRemoteException &e) {
            response.exception = e.clone();
#ifdef DEUBG_RPC_PROTOCOL
            qCDebug(logger) << e.what();
#endif
        } catch (...) {
            QSharedPointer<RpcRemoteException> e(new RpcRemoteException("unknown exception caught."));
            response.exception.setValue(e);
#ifdef DEUBG_RPC_PROTOCOL
            qCDebug(logger) << "unknown exception caught while lookup and call request method:" << request->methodName
                            << request->args << request->kwargs << request->header;
#endif
        }
        if (broken || rpc.isNull()) {
            return;
        }
    }

    QSharedPointer<UseStream> streamFromServer;
    if (response.exception.isNull()) {
        streamFromServer = convertUseStream(response.result);
    }
    auto clean = qScopeGuard([streamFromServer] {
        if (!streamFromServer.isNull()) {
            streamFromServer->ready.set();
        }
    });

    if (!streamFromServer.isNull()) {
        QSharedPointer<VirtualChannel> subChannelFromServer = channel->makeChannel();
        if (broken || rpc.isNull()) {
            return;
        }
        if (subChannelFromServer.isNull()) {
            qCWarning(logger) << "can not make channel for the response of" << request->methodName;
            QSharedPointer<RpcRemoteException> e(new RpcRemoteException("bad channel"));
            response.exception.setValue(e);
        } else {
            QSharedPointer<SocketLike> rawSocket;
            QByteArray connectionId;
            if (streamFromServer->preferRawSocket) {
                rawSocket = rpc->makeRawSocket(name, connectionId);
                if (rawSocket.isNull() || connectionId.isEmpty()) {
                    qCDebug(logger) << "can not make raw socket to" << name << "for" << request->methodName;
                }
                if (broken || rpc.isNull()) {
                    return;
                }
            }
            streamFromServer->place = UseStream::ServerSide | UseStream::ValueOfResponse;
            streamFromServer->channel = subChannelFromServer;
            streamFromServer->rawSocket = rawSocket;
            response.channel = subChannelFromServer->channelNumber();
            response.rawSocket = connectionId;
        }
    }

    if (!response.exception.isNull()) {
        response.result.clear();
    }

    const QByteArray &responseBytes = packResponse(rpc.data()->serialization(), response);
    if (responseBytes.isEmpty()) {
        qCDebug(logger) << "can not serialize response.";
        return;
    }

    success = channel->sendPacket(responseBytes);
}

QByteArray removeNamespace(const QByteArray &typeName)
{
    if (typeName.isEmpty()) {
        return typeName;
    }
    int lt = typeName.indexOf('<');
    QByteArray leftPart, rightPart;
    if (lt >= 0) {
        leftPart = typeName.left(lt);
        int gt = typeName.lastIndexOf('>');
        if (gt < 0) {
            return typeName;
        }
        rightPart = typeName.mid(lt + 1, gt - lt - 1);
    } else {
        leftPart = typeName;
        rightPart = "";
    }
    int colon = leftPart.lastIndexOf("::");
    if (colon >= 0) {
        leftPart = leftPart.mid(colon + 2);
    }
    if (rightPart.isEmpty()) {
        return leftPart;
    } else {
        return leftPart + "<" + removeNamespace(rightPart) + ">";
    }
}

int metaTypeOf(const char *typeNameBytes)
{
    QByteArray typeName(typeNameBytes);
    typeName.replace(' ', "");
    int type = QMetaType::type(typeName);
    if (type) {
        return type;
    }
    removeNamespace(typeName);

    for (int i = QMetaType::User;; ++i) {
        const char *s = QMetaType::typeName(i);
        if (!s) {
            return 0;
        }
        QByteArray tempName(s);
        if (tempName.isEmpty()) {
            return 0;
        }
        typeName.replace(' ', "");
        tempName = removeNamespace(tempName);
        if (typeName == tempName) {
            return i;
        }
    }
}

// return method's arg not matched reason
QString objectMethodArgMatchedError(const QMetaMethod &found, QVariantList &args, QList<QGenericArgument> &parameters)
{
    const QList<QByteArray> &parameterTypeNames = found.parameterTypes();
    const QList<QByteArray> &parameterNames = found.parameterNames();
    QList<int> parameterTypes;
    for (int i = 0; i < found.parameterCount(); ++i) {
        parameterTypes.append(found.parameterType(i));
    }
    if (parameterTypeNames.size() != parameterNames.size()) {
        return "parameter names and types do not match.";
    }
    if (parameterTypeNames.size() != parameterTypes.size()) {
        return "parameter type ids and names do not match.";
    }
    if (args.size() != parameterTypes.size()) {
        return "the size of past args do not match the parameter count of method.";
    }
    for (int i = 0; i < parameterTypes.size(); ++i) {
        int typeId = parameterTypes.at(i);
        const QByteArray &typeName = parameterTypeNames.at(i);
        const QByteArray &parameterName = parameterNames.at(i);
        QVariant &arg = args[i];
        if (arg.isValid()) {
            if (!arg.convert(typeId)) {
                QString message = "the parameter %1 with type %2 can not accept the past argument.";
                return message.arg(QString::fromUtf8(parameterName), QString::fromUtf8(typeName));
            }
        } else {
            // xxx for null shared_pointer
            void *obj = QMetaType::create(typeId);
            arg = QVariant(typeId, obj);
        }
        parameters.append(QGenericArgument(typeName.constData(), arg.constData()));
    }
    for (int i = parameters.size(); i < 10; ++i) {
        parameters.append(QGenericArgument());
    }
    return QString();
}

QVariant objectCall(QObject *obj, const QString &methodName, const QVariantList &args, QVariantMap kwargs)
{
    Q_UNUSED(kwargs);
    const QMetaObject *metaObj = obj->metaObject();
    if (!metaObj) {
        throw RpcRemoteException("obj is not callable.");
    }
    if (args.size() > 9) {
        throw RpcRemoteException("too many arguments.");
    }

    QString lastError;
    while (true) {
        for (int i = metaObj->methodOffset(); i < metaObj->methodCount(); ++i) {
            const QMetaMethod &found = metaObj->method(i);
            if (found.name() != methodName) {
                continue;
            }
            QList<QGenericArgument> parameters; // parameters.size() == 10
            QVariantList newArg(args); // may be changed
            QString error = objectMethodArgMatchedError(found, newArg, parameters);
            if (!error.isEmpty()) {
                lastError = error;
                continue;
            }
            int rtype = metaTypeOf(found.typeName());
            if (!rtype) {
                throw RpcRemoteException(QString::fromUtf8("unknown return type: %1").arg(found.typeName()));
            }
            QVariant rvalue(rtype, QMetaType::create(rtype));
            QGenericReturnArgument rarg(found.typeName(), rvalue.data());

            found.invoke(obj, Qt::DirectConnection, rarg, parameters[0], parameters[1], parameters[2], parameters[3],
                parameters[4], parameters[5], parameters[6], parameters[7], parameters[8], parameters[9]);
            return rvalue;
        }
        metaObj = metaObj->superClass();
        if (!metaObj) {
            break;
        }
    }
    throw RpcRemoteException(lastError.isEmpty() ? methodName + " method not found." : lastError);
}

QVariant PeerPrivate::lookupAndCall(const QString &methodName, const QVariantList &args, const QVariantMap &kwargs,
                                    const QVariantMap &header)
{
    Q_Q(Peer);
    const QStringList &l = methodName.split(QChar('.'));
    if (l.size() < 1) {
        throw RpcRemoteException();
    }
    const QString &serviceName = l[0];
    const QStringList &l2 = l.mid(1);
    if (!q->getServices().contains(serviceName)) {
        throw RpcRemoteException();
    }
    const RpcService &rpcService = q->getServices().value(serviceName);

    QPointer<Rpc> rpc = this->rpc;
    rpc.data()->d_func()->setCurrentPeerAndHeader(q, header);
    Cleaner cleaner([rpc] {
        if (rpc.isNull())
            return;
        rpc.data()->d_func()->deleteCurrentPeerAndHeader();
    });
    Q_UNUSED(cleaner);

    if (!this->rpc->dd_ptr->loggingCallback.isNull()) {
        if (rpcService.type == ServiceType::FUNCTION) {
            this->rpc->dd_ptr->loggingCallback->calling(q, methodName, args, kwargs);
            try {
                const QVariant &result = rpcService.function(args, kwargs);
                this->rpc->dd_ptr->loggingCallback->success(q, methodName, args, kwargs, result);
                return result;
            } catch (...) {
                this->rpc->dd_ptr->loggingCallback->failed(q, methodName, args, kwargs);
                throw;
            }
        } else {
            if (l2.isEmpty()) {
                throw RpcRemoteException();
            }
            const QSharedPointer<Callable> &callable = qSharedPointerDynamicCast<Callable>(rpcService.instance);
            if (callable.isNull()) {
                try {
                    this->rpc->dd_ptr->loggingCallback->calling(q, methodName, args, kwargs);
                    const QVariant &result = objectCall(rpcService.instance.data(), l2[0], args, kwargs);
                    this->rpc->dd_ptr->loggingCallback->success(q, methodName, args, kwargs, result);
                    return result;
                } catch (...) {
                    this->rpc->dd_ptr->loggingCallback->failed(q, methodName, args, kwargs);
                    throw;
                }
            } else {
                try {
                    this->rpc->dd_ptr->loggingCallback->calling(q, methodName, args, kwargs);
                    const QVariant &result = callable->call(l2[0], args, kwargs);
                    this->rpc->dd_ptr->loggingCallback->success(q, methodName, args, kwargs, result);
                    return result;
                } catch (...) {
                    this->rpc->dd_ptr->loggingCallback->failed(q, methodName, args, kwargs);
                    throw;
                }
            }
        }
    } else {
        if (rpcService.type == ServiceType::FUNCTION) {
            return rpcService.function(args, kwargs);
        } else {
            if (l2.isEmpty()) {
                throw RpcRemoteException();
            }
            const QSharedPointer<Callable> &callable = qSharedPointerDynamicCast<Callable>(rpcService.instance);
            if (callable.isNull()) {
                return objectCall(rpcService.instance.data(), l2[0], args, kwargs);
            } else {
                return callable->call(l2[0], args, kwargs);
            }
        }
    }
}

Peer::Peer(const QString &name, const QSharedPointer<DataChannel> &channel, const QPointer<Rpc> &rpc)
    : d_ptr(new PeerPrivate(name, channel, rpc, this))
{
}

Peer::~Peer()
{
    delete d_ptr;
}

void Peer::shutdown()
{
    Q_D(Peer);
    d->shutdown();
}

bool Peer::isOk() const
{
    Q_D(const Peer);
    return !d->broken && !d->rpc.isNull();
}

bool Peer::isActive() const
{
    Q_D(const Peer);
    return !d->waiters.isEmpty();
}

QString Peer::name() const
{
    Q_D(const Peer);
    return d->name;
}

void Peer::setName(const QString &name)
{
    Q_D(Peer);
    d->name = name;
}

QString Peer::address() const
{
    Q_D(const Peer);
    return d->address;
}

void Peer::setAddress(const QString &address)
{
    Q_D(Peer);
    d->address = address;
}

QVariant Peer::call(const QString &method, const QVariantList &args, const QVariantMap &kwargs)
{
    Q_D(Peer);
    return d->call(method, args, kwargs);
}

QVariant Peer::call(const QString &method, const QVariant &arg1)
{
    Q_D(Peer);
    QVariantList args;
    args << arg1;
    return d->call(method, args, QVariantMap());
}

QVariant Peer::call(const QString &method, const QVariant &arg1, const QVariant &arg2)
{
    Q_D(Peer);
    QVariantList args;
    args << arg1 << arg2;
    return d->call(method, args, QVariantMap());
}

QVariant Peer::call(const QString &method, const QVariant &arg1, const QVariant &arg2, const QVariant &arg3)
{
    Q_D(Peer);
    QVariantList args;
    args << arg1 << arg2 << arg3;
    return d->call(method, args, QVariantMap());
}

QVariant Peer::call(const QString &method, const QVariant &arg1, const QVariant &arg2, const QVariant &arg3,
                    const QVariant &arg4)
{
    Q_D(Peer);
    QVariantList args;
    args << arg1 << arg2 << arg3 << arg4;
    return d->call(method, args, QVariantMap());
}

QVariant Peer::call(const QString &method, const QVariant &arg1, const QVariant &arg2, const QVariant &arg3,
                    const QVariant &arg4, const QVariant &arg5)
{
    Q_D(Peer);
    QVariantList args;
    args << arg1 << arg2 << arg3 << arg4 << arg5;
    return d->call(method, args, QVariantMap());
}

QVariant Peer::call(const QString &method, const QVariant &arg1, const QVariant &arg2, const QVariant &arg3,
                    const QVariant &arg4, const QVariant &arg5, const QVariant &arg6)
{
    Q_D(Peer);
    QVariantList args;
    args << arg1 << arg2 << arg3 << arg4 << arg5 << arg6;
    return d->call(method, args, QVariantMap());
}

QVariant Peer::call(const QString &method, const QVariant &arg1, const QVariant &arg2, const QVariant &arg3,
                    const QVariant &arg4, const QVariant &arg5, const QVariant &arg6, const QVariant &arg7)
{
    Q_D(Peer);
    QVariantList args;
    args << arg1 << arg2 << arg3 << arg4 << arg5 << arg6 << arg7;
    return d->call(method, args, QVariantMap());
}

QVariant Peer::call(const QString &method, const QVariant &arg1, const QVariant &arg2, const QVariant &arg3,
                    const QVariant &arg4, const QVariant &arg5, const QVariant &arg6, const QVariant &arg7,
                    const QVariant &arg8)
{
    Q_D(Peer);
    QVariantList args;
    args << arg1 << arg2 << arg3 << arg4 << arg5 << arg6 << arg7 << arg8;
    return d->call(method, args, QVariantMap());
}

QVariant Peer::call(const QString &method, const QVariant &arg1, const QVariant &arg2, const QVariant &arg3,
                    const QVariant &arg4, const QVariant &arg5, const QVariant &arg6, const QVariant &arg7,
                    const QVariant &arg8, const QVariant &arg9)
{
    Q_D(Peer);
    QVariantList args;
    args << arg1 << arg2 << arg3 << arg4 << arg5 << arg6 << arg7 << arg8 << arg9;
    return d->call(method, args, QVariantMap());
}

QSharedPointer<VirtualChannel> Peer::makeChannel()
{
    Q_D(Peer);
    if (!isOk()) {
        return QSharedPointer<VirtualChannel>();
    }
    return d->channel->makeChannel();
}

QSharedPointer<VirtualChannel> Peer::takeChannel(quint32 channelNumber)
{
    Q_D(Peer);
    if (!isOk()) {
        return QSharedPointer<VirtualChannel>();
    }
    return d->channel->takeChannel(channelNumber);
}

END_LAFRPC_NAMESPACE
