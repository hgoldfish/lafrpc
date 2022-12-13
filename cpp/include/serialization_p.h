#ifndef SERIALIZATION_P_H
#define SERIALIZATION_P_H
#include "base.h"

BEGIN_LAFRPC_NAMESPACE

namespace detail {

class BaseSerializer
{
public:
    virtual void *create() = 0;
    virtual QVariantMap saveState(void *p) = 0;
    virtual bool restoreState(void *p, const QVariantMap &state) = 0;
    virtual void *toVoid(const QVariant &v) = 0;
    virtual QVariant fromVoid(void *p) = 0;
};

template<typename T>
class Serializer : public BaseSerializer
{
public:
    virtual void *create() override { return reinterpret_cast<void *>(new T()); }

    virtual QVariantMap saveState(void *p) override { return reinterpret_cast<T *>(p)->saveState(); }

    virtual bool restoreState(void *p, const QVariantMap &state) override
    {
        return reinterpret_cast<T *>(p)->restoreState(state);
    }

    virtual void *toVoid(const QVariant &v) override
    {
        QSharedPointer<T> p = v.value<QSharedPointer<T>>();
        return reinterpret_cast<void *>(p.data());
    }

    virtual QVariant fromVoid(void *p) override
    {
        return QVariant::fromValue(QSharedPointer<T>(reinterpret_cast<T *>(p)));
    }

    static QString lafrpcKey() { return T::lafrpcKey(); }

    static QString className() { return QString::fromLatin1(typeid(T).name()); }
};

struct SerializableInfo
{
    QString name;
    int metaTypeId;
    QSharedPointer<BaseSerializer> serializer;
};

extern QList<std::function<void(const QVariant &)>> exceptionRaisers;
extern QList<std::function<QSharedPointer<UseStream>(const QVariant &)>> useStreamConvertors;

template<typename T>
void tryRaiseException(const QVariant &v)
{
    QSharedPointer<T> t = v.value<QSharedPointer<T>>();
    if (!t.isNull()) {
        t->raise();
    }
}

template<typename T>
QSharedPointer<UseStream> tryConvertUseStream(const QVariant &v)
{
    if (v.canConvert<QSharedPointer<T>>()) {
        QSharedPointer<T> p = v.value<QSharedPointer<T>>();
        if (!p.isNull()) {
            return qSharedPointerDynamicCast<UseStream>(p);
        }
    }
    return QSharedPointer<UseStream>();
}

template<typename T>
inline void registerExceptionClass(T * = 0,
                                   typename std::enable_if<std::is_base_of<RpcRemoteException, T>::value>::type * = 0)
{
    exceptionRaisers.append(tryRaiseException<T>);
}

template<typename T>
inline void
registerExceptionClass(T * = 0, typename std::enable_if<!(std::is_base_of<RpcRemoteException, T>::value)>::type * = 0)
{
}

template<typename T>
inline void registerUseStreamClass(T * = 0, typename std::enable_if<std::is_base_of<UseStream, T>::value>::type * = 0)
{
    useStreamConvertors.append(tryConvertUseStream<T>);
}

template<typename T>
inline void registerUseStreamClass(T * = 0,
                                   typename std::enable_if<!(std::is_base_of<UseStream, T>::value)>::type * = 0)
{
}

}  // namespace detail

END_LAFRPC_NAMESPACE

#endif
