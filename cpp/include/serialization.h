#ifndef LAFRPC_SERIALIZATION_H
#define LAFRPC_SERIALIZATION_H

#include "serialization_p.h"
#include <QtCore/qcryptographichash.h>
#include <QtCore/qmap.h>
#include <QtCore/qmetatype.h>
#include <QtCore/qsharedpointer.h>
#include <QtCore/qvariant.h>
#include <type_traits>
#include <typeinfo>

BEGIN_LAFRPC_NAMESPACE

class Serialization
{
public:
    static const QString SpecialSidKey;
    static QMap<QString, detail::SerializableInfo> classes;

    virtual ~Serialization();
public:
    virtual QByteArray pack(const QVariant &obj) = 0;
    virtual QVariant unpack(const QByteArray &data) = 0;
protected:
    QVariant saveState(const QVariant &obj);
    QVariant restoreState(const QVariant &data);
};

template<typename T>
QString registerClass()
{
    const QString &lafrpcKey = detail::Serializer<T>::lafrpcKey();
    if (Serialization::classes.contains(lafrpcKey)) {
        return lafrpcKey;
    }
    qRegisterMetaType<QSharedPointer<T>>();
    detail::SerializableInfo &info = Serialization::classes[lafrpcKey];
    info.serializer = QSharedPointer<detail::Serializer<T>>::create();
    info.metaTypeId = qMetaTypeId<QSharedPointer<T>>();
    info.name = detail::Serializer<T>::className();
    detail::registerExceptionClass<T>();
    detail::registerUseStreamClass<T>();
    return lafrpcKey;
}

template<typename T>
void unregisterClass()
{
    const QString &lafrpcKey = detail::Serializer<T>::lafrpcKey();
    Serialization::classes.remove(lafrpcKey);
}

class JsonSerialization : public Serialization
{
public:
    virtual QByteArray pack(const QVariant &obj) override;
    virtual QVariant unpack(const QByteArray &data) override;
};

class DataStreamSerialization : public Serialization
{
public:
    virtual QByteArray pack(const QVariant &obj) override;
    virtual QVariant unpack(const QByteArray &data) override;
};

class MessagePackSerialization : public Serialization
{
public:
    virtual QByteArray pack(const QVariant &obj) override;
    virtual QVariant unpack(const QByteArray &data) override;
};

END_LAFRPC_NAMESPACE

#endif  // LAFRPC_SERIALIZATION_H
