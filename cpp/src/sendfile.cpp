#include <functional>
#include <QtCore/qfileinfo.h>
#include <QtCore/qdebug.h>
#include "../include/sendfile.h"

BEGIN_LAFRPC_NAMESPACE

const qint64 BLOCK_SIZE = 1024 * 32;

class RpcFilePrivate
{
public:
    RpcFilePrivate(RpcFile *q);
public:
    bool writeTo(QFile &f, RpcFile::ProgressCallback progressCallback, quint64 begin = 0);
    bool readFrom(QFile &f, RpcFile::ProgressCallback progressCallback);
    bool sendfile(QFile &f, RpcFile::ProgressCallback progressCallback);
    bool recvfile(QFile &f, RpcFile::ProgressCallback progressCallback, quint64 begin = 0);
public:
    QString name;
    quint64 size;
    quint64 atime;
    quint64 mtime;
    quint64 ctime;
private:
    RpcFile * const q_ptr;
    Q_DECLARE_PUBLIC(RpcFile)

};

RpcFilePrivate::RpcFilePrivate(RpcFile *q)
    :size(0), atime(0), mtime(0), ctime(0), q_ptr(q) {}


bool RpcFilePrivate::readFrom(QFile &f, RpcFile::ProgressCallback progressCallback)
{
    Q_Q(RpcFile);
    if (size == 0) {
        progressCallback(0, 0, 0);
        return true;
    }
    q->channel->setCapacity(32);

    quint64 count = 0;
    while(count < size) {
        const QByteArray &buf = f.read(qMin<quint64>(BLOCK_SIZE, size - count));
        if (f.error() != QFile::NoError) {
            qWarning() << "rpc file read error:" << f.errorString();
            progressCallback(-1, count, size);
            return false;
        }
        if (buf.isEmpty()) {
            progressCallback(-1, count, size);
            return false;
        }
        bool success = q->channel->sendPacket(buf);
        if (!success) {
            qDebug() << "rpc file send error.";
            progressCallback(-1, count, size);
            return false;
        } else {
            count += buf.size();
            progressCallback(buf.size(), count, size);
        }
    }
    return true;
}


bool RpcFilePrivate::writeTo(QFile &f, RpcFile::ProgressCallback progressCallback, quint64 begin)
{
    Q_Q(RpcFile);
    if (size == 0) {
        progressCallback(0, 0, 0);
        return true;
    }
    q->channel->setCapacity(32);
    quint64 count = begin;
    while(count < size) {
        const QByteArray &buf = q->channel->recvPacket();
        if (buf.isEmpty()) {
            qWarning() << "rpc file receiving error.";
            progressCallback(-1, count, size);
            return false;
        }
        qint64 bytes = f.write(buf);
        if (f.error() != QFile::NoError) {
            qWarning() << "rpc file write error:" << f.errorString();
            progressCallback(-1, count, size);
            return false;
        }
        if (bytes != buf.size()) {
            qWarning() << "rpc file write error: partial writing.";
            progressCallback(-1, count, size);
            return false;
        }
        count += buf.size();
        progressCallback(buf.size(), count, size);
    }
    // TODO set times.
    return true;
}


bool RpcFilePrivate::sendfile(QFile &f, RpcFile::ProgressCallback progressCallback)
{
    Q_Q(RpcFile);
    if (size == 0) {
        progressCallback(-1, 0, 0);
        return false;
    }
    quint64 count = 0;
    while(count < size) {
        const QByteArray &buf = f.read(qMin<qint64>(BLOCK_SIZE, size - count));
        if (f.error() != QFile::NoError) {
            qWarning() << "rpc file read error:" << f.errorString();
            progressCallback(-1, count, size);
            return false;
        }
        if (buf.isEmpty()) {
            progressCallback(-1, count, size);
            return false;
        }
        // TODO use send() instead of sendall() to maxium the boundrate.
        qint64 bs = q->rawSocket->sendall(buf);
        if (bs != buf.size()) {
            qDebug() << "rpc file send error.";
            progressCallback(-1, count, size);
            return false;
        } else {
            count += buf.size();
            progressCallback(buf.size(), count, size);
        }
    }
    return true;
}


bool RpcFilePrivate::recvfile(QFile &f, RpcFile::ProgressCallback progressCallback, quint64 begin)
{
    Q_Q(RpcFile);
    if (size == 0) {
        progressCallback(0, 0, 0);
        return true;
    }
    quint64 count = begin;
    while(count < size) {
        const QByteArray &buf = q->rawSocket->recv(1024);
        if (buf.isEmpty()) {
            qWarning() << "rpc file receiving error.";
            progressCallback(-1, count, size);
            return false;
        }
        qint64 bytes = f.write(buf);
        if (f.error() != QFile::NoError) {
            qWarning() << "rpc file write error:" << f.errorString();
            progressCallback(-1, count, size);
            return false;
        }
        if (bytes != buf.size()) {
            qWarning() << "rpc file write error: partial writing.";
            progressCallback(-1, count, size);
            return false;
        }
        count += buf.size();
        progressCallback(buf.size(), count, size);
    }
    // TODO set times.
    return true;
}


RpcFile::RpcFile(const QString &filePath)
    :d_ptr(new RpcFilePrivate(this))
{
    Q_D(RpcFile);
    QFileInfo fileInfo(filePath);
    d->name = fileInfo.baseName();
    if (fileInfo.exists()) {
        d->size = fileInfo.size();
        d->ctime = fileInfo.created().toSecsSinceEpoch();
        d->mtime = fileInfo.lastModified().toSecsSinceEpoch();
        d->atime = fileInfo.lastRead().toSecsSinceEpoch();
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

bool RpcFile::isValid() const
{
    Q_D(const RpcFile);
    return !d->name.isEmpty();
}


inline void defaultProgressCallback(qint64, quint64, quint64)
{
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


bool RpcFile::writeTo(QFile &f, RpcFile::ProgressCallback progressCallback)
{
    Q_D(RpcFile);
    waitForReady();
    if (rawSocket.isNull()) {
        return d->writeTo(f, progressCallback ? progressCallback : defaultProgressCallback, 0);
    } else {
        return d->recvfile(f, progressCallback ? progressCallback : defaultProgressCallback, 0);
    }
}


bool RpcFile::readFrom(QFile &f, RpcFile::ProgressCallback progressCallback)
{
    Q_D(RpcFile);
    waitForReady();
    if (rawSocket.isNull()) {
        return d->readFrom(f, progressCallback ? progressCallback : defaultProgressCallback);
    } else {
        return d->sendfile(f, progressCallback ? progressCallback : defaultProgressCallback);
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
    return true;
}

END_LAFRPC_NAMESPACE
