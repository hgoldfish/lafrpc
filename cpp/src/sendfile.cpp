#include <functional>
#include <QtCore/qfileinfo.h>
#include <QtCore/qdebug.h>
#include <QtCore/qcryptographichash.h>
#include <QtCore/qbuffer.h>
#include <QtCore/qloggingcategory.h>
#include "../include/sendfile.h"

static Q_LOGGING_CATEGORY(logger, "lafrpc.sendfile")

BEGIN_LAFRPC_NAMESPACE

const qint64 BLOCK_SIZE = 1024 * 32;

class RpcFilePrivate
{
public:
    RpcFilePrivate(RpcFile *q);
public:
    bool sendfileViaChannel(QIODevice *f, RpcFile::ProgressCallback progressCallback);
    bool recvfileViaChannel(QIODevice *f, RpcFile::ProgressCallback progressCallback, const QByteArray &header);

    bool sendfileViaRawSocket(QIODevice *f, RpcFile::ProgressCallback progressCallback);
    bool recvfileViaRawSocket(QIODevice *f, RpcFile::ProgressCallback progressCallback, const QByteArray &header);
public:
    QString filePath;
    QString name;
    quint64 size;
    quint64 atime;
    quint64 mtime;
    quint64 ctime;
    QByteArray hash;
private:
    RpcFile * const q_ptr;
    Q_DECLARE_PUBLIC(RpcFile)

};

RpcFilePrivate::RpcFilePrivate(RpcFile *q)
    :size(0), atime(0), mtime(0), ctime(0), q_ptr(q) {}


bool RpcFilePrivate::sendfileViaChannel(QIODevice *f, RpcFile::ProgressCallback progressCallback)
{
    Q_Q(RpcFile);
    if (size == 0) {
        progressCallback(0, 0, 0);
        return true;
    }
    q->channel->setCapacity(32);

    quint64 count = 0;
    char buf[BLOCK_SIZE];
    while(count < size) {
        qint64 readBytes = f->read(buf, qMin<qint64>(BLOCK_SIZE, static_cast<qint64>(size - count)));
        if (readBytes < 0) {
            qCWarning(logger) << "rpc file read error:" << f->errorString();
            progressCallback(-1, count, size);
            return false;
        } else if (readBytes == 0) {
            progressCallback(-1, count, size);
            return false;
        }
        bool success = q->channel->sendPacket(QByteArray(buf, static_cast<int>(readBytes)));
        if (!success) {
            qCDebug(logger) << "rpc file send error.";
            progressCallback(-1, count, size);
            return false;
        } else {
            count += static_cast<quint64>(readBytes);
            bool keepGo = progressCallback(readBytes, count, size);
            if (!keepGo) {
                return false;
            }
        }
    }
    q->channel->recvPacket();  // ensure all data sent.
    return true;
}


bool RpcFilePrivate::recvfileViaChannel(QIODevice *f, RpcFile::ProgressCallback progressCallback, const QByteArray &header)
{
    Q_Q(RpcFile);
    if (size == 0) {
        progressCallback(0, 0, 0);
        return true;
    }
    q->channel->setCapacity(32);
    quint64 count = static_cast<quint64>(header.size());
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    const bool doHash = !hash.isEmpty();
    while(count < size) {
        const QByteArray &buf = q->channel->recvPacket();
        if (buf.isEmpty()) {
            qCWarning(logger) << "rpc file receiving error.";
            progressCallback(-1, count, size);
            return false;
        }
        qint64 writtenBytes = f->write(buf);
        if (writtenBytes < 0) {
            qCWarning(logger) << "rpc file write error:" << f->errorString();
            progressCallback(-1, count, size);
            return false;
        } else if (writtenBytes != buf.size()) {
            qCWarning(logger) << "rpc file write error: partial writing.";
            progressCallback(-1, count, size);
            return false;
        }
        count += static_cast<quint64>(buf.size());
        if (doHash) {
            hasher.addData(buf);
        }
        bool keepGo = progressCallback(buf.size(), count, size);
        if (!keepGo) {
            return false;
        }
    }
    if (doHash) {
        const QByteArray &myHash = hasher.result();
        if (myHash != hash) {
            qCDebug(logger) << "writeTo() got mismatched hash.";
            return false;
        }
    }
    // TODO set times.
    return true;
}


