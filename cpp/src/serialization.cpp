#include <QtCore/QJsonDocument>
#include <QtCore/QJsonValue>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QDataStream>
#include <QtCore/QtEndian>
#include "../include/serialization.h"

BEGIN_LAFRPC_NAMESPACE

const QString Serialization::SpecialSidKey = "__laf_sid__";
QMap<QString, SerializableInfo> Serialization::classes;


Serialization::~Serialization()
{

}


QVariant Serialization::saveState(const QVariant &obj)
{
    QVariant::Type type = obj.type();
    if(type == QVariant::Int
            || type == QVariant::Double
            || type == QVariant::String
            || type == QVariant::Bool
            || type == QVariant::ByteArray
            || type == QVariant::LongLong
            || type == QVariant::UInt
            || type == QVariant::ULongLong
            || type == QVariant::DateTime
            || type == QVariant::Invalid) {
        return obj;
    } else if(type == QVariant::List) {
        const QVariantList &l = obj.toList();
        QVariantList result;
        for(const QVariant &e: l) {
            result.append(saveState(e));
        }
        return result;
    } else if(type == QVariant::Map) {
        const QVariantMap &d = obj.toMap();
        QVariantMap result;
        for(QVariantMap::const_iterator itor = d.constBegin(); itor != d.constEnd(); ++itor) {
            result.insert(itor.key(), saveState(itor.value()));
        }
        return result;
    } else {
        for(QMap<QString, SerializableInfo>::const_iterator itor = classes.constBegin(); itor != classes.constEnd(); ++itor) {
            const SerializableInfo &info = itor.value();
            if(info.metaTypeId == obj.userType()) {
                void *p = info.serializer->toVoid(obj);
                if(!p) {
                    return QVariant();
                }
                const QVariantMap &d = info.serializer->saveState(p);
                QVariantMap result;
                for(QVariantMap::const_iterator itor = d.constBegin(); itor != d.constEnd(); ++itor) {
                    result.insert(itor.key(), saveState(itor.value()));
                }
                result[Serialization::SpecialSidKey] = itor.key();
                return result;
            }
        }
        qDebug() << "unknown type: " << obj.type();
        throw RpcSerializationException();
    }
}

QVariant Serialization::restoreState(const QVariant &data)
{
    QVariant::Type type = data.type();
    if(type == QVariant::Int
            || type == QVariant::Double
            || type == QVariant::String
            || type == QVariant::Bool
            || type == QVariant::ByteArray
            || type == QVariant::LongLong
            || type == QVariant::UInt
            || type == QVariant::ULongLong
            || type == QVariant::DateTime
            || type == QVariant::Invalid) {
        return data;
    } else if (type == QVariant::List) {
        const QVariantList &l = data.toList();
        QVariantList result;
        for(const QVariant &e: l) {
            result.append(restoreState(e));
        }
        return result;
    } else if (type == QVariant::Map) {
        const QVariantMap &d = data.toMap();
        QVariantMap result;
        for(QVariantMap::const_iterator itor = d.constBegin(); itor != d.constEnd(); ++itor) {
            result.insert(itor.key(), restoreState(itor.value()));
        }
        if(result.contains(Serialization::SpecialSidKey)) {
            const QString &lafrpcKey = result.value(Serialization::SpecialSidKey).toString();
            if(classes.contains(lafrpcKey)) {
                const SerializableInfo &info = classes[lafrpcKey];
                void *p = info.serializer->create();
                if(info.serializer->restoreState(p, result)) {
                    return info.serializer->fromVoid(p);
                } else {
                    qDebug() << "restoreState() returns false";
                    throw RpcSerializationException();
                }
            } else {
                qDebug() << "unknown sid" << lafrpcKey;
                throw RpcSerializationException();
            }
        } else {
            return result;
        }
    } else {
        qDebug() << "unknwon type:" << data.type();
        throw RpcSerializationException();
    }
}

QVariant convertDateTime(const QVariant &obj)
{
    QVariant::Type type = obj.type();
    if(type == QVariant::Int
            || type == QVariant::Double
            || type == QVariant::String
            || type == QVariant::Bool
            || type == QVariant::ByteArray
            || type == QVariant::LongLong
            || type == QVariant::UInt
            || type == QVariant::ULongLong
            || type == QVariant::Invalid) {
        return obj;
    } else if(type == QVariant::DateTime) {
        return obj.toDateTime().toString(Qt::ISODate);
    } else if(type == QVariant::List) {
        const QVariantList &l = obj.toList();
        QVariantList result;
        foreach(const QVariant &e, l) {
            result.append(convertDateTime(e));
        }
        return result;
    } else if(type == QVariant::Map) {
        const QVariantMap &d = obj.toMap();
        QVariantMap result;
        for(QVariantMap::const_iterator itor = d.constBegin(); itor != d.constEnd(); ++itor) {
            result.insert(itor.key(), convertDateTime(itor.value()));
        }
        return result;
    } else {
        qDebug() << "json can not handle this type:" << obj.type() << obj;
        throw RpcSerializationException();
    }
}

QByteArray JsonSerialization::pack(const QVariant &obj)
{
    const QVariant &v = convertDateTime(saveState(obj));
    const QJsonValue &jv = QJsonValue::fromVariant(v);
    if(jv.isArray()) {
        QJsonDocument doc(jv.toArray());
        return doc.toJson();
    } else if(jv.isObject()) {
        QJsonDocument doc(jv.toObject());
        return doc.toJson();
    } else {
        qDebug() << "primitive type is not supported by json serialization." << obj.toString();
        throw RpcSerializationException();
    }
}

QVariant JsonSerialization::unpack(const QByteArray &data)
{
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if(error.error != QJsonParseError::NoError) {
        throw RpcSerializationException();
    } else {
        if(doc.isArray()) {
            const QVariantList &v = doc.array().toVariantList();
            return restoreState(v);
        } else if(doc.isObject()) {
            const QVariantMap &v = doc.object().toVariantMap();
            return restoreState(v);
        } else {
            qDebug() << "unknown json document type.";
            throw RpcSerializationException();
        }
    }
}


QByteArray DataStreamSerialization::pack(const QVariant &obj)
{
    QByteArray buf;
    QDataStream ds(&buf, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::BigEndian);
    ds << saveState(obj);
    if(ds.status() != QDataStream::Ok) {
        throw RpcSerializationException();
    }
    return buf;
}


QVariant DataStreamSerialization::unpack(const QByteArray &data)
{
    QDataStream ds(data);
    ds.setByteOrder(QDataStream::BigEndian);
    QVariant v;
    ds >> v;
    if(ds.status() != QDataStream::Ok) {
        throw RpcSerializationException();
    }
    return restoreState(v);
}



QByteArray MessagePackSerialization::pack(const QVariant &obj)
{
    QByteArray buf;
    qtng::MsgPackStream ds(&buf, QIODevice::WriteOnly);
    ds << saveState(obj);
    if (ds.status() != qtng::MsgPackStream::Ok) {
        throw RpcSerializationException();
    }
    return buf;
}


QVariant MessagePackSerialization::unpack(const QByteArray &data)
{
    qtng::MsgPackStream ds(data);
    QVariant v;
    ds >> v;
    if(ds.status() != qtng::MsgPackStream::Ok) {
        throw RpcSerializationException();
    }
    return restoreState(v);
}


END_LAFRPC_NAMESPACE
