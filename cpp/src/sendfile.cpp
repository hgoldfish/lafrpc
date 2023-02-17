#include "../include/sendfile.h"
#include <QtCore/qbuffer.h>
#include <QtCore/qcryptographichash.h>
#include <QtCore/qdebug.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qloggingcategory.h>
#include <functional>

static Q_LOGGING_CATEGORY(logger, "lafrpc.sendfile");
using namespace qtng;
const static qint64 BLOCK_SIZE = 1024 * 32;

BEGIN_LAFRPC_NAMESPACE

class RpcFilePrivate
{
public:
    RpcFilePrivate(RpcFile *q);
public:
    bool sendfileViaChannel(QSharedPointer<FileLike> f, RpcFile::ProgressCallback progressCallback);
    bool recvfileViaChannel(QSharedPointer<FileLike> f, RpcFile::ProgressCallback progressCallback,
                            const QByteArray &header);
    bool sendfileViaRawSocket(QSharedPointer<FileLike> f, RpcFile::ProgressCallback progressCallback);
    bool recvfileViaRawSocket(QSharedPointer<FileLike> f, RpcFile::ProgressCallback progressCallback,
                              const QByteArray &header);
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
    : size(0)
    , atime(0)
    , mtime(0)
    , ctime(0)
    , q_ptr(q)
{
}

bool RpcFilePrivate::sendfileViaChannel(QSharedPointer<FileLike> f, RpcFile::ProgressCallback progressCallback)
{
    Q_Q(RpcFile);
    if (size == 0) {
        if (progressCallback)
            progressCallback(0, 0, 0);
        return true;
    }
    q->channel->setCapacity(32);

    quint64 count = 0;
    QByteArray buf(BLOCK_SIZE, Qt::Uninitialized);
    if (progressCallback) {
        while (count < size) {
            qint64 readBytes = f->read(buf.data(), qMin<qint64>(BLOCK_SIZE, static_cast<qint64>(size - count)));
            if (readBytes < 0) {
                qCWarning(logger) << "rpc file read error.";
                progressCallback(-1, count, size);
                return false;
            } else if (readBytes == 0) {
                progressCallback(-1, count, size);
                return false;
            }
            bool success = q->channel->sendPacket(buf.left(readBytes));
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
    } else {
        while (count < size) {
            qint64 readBytes = f->read(buf.data(), qMin<qint64>(BLOCK_SIZE, static_cast<qint64>(size - count)));
            if (readBytes < 0) {
                qCWarning(logger) << "rpc file read error.";
                return false;
            } else if (readBytes == 0) {
                return false;
            }
            bool success = q->channel->sendPacket(buf.left(readBytes));
            if (!success) {
                qCDebug(logger) << "rpc file send error.";
                return false;
            } else {
                count += static_cast<quint64>(readBytes);
            }
        }
    }

    q->channel->recvPacket();  // ensure all data sent.
    return true;
}

bool RpcFilePrivate::recvfileViaChannel(QSharedPointer<FileLike> f, RpcFile::ProgressCallback progressCallback,
                                        const QByteArray &header)
{
    Q_Q(RpcFile);
    if (size == 0) {
        if (progressCallback)
            progressCallback(0, 0, 0);
        return true;
    }
    q->channel->setCapacity(32);
    quint64 count = static_cast<quint64>(header.size());
    QSharedPointer<QCryptographicHash> hasher;
    const bool doHash = !hash.isEmpty();
    if (doHash) {
        hasher.reset(new QCryptographicHash (QCryptographicHash::Sha256));
    }
    if (progressCallback) {
        while (count < size) {
            const QByteArray &buf = q->channel->recvPacket();
            if (buf.isEmpty()) {
                qCWarning(logger) << "rpc file receiving error." << q->channel->errorString();
                progressCallback(-1, count, size);
                return false;
            }
            qint64 writtenBytes = f->write(buf);
            if (writtenBytes < 0) {
                qCWarning(logger) << "rpc file write error.";
                progressCallback(-1, count, size);
                return false;
            } else if (writtenBytes != buf.size()) {
                qCWarning(logger) << "rpc file write error: partial writing.";
                progressCallback(-1, count, size);
                return false;
            }
            count += static_cast<quint64>(buf.size());
            if (doHash) {
                hasher->addData(buf);
            }
            bool keepGo = progressCallback(buf.size(), count, size);
            if (!keepGo) {
                return false;
            }
        }
    } else {
        // remove the calling of progressCallback
        while (count < size) {
            const QByteArray &buf = q->channel->recvPacket();
            if (buf.isEmpty()) {
                qCWarning(logger) << "rpc file receiving error." << q->channel->errorString();
                ;
                return false;
            }
            qint64 writtenBytes = f->write(buf);
            if (writtenBytes < 0) {
                qCWarning(logger) << "rpc file write error.";
                return false;
            } else if (writtenBytes != buf.size()) {
                qCWarning(logger) << "rpc file write error: partial writing.";
                return false;
            }
            count += static_cast<quint64>(buf.size());
            if (doHash) {
                hasher->addData(buf);
            }
        }
    }

    if (doHash) {
        const QByteArray &myHash = hasher->result();
        if (myHash != hash) {
            qCDebug(logger) << "writeTo() got mismatched hash.";
            return false;
        }
    }
    // TODO set times.
    return true;
}

bool RpcFilePrivate::sendfileViaRawSocket(QSharedPointer<FileLike> f, RpcFile::ProgressCallback progressCallback)
{
    Q_Q(RpcFile);
    if (size == 0) {
        if (progressCallback)
            progressCallback(-1, 0, 0);
        return false;
    }
    if (progressCallback) {
        quint64 count = 0;
        QByteArray buf(BLOCK_SIZE, Qt::Uninitialized);
        while (count < size) {
            qint64 readBytes = f->read(buf.data(), qMin<qint64>(BLOCK_SIZE, static_cast<qint64>(size - count)));
            if (readBytes < 0) {
                qCWarning(logger) << "rpc file read error.";
                progressCallback(-1, count, size);
                return false;
            } else if (readBytes == 0) {
                progressCallback(-1, count, size);
                return false;
            }
            // TODO use send() instead of sendall() to maxium the boundrate.
            qint32 bs = q->rawSocket->sendall(buf.left(readBytes));
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
    } else {
        if (!sendfile(f, q->rawSocket, size)) {
            return false;
        }
    }

    q->rawSocket->recv(1);
    return true;
}

bool RpcFilePrivate::recvfileViaRawSocket(QSharedPointer<FileLike> f, RpcFile::ProgressCallback progressCallback,
                                          const QByteArray &header)
{
    Q_Q(RpcFile);
    if (size == 0) {
        if (progressCallback)
            progressCallback(0, 0, 0);
        return true;
    }
    quint64 count = static_cast<quint64>(header.size());
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    const bool doHash = !hash.isEmpty();
    if (progressCallback) {
        while (count < size) {
            const QByteArray &buf = q->rawSocket->recv(1024);
            if (buf.isEmpty()) {
                qCWarning(logger) << "rpc file receiving error.";
                progressCallback(-1, count, size);
                return false;
            }
            qint64 writtenBytes = f->write(buf);
            if (writtenBytes < 0) {
                qCWarning(logger) << "rpc file write error.";
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
    } else {
        if (!sendfile(q->rawSocket, f, size)) {
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
    : d_ptr(new RpcFilePrivate(this))
{
    Q_D(RpcFile);
    QFileInfo fileInfo(filePath);
    d->filePath = filePath;
    d->name = fileInfo.fileName();
    if (fileInfo.exists()) {
        d->size = static_cast<quint64>(fileInfo.size());
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        d->ctime = static_cast<quint64>(fileInfo.metadataChangeTime().toMSecsSinceEpoch());
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
    : d_ptr(new RpcFilePrivate(this))
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
    const QByteArray &hash =
            callInThread<QByteArray>([filePath]() -> QByteArray { return LAFRPC_NAMESPACE::calculateHash(filePath); });
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

bool RpcFile::writeToPath(const QString &path, RpcFile::ProgressCallback progressCallback)
{
    Q_D(RpcFile);
    QSharedPointer<QFile> f(new QFile(path));
    if (!f->open(QIODevice::WriteOnly | QIODevice::Unbuffered)) {
        if (progressCallback) {
            progressCallback(-1, 0, d->size);
        }
        return false;
    }
    return writeTo(FileLike::rawFile(f), progressCallback);
}

bool RpcFile::readFromPath(const QString &path, RpcFile::ProgressCallback progressCallback)
{
    Q_D(RpcFile);
    QSharedPointer<QFile> f(new QFile(path));
    if (!f->open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
        if (progressCallback) {
            progressCallback(-1, 0, d->size);
        }
        return false;
    }
    return readFrom(FileLike::rawFile(f), progressCallback);
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

bool RpcFile::writeTo(QSharedPointer<FileLike> f, RpcFile::ProgressCallback progressCallback)
{
    Q_D(RpcFile);
    if (!ready.tryWait()) {
        return false;
    }
    if (rawSocket.isNull()) {
        return d->recvfileViaChannel(f, progressCallback, QByteArray());
    } else {
        return d->recvfileViaRawSocket(f, progressCallback, QByteArray());
    }
}

bool RpcFile::readFrom(QSharedPointer<FileLike> f, RpcFile::ProgressCallback progressCallback)
{
    Q_D(RpcFile);
    if (!ready.tryWait()) {
        return false;
    }
    if (rawSocket.isNull()) {
        return d->sendfileViaChannel(f, progressCallback);
    } else {
        return d->sendfileViaRawSocket(f, progressCallback);
    }
}

bool RpcFile::sendall(const QByteArray &data, ProgressCallback progressCallback)
{
    Q_D(RpcFile);
    if (!ready.tryWait()) {
        return false;
    }
    if (rawSocket.isNull()) {
        return d->sendfileViaChannel(FileLike::bytes(data), progressCallback);
    } else {
        return d->sendfileViaRawSocket(FileLike::bytes(data), progressCallback);
    }
}

bool RpcFile::recvall(QByteArray &data, ProgressCallback progressCallback)
{
    Q_D(RpcFile);
    if (!ready.tryWait()) {
        return false;
    }
    if (rawSocket.isNull()) {
        return d->recvfileViaChannel(FileLike::bytes(&data), progressCallback, QByteArray());
    } else {
        return d->recvfileViaRawSocket(FileLike::bytes(&data), progressCallback, QByteArray());
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
    if (!d->hash.isEmpty()) {
        state.insert("hash", d->hash);
    }
    return state;
}

bool RpcFile::restoreState(const QVariantMap &state)
{
    Q_D(RpcFile);
    bool ok;
#define CHECKOK(valid, field)             \
  if (!valid) {                           \
    qDebug("can not restore %s.", field); \
    return false;                         \
  }
    d->name = state.value("name").toString();
    CHECKOK(!d->name.isEmpty(), "RpcFile.name");
    d->size = state.value("size").toULongLong(&ok);
    CHECKOK(ok, "RpcFile.size");
    d->atime = state.value("atime").toULongLong(&ok);
    CHECKOK(ok, "RpcFile.atime");
    d->ctime = state.value("ctime").toULongLong(&ok);
    CHECKOK(ok, "RpcFile.ctime");
    d->mtime = state.value("mtime").toULongLong(&ok);
    CHECKOK(ok, "RpcFile.mtime");
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