bool RpcFilePrivate::sendfileViaRawSocket(QIODevice *f, RpcFile::ProgressCallback progressCallback)
{
    Q_Q(RpcFile);
    if (size == 0) {
        progressCallback(-1, 0, 0);
        return false;
    }
    quint64 count = 0;
    char buf[BLOCK_SIZE];
    while(count < size) {
        qint64 readBytes = f->read(buf, qMin<qint64>(BLOCK_SIZE, static_cast<qint64>(size - count)));
        if (readBytes < 0) {
            qCWarning(logger) << "rpc file read error:" << f->errorString();
            progressCallback(-1, count, size);
            return false;
        } else if (readBytes == 0) {
            progressCallback(-1, count, size);
            return false;
        }
        // TODO use send() instead of sendall() to maxium the boundrate.
        qint32 bs = q->rawSocket->sendall(buf, static_cast<qint32>(readBytes));
        if (bs != readBytes) {
            qCDebug(logger) << "rpc file send error.";
            progressCallback(-1, count, size);
            return false;
        } else {
            count += static_cast<quint64>(readBytes);
            bool keepGo = progressCallback(readBytes, count, size);
            if (!keepGo) {
                return false;
            }
        }
    }
    q->rawSocket->recv(1);
    return true;
}


bool RpcFilePrivate::recvfileViaRawSocket(QIODevice *f, RpcFile::ProgressCallback progressCallback, const QByteArray &header)
{
    Q_Q(RpcFile);
    if (size == 0) {
        progressCallback(0, 0, 0);
        return true;
    }
    quint64 count = static_cast<quint64>(header.size());
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    const bool doHash = !hash.isEmpty();
    while(count < size) {
        const QByteArray &buf = q->rawSocket->recv(1024);
        if (buf.isEmpty()) {
            qCWarning(logger) << "rpc file receiving error.";
            progressCallback(-1, count, size);
            return false;
        }
        qint64 writtenBytes = f->write(buf);
        if (writtenBytes < 0) {
            qCWarning(logger) << "rpc file write error:" << f->errorString();
            progressCallback(-1, count, size);
            return false;
        } else if (writtenBytes != buf.size()) {
            qCWarning(logger) << "rpc file write error: partial writing.";
            progressCallback(-1, count, size);
            return false;
        }
        count += static_cast<quint64>(buf.size());
        if (doHash) {
            hasher.addData(buf);
        }
        bool keepGo = progressCallback(buf.size(), count, size);
        if (!keepGo) {
            return false;
        }
    }
    if (doHash) {
        const QByteArray &myHash = hasher.result();
        if (myHash != hash) {
            qCDebug(logger) << "recvfile() got mismatched hash.";
            return false;
        }
    }
    // TODO set times.
    return true;
}


RpcFile::RpcFile(const QString &filePath, bool withHash)
    :d_ptr(new RpcFilePrivate(this))
{
    Q_D(RpcFile);
    QFileInfo fileInfo(filePath);
    d->filePath = filePath;
    d->name = fileInfo.fileName();
    if (fileInfo.exists()) {
        d->size = static_cast<quint64>(fileInfo.size());
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        d->ctime = static_cast<quint64>(fileInfo.birthTime().toMSecsSinceEpoch());
#else
        d->ctime = static_cast<quint64>(fileInfo.created().toMSecsSinceEpoch());
#endif
        d->mtime = static_cast<quint64>(fileInfo.lastModified().toMSecsSinceEpoch());
        d->atime = static_cast<quint64>(fileInfo.lastRead().toMSecsSinceEpoch());
        if (withHash) {
            calculateHash();
        }
    }
}


RpcFile::RpcFile()
    :d_ptr(new RpcFilePrivate(this))
{
}


RpcFile::~RpcFile()
{
    delete d_ptr;
}


static QByteArray calculateHash(const QString &filePath)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    hasher.addData(&f);
    return hasher.result();
}


bool RpcFile::calculateHash()
{
    Q_D(RpcFile);
    if (d->filePath.isEmpty()) {
        return false;
    }
    QString filePath = d->filePath;
    const QByteArray &hash = qtng::callInThread<QByteArray>([filePath] () -> QByteArray {
        return LAFRPC_NAMESPACE::calculateHash(filePath);
    });
    if (hash.isEmpty()) {
        return false;
    } else {
        d->hash = hash;
        return true;
    }
}


bool RpcFile::isValid() const
{
    Q_D(const RpcFile);
    return !d->name.isEmpty();
}


inline bool defaultProgressCallback(qint64, quint64, quint64)
{
    return true;
}


bool RpcFile::writeToPath(const QString &path, RpcFile::ProgressCallback progressCallback)
{
    Q_D(RpcFile);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Unbuffered)) {
        if (progressCallback) {
            progressCallback(-1, 0, d->size);
        }
        return false;
    }
    return writeTo(f, progressCallback);
}


bool RpcFile::readFromPath(const QString &path, RpcFile::ProgressCallback progressCallback)
{
    Q_D(RpcFile);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
        if (progressCallback) {
            progressCallback(-1, 0, d->size);
        }
        return false;
    }
    return readFrom(f, progressCallback);
}


bool RpcFile::readFromPath(ProgressCallback progressCallback)
{
    Q_D(RpcFile);
    if (d->filePath.isEmpty()) {
        if (progressCallback) {
            progressCallback(-1, 0, d->size);
        }
        return false;
    }
    return readFromPath(d->filePath, progressCallback);
}


