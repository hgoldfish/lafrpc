#ifndef LAFRPC_UTILS_H
#define LAFRPC_UTILS_H

#include <functional>
#include <QtCore/qmap.h>
#include <QtCore/qsharedpointer.h>
#include <QtCore/quuid.h>
#include <QtCore/qbytearray.h>
#include <QtCore/qvariant.h>

#ifndef LAFRPC_NAMESPACE
#define LAFRPC_NAMESPACE laf_rpc
#endif
#define BEGIN_LAFRPC_NAMESPACE namespace LAFRPC_NAMESPACE {
#define END_LAFRPC_NAMESPACE }


BEGIN_LAFRPC_NAMESPACE

inline QByteArray createUuid()
{
    const QByteArray &id = QUuid::createUuid().toString().toLatin1();
    if(id.size() > 2) {
        return id.mid(1, id.size() - 2);
    } else {
        return id;
    }
}


inline QString createUuidAsString()
{
    const QString &id = QUuid::createUuid().toString();
    if(id.size() > 2) {
        return id.mid(1, id.size() - 2);
    } else {
        return id;
    }
}


struct Cleaner
{
    Cleaner(const std::function<void()> &del)
        :del(del) {}
    ~Cleaner() { del(); }
    std::function<void()> del;
};


typedef std::function<QVariant(const QVariantList&, const QVariantMap &)> RpcFunction;

enum ServiceType
{
    FUNCTION = 1,
    INSTANCE = 2,
};

struct RpcService
{
    QString name;
    ServiceType type;
    RpcFunction function;
    QSharedPointer<QObject> instance;
};

template<typename Base>
class RegisterServiceMixin: public Base
{
public:
    void clearServices();
    void registerFunction(const RpcFunction &function, const QString &name);
    template<typename T> void registerInstance(const QSharedPointer<T> &instance, const QString &name);
    void unregisterFunction(const QString &name);
    void unreigsterInstance(const QString &name);
    QMap<QString, RpcService> getServices();
    void setServices(const QMap<QString, RpcService> &services);
protected:
    QMap<QString, RpcService> services;
};

template<typename Base>
void RegisterServiceMixin<Base>::clearServices()
{
    services.clear();
}

template<typename Base>
void RegisterServiceMixin<Base>::registerFunction(const RpcFunction &function, const QString &name)
{
    RpcService service;
    service.name = name;
    service.type = ServiceType::FUNCTION;
    service.function = function;
    services.insert(name, service);
}

template<typename Base>
template<typename T>
void RegisterServiceMixin<Base>::registerInstance(const QSharedPointer<T> &instance, const QString &name)
{
    RpcService service;
    service.name = name;
    service.type = ServiceType::INSTANCE;
    service.instance = qSharedPointerObjectCast<QObject>(instance);
    services.insert(name, service);
}


template<typename Base>
void RegisterServiceMixin<Base>::unregisterFunction(const QString &name)
{
    services.remove(name);
}

template<typename Base>
void RegisterServiceMixin<Base>::unreigsterInstance(const QString &name)
{
    services.remove(name);
}

template<typename Base>
QMap<QString, RpcService> RegisterServiceMixin<Base>::getServices()
{
    return services;
}

template<typename Base>
void RegisterServiceMixin<Base>::setServices(const QMap<QString, RpcService> &services)
{
    this->services = services;
}

END_LAFRPC_NAMESPACE

#endif // LAFRPC_UTILS_H

