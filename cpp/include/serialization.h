#ifndef LAFRPC_SERIALIZATION_H
#define LAFRPC_SERIALIZATION_H

#include <QtCore/qcryptographichash.h>
#include <QtCore/qvariant.h>
#include <QtCore/qmap.h>
#include <QtCore/qsharedpointer.h>
#include <QtCore/qmetatype.h>
#include <typeinfo>
#include <type_traits>
#include "../include/base.h"

BEGIN_LAFRPC_NAMESPACE


class BaseSerializer
{
public:
    virtual void *create() = 0;
    virtual QVariantMap saveState(void *p) = 0;
    virtual bool restoreState(void *p, const QVariantMap& state) = 0;
    virtual void *toVoid(const QVariant &v) = 0;
    virtual QVariant fromVoid(void *p) = 0;
};


template<typename T>
class Serializer: public BaseSerializer
{
public:
    virtual void *create() override
    {
        return reinterpret_cast<void*>(new T());
    }

    virtual QVariantMap saveState(void *p) override
    {
        return reinterpret_cast<T*>(p)->saveState();
    }

    virtual bool restoreState(void *p, const QVariantMap& state) override
    {
        return reinterpret_cast<T*>(p)->restoreState(state);
    }

    virtual void *toVoid(const QVariant &v) override
    {
        QSharedPointer<T> p = v.value<QSharedPointer<T>>();
        return reinterpret_cast<void*>(p.data());
    }

    virtual QVariant fromVoid(void *p) override
    {
        return QVariant::fromValue(QSharedPointer<T>(reinterpret_cast<T*>(p)));
    }

    static QString lafrpcKey()
    {
        return T::lafrpcKey();
    }

    static QString className()
    {
        return QString::fromLatin1(typeid(T).name());
    }
};


struct SerializableInfo
{
    QString name;
    int metaTypeId;
    QSharedPointer<BaseSerializer> serializer;
};


class Serialization
{
public:
    static const QString SpecialSidKey;
    static QMap<QString, SerializableInfo> classes;

    virtual ~Serialization();
public:
    virtual QByteArray pack(const QVariant &obj) = 0;
    virtual QVariant unpack(const QByteArray &data) = 0;
protected:
    QVariant saveState(const QVariant &obj);
    QVariant restoreState(const QVariant &data);
};


template<typename T>
inline void registerExceptionClass(T * = 0, typename std::enable_if<std::is_base_of<RpcRemoteException, T>::value>::type * = 0)
{
    RpcRemoteException::registerException<T>();
}

template<typename T>
inline void registerExceptionClass(T * = 0, typename std::enable_if<!(std::is_base_of<RpcRemoteException, T>::value)>::type * = 0)
{
}

template<typename T>
inline void registerUseStreamClass(T * = 0, typename std::enable_if<std::is_base_of<UseStream, T>::value>::type * = 0)
{
    UseStream::registerClass<T>();
}

template<typename T>
inline void registerUseStreamClass(T * = 0, typename std::enable_if<!(std::is_base_of<UseStream, T>::value)>::type * = 0)
{
}


template<typename T>
QString registerClass()
{
    const QString &lafrpcKey = Serializer<T>::lafrpcKey();
    if (Serialization::classes.contains(lafrpcKey)) {
        return lafrpcKey;
    }
    qRegisterMetaType<QSharedPointer<T>>();
    SerializableInfo &info = Serialization::classes[lafrpcKey];
    info.serializer = QSharedPointer<Serializer<T>>::create();
    info.metaTypeId = qMetaTypeId<QSharedPointer<T>>();
    info.name = Serializer<T>::className();
    registerExceptionClass<T>();
    registerUseStreamClass<T>();
    return lafrpcKey;
}


template<typename T>
void unregisterClass()
{
    const QString &lafrpcKey = Serializer<T>::lafrpcKey();
    Serialization::classes.remove(lafrpcKey);
}


class JsonSerialization: public Serialization
{
public:
    virtual QByteArray pack(const QVariant &obj) override;
    virtual QVariant unpack(const QByteArray &data) override;
};


class DataStreamSerialization: public Serialization
{
public:
    virtual QByteArray pack(const QVariant &obj) override;
    virtual QVariant unpack(const QByteArray &data) override;
};


class MessagePackSerialization: public Serialization
{
public:
    virtual QByteArray pack(const QVariant &obj) override;
    virtual QVariant unpack(const QByteArray &data) override;
};


END_LAFRPC_NAMESPACE

#endif // LAFRPC_SERIALIZATION_H