bool RpcFile::writeTo(QFile &f, RpcFile::ProgressCallback progressCallback)
{
    Q_D(RpcFile);
    if (!ready.wait()) {
        return false;
    }
    if (rawSocket.isNull()) {
        return d->recvfileViaChannel(&f, progressCallback ? progressCallback : defaultProgressCallback, QByteArray());
    } else {
        return d->recvfileViaRawSocket(&f, progressCallback ? progressCallback : defaultProgressCallback, QByteArray());
    }
}


bool RpcFile::readFrom(QFile &f, RpcFile::ProgressCallback progressCallback)
{
    Q_D(RpcFile);
    if (!ready.wait()) {
        return false;
    }
    if (rawSocket.isNull()) {
        return d->sendfileViaChannel(&f, progressCallback ? progressCallback : defaultProgressCallback);
    } else {
        return d->sendfileViaRawSocket(&f, progressCallback ? progressCallback : defaultProgressCallback);
    }
}


bool RpcFile::sendall(const QByteArray &data, ProgressCallback progressCallback)
{
    Q_D(RpcFile);
    if (!ready.wait()) {
        return false;
    }
    QByteArray bs = data;
    QBuffer buf(&bs);
    if (!buf.open(QIODevice::ReadOnly)) {
        if (progressCallback) {
            progressCallback(-1, 0, d->size);
        }
        return false;
    }
    if (rawSocket.isNull()) {
        return d->sendfileViaChannel(&buf, progressCallback ? progressCallback : defaultProgressCallback);
    } else {
        return d->sendfileViaRawSocket(&buf, progressCallback ? progressCallback : defaultProgressCallback);
    }
}


bool RpcFile::recvall(QByteArray &data, ProgressCallback progressCallback)
{
    Q_D(RpcFile);
    if (!ready.wait()) {
        return false;
    }
    QBuffer buf(&data);
    if (!buf.open(QIODevice::WriteOnly)) {
        if (progressCallback) {
            progressCallback(-1, 0, d->size);
        }
        return false;
    }
    if (rawSocket.isNull()) {
        return d->recvfileViaChannel(&buf, progressCallback ? progressCallback : defaultProgressCallback, QByteArray());
    } else {
        return d->recvfileViaRawSocket(&buf, progressCallback ? progressCallback : defaultProgressCallback, QByteArray());
    }
}


QVariantMap RpcFile::saveState()
{
    Q_D(const RpcFile);
    QVariantMap state;
    state.insert("name", d->name);
    state.insert("size", d->size);
    state.insert("mtime", d->mtime);
    state.insert("ctime", d->ctime);
    state.insert("atime", d->atime);
    state.insert("hash", d->hash);
    return state;
}

bool RpcFile::restoreState(const QVariantMap &state)
{
    Q_D(RpcFile);
    bool ok;
    d->name = state.value("name").toString();
    if (d->name.isEmpty()) {
        return false;
    }
    d->size = state.value("size").toULongLong(&ok);
    if (!ok) {
        return false;
    }
    d->atime = state.value("atime").toULongLong(&ok);
    if (!ok) {
        return false;
    }
    d->ctime = state.value("ctime").toULongLong(&ok);
    if (!ok) {
        return false;
    }
    d->mtime = state.value("mtime").toULongLong(&ok);
    if (!ok) {
        return false;
    }
    d->hash = state.value("hash").toByteArray();
    return true;
}


QString RpcFile::name() const
{
    Q_D(const RpcFile);
    return d->name;
}


void RpcFile::setName(const QString &name)
{
    Q_D(RpcFile);
    d->name = name;
}


quint64 RpcFile::size() const
{
    Q_D(const RpcFile);
    return d->size;
}


void RpcFile::setSize(quint64 size)
{
    Q_D(RpcFile);
    d->size = size;
}


QDateTime RpcFile::modified() const
{
    Q_D(const RpcFile);
    return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(d->mtime));
}


void RpcFile::setModified(const QDateTime &dt)
{
    Q_D(RpcFile);
    d->mtime = static_cast<quint64>(dt.toMSecsSinceEpoch());
}


QDateTime RpcFile::created() const
{
    Q_D(const RpcFile);
    return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(d->ctime));
}


void RpcFile::setCreated(const QDateTime &dt)
{
    Q_D(RpcFile);
    d->ctime = static_cast<quint64>(dt.toMSecsSinceEpoch());
}


QDateTime RpcFile::lastAccess() const
{
    Q_D(const RpcFile);
    return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(d->atime));
}


void RpcFile::setLastAccess(const QDateTime &dt)
{
    Q_D(RpcFile);
    d->atime = static_cast<quint64>(dt.toMSecsSinceEpoch());
}


QByteArray RpcFile::hash() const
{
    Q_D(const RpcFile);
    return d->hash;
}


void RpcFile::setHash(const QByteArray &hash)
{
    Q_D(RpcFile);
    d->hash = hash;
}


END_LAFRPC_NAMESPACE
