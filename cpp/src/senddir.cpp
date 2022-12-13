#include "../include/senddir.h"
#include <QtCore/qdebug.h>

using namespace qtng;

BEGIN_LAFRPC_NAMESPACE

RpcDirFileEntry::RpcDirFileEntry()
    : size(0)
    , isdir(false)
{
}

RpcDirFileProvider::~RpcDirFileProvider() { }

bool RpcDirFileProvider::createDirectory(const QString &)
{
    return false;
}

bool RpcDirFileProvider::updateTimes(const QString &, const QDateTime &, const QDateTime &, const QDateTime &)
{
    return false;
}

QString NativeRpcDirFileProvider::makePath(const QString &filePath)
{
    if (!rootDir.isReadable()) {
        return QString();
    }
    if (filePath.isEmpty()) {
        return QString();
    }
    if (filePath.startsWith("/")) {
        qWarning("file path should not start with /.");
        return QString();
    }
    const QString &fullFilePath = rootDir.filePath(filePath);
    if (!fullFilePath.startsWith(rootDir.path())) {
        qDebug() << "file path is not so secure." << rootDir.path() << filePath << fullFilePath;
        return QString();
    }
    return fullFilePath;
}

QSharedPointer<FileLike> NativeRpcDirFileProvider::getFile(const QString &filePath, QIODevice::OpenMode mode)
{
    const QString fullFilePath = makePath(filePath);
    if (fullFilePath.isEmpty()) {
        return QSharedPointer<FileLike>();
    }
    QSharedPointer<QFile> file(new QFile(fullFilePath));
    if (!file->open(mode)) {
        qDebug() << "can not open file:" << fullFilePath;
        return QSharedPointer<FileLike>();
    }
    return FileLike::rawFile(file);
}

bool NativeRpcDirFileProvider::createDirectory(const QString &dirPath)
{
    const QString fullDirPath = makePath(dirPath);
    if (fullDirPath.isEmpty()) {
        return false;
    }
    return rootDir.mkpath(dirPath);
}

bool NativeRpcDirFileProvider::updateTimes(const QString &filePath, const QDateTime &created,
                                           const QDateTime &lastModified, const QDateTime &lastAccess)
{
    const QString fullFilePath = makePath(filePath);
    if (fullFilePath.isEmpty()) {
        return false;
    }
    if (created.isValid()) {
        // TODO update created time.
    }
    if (lastModified.isValid()) {
        // TODO update last modified time.
    }
    if (lastAccess.isValid()) {
        // TODO update last access time.
    }
    return true;
}

class RpcDirPrivate
{
public:
    RpcDirPrivate(RpcDir *q);
public:
    bool writeTo(QSharedPointer<RpcDirFileProvider> provider, RpcDir::ProgressCallback progressCallback);
    bool readFrom(QSharedPointer<RpcDirFileProvider> provider, RpcDir::ProgressCallback progressCallback);
public:
    QString name;
    QString dirPath;
    quint64 size;
    QDateTime created;
    QDateTime lastModified;
    QDateTime lastAccess;
    QList<RpcDirFileEntry> entries;
private:
    RpcDir * const q_ptr;
    Q_DECLARE_PUBLIC(RpcDir)
};

RpcDirPrivate::RpcDirPrivate(RpcDir *q)
    : size(0)
    , q_ptr(q)
{
}

bool RpcDirPrivate::writeTo(QSharedPointer<RpcDirFileProvider> provider, RpcDir::ProgressCallback progressCallback)
{
    Q_Q(RpcDir);
    quint64 totalWritten = 0;
    for (const RpcDirFileEntry &entry : entries) {
        if (entry.isdir) {
            if (!provider->createDirectory(entry.path)) {
                if (progressCallback)
                    progressCallback(CallbackInfo(entry.path, -1, 0, 0, totalWritten, this->size));
                return false;
            } else {
                if (progressCallback)
                    progressCallback(CallbackInfo(entry.path, 0, 0, 0, totalWritten, this->size));
            }
        } else if (entry.size == 0) {
            QSharedPointer<FileLike> file = provider->getFile(entry.path, QIODevice::WriteOnly);
            if (file.isNull()) {
                if (progressCallback)
                    progressCallback(CallbackInfo(entry.path, -1, 0, 0, totalWritten, this->size));
                return false;
            } else {
                if (progressCallback)
                    progressCallback(CallbackInfo(entry.path, 0, 0, 0, totalWritten, this->size));
            }
        } else {
            QSharedPointer<FileLike> file = provider->getFile(entry.path, QIODevice::WriteOnly);
            if (file.isNull()) {
                if (progressCallback)
                    progressCallback(CallbackInfo(entry.path, -1, 0, entry.size, totalWritten, this->size));
                return false;
            }
            QSharedPointer<DataChannel> channel = q->channel->makeChannel();
            if (channel.isNull()) {
                if (progressCallback)
                    progressCallback(CallbackInfo(entry.path, -1, 0, entry.size, totalWritten, this->size));
                return false;
            }

            quint64 fileWritten = 0;
            while (fileWritten < entry.size) {
                const QByteArray &buf = channel->recvPacket();
                if (buf.isEmpty()) {
                    if (progressCallback)
                        progressCallback(
                                CallbackInfo(entry.path, -1, fileWritten, entry.size, totalWritten, this->size));
                    return false;
                }
                if (file->write(buf.data(), buf.size()) != buf.size()) {
                    if (progressCallback)
                        progressCallback(
                                CallbackInfo(entry.path, -1, fileWritten, entry.size, totalWritten, this->size));
                    return false;
                }
                fileWritten += static_cast<quint64>(buf.size());
                totalWritten += static_cast<quint64>(buf.size());
                if (progressCallback) {
                    bool keepGoing = progressCallback(
                            CallbackInfo(entry.path, buf.size(), fileWritten, entry.size, totalWritten, this->size));
                    if (!keepGoing) {
                        return true;
                    }
                }
            }
            if (fileWritten > entry.size) {
                if (progressCallback)
                    progressCallback(CallbackInfo(entry.path, -1, fileWritten, entry.size, totalWritten, this->size));
                return false;
            } else {
                bool ok = provider->updateTimes(entry.path, entry.created, entry.lastModified, entry.lastAccess);
                if (!ok) {
                    if (progressCallback)
                        progressCallback(
                                CallbackInfo(entry.path, -1, fileWritten, entry.size, totalWritten, this->size));
                    return false;
                }
            }
        }
    }
    return true;
}

bool RpcDirPrivate::readFrom(QSharedPointer<RpcDirFileProvider> provider, RpcDir::ProgressCallback progressCallback)
{
    Q_Q(RpcDir);
    if (!q->ready.wait()) {
        return false;
    }
    quint64 totalRead = 0;
    QByteArray buf(1024 * 64, Qt::Uninitialized);
    for (const RpcDirFileEntry &entry : entries) {
        if (entry.isdir || entry.size == 0) {
            if (progressCallback)
                progressCallback(CallbackInfo(entry.path, 0, 0, 0, totalRead, this->size));
        } else {
            QSharedPointer<FileLike> file = provider->getFile(entry.path, QIODevice::ReadOnly);
            if (file.isNull()) {
                if (progressCallback)
                    progressCallback(CallbackInfo(entry.path, -1, 0, entry.size, totalRead, this->size));
                return false;
            }
            QSharedPointer<DataChannel> channel = q->channel->takeChannel();
            if (channel.isNull()) {
                if (progressCallback)
                    progressCallback(CallbackInfo("", -1, 0, 0, totalRead, this->size));
                return false;
            }
            quint64 fileRead = 0;
            qint32 blockSize = static_cast<qint32>(channel->payloadSizeHint());
            while (fileRead < entry.size) {
                qint32 bs = file->read(buf.data(), blockSize);
                if (bs <= 0) {
                    if (progressCallback)
                        progressCallback(CallbackInfo(entry.path, -1, fileRead, entry.size, totalRead, this->size));
                    return false;
                }
                if (!channel->sendPacket(QByteArray(buf.data(), bs))) {
                    if (progressCallback)
                        progressCallback(CallbackInfo(entry.path, -1, fileRead, entry.size, totalRead, this->size));
                    return false;
                }
                fileRead += static_cast<quint64>(bs);
                totalRead += static_cast<quint64>(bs);
                if (progressCallback) {
                    bool keepGoing =
                            progressCallback(CallbackInfo(entry.path, bs, fileRead, entry.size, totalRead, this->size));
                    if (!keepGoing) {
                        return true;
                    }
                }
            }
            channel->recvPacket();  // wait for remote closing the channel.
        }
    }

    return true;
}

RpcDir::RpcDir(const QString &path)
    : d_ptr(new RpcDirPrivate(this))
{
    Q_D(RpcDir);
    QFileInfo dirInfo(path);
    d->dirPath = path;
    d->name = dirInfo.fileName();
    if (dirInfo.exists()) {
        d->size = static_cast<quint64>(dirInfo.size());
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        d->created = dirInfo.metadataChangeTime();
#else
        d->created = dirInfo.created();
#endif
        d->lastModified = dirInfo.lastModified();
        d->lastAccess = dirInfo.lastRead();
        populate();
    }
}

RpcDir::RpcDir()
    : d_ptr(new RpcDirPrivate(this))
{
}

RpcDir::~RpcDir()
{
    delete d_ptr;
}

struct PopulateResult
{
    PopulateResult()
        : totalSize(0)
    {
    }
    QList<RpcDirFileEntry> entries;
    quint64 totalSize;
};

static void _populate(const QDir &dir, const QString &relativePath, PopulateResult &result)
{
    QDir::Filters filters =
            QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Readable | QDir::Executable | QDir::Hidden;
    for (const QFileInfo &fileInfo : dir.entryInfoList(filters, QDir::DirsFirst)) {
        RpcDirFileEntry entry;
        const QString &name = fileInfo.fileName();
        if (Q_UNLIKELY(name.contains("/"))) {
            continue;
        }
        entry.path = relativePath.isEmpty() ? name : relativePath + "/" + name;
        entry.size = static_cast<quint64>(fileInfo.size());
        entry.isdir = fileInfo.isDir();
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        entry.created = fileInfo.metadataChangeTime();
#else
        entry.created = fileInfo.created();
#endif
        entry.lastModified = fileInfo.lastModified();
        entry.lastAccess = fileInfo.lastRead();
        result.entries.append(entry);
        result.totalSize += entry.size;
        if (fileInfo.isDir()) {
            _populate(QDir(fileInfo.path()), entry.path, result);
        }
    }
}

static PopulateResult populate(const QString &dirPath)
{
    QList<RpcDirFileEntry> entries;
    QDir rootDir(dirPath);
    PopulateResult result;
    _populate(rootDir, QString(), result);
    return result;
}

bool RpcDir::populate()
{
    Q_D(RpcDir);
    if (d->dirPath.isEmpty() || !QDir(d->dirPath).isReadable()) {
        return false;
    }
    QString dirPath;
    PopulateResult result =
            qtng::callInThread<PopulateResult>([dirPath] { return LAFRPC_NAMESPACE::populate(dirPath); });
    d->entries = result.entries;
    d->size = result.totalSize;
    return true;
}

bool RpcDir::isValid() const
{
    Q_D(const RpcDir);
    return !d->name.isEmpty();
}

bool RpcDir::writeToPath(const QString &path, ProgressCallback progressCallback)
{
    Q_D(RpcDir);
    return d->writeTo(QSharedPointer<NativeRpcDirFileProvider>::create(path), progressCallback);
}

bool RpcDir::readFromPath(const QString &path, ProgressCallback progressCallback)
{
    Q_D(RpcDir);
    return d->readFrom(QSharedPointer<NativeRpcDirFileProvider>::create(path), progressCallback);
}

bool RpcDir::readFromPath(ProgressCallback progressCallback)
{
    Q_D(RpcDir);
    if (d->dirPath.isEmpty()) {
        return false;
    }
    return d->readFrom(QSharedPointer<NativeRpcDirFileProvider>::create(d->dirPath), progressCallback);
}

bool RpcDir::writeTo(QSharedPointer<RpcDirFileProvider> provider, ProgressCallback progressCallback)
{
    Q_D(RpcDir);
    return d->writeTo(provider, progressCallback);
}

bool RpcDir::readFrom(QSharedPointer<RpcDirFileProvider> provider, ProgressCallback progressCallback)
{
    Q_D(RpcDir);
    return d->readFrom(provider, progressCallback);
}

QString RpcDir::name() const
{
    Q_D(const RpcDir);
    return d->name;
}

void RpcDir::setName(const QString &name)
{
    Q_D(RpcDir);
    d->name = name;
}

quint64 RpcDir::size() const
{
    Q_D(const RpcDir);
    return d->size;
}

void RpcDir::setSize(quint64 size)
{
    Q_D(RpcDir);
    d->size = size;
}

QDateTime RpcDir::lastModified() const
{
    Q_D(const RpcDir);
    return d->lastModified;
}

void RpcDir::setLastModified(const QDateTime &dt)
{
    Q_D(RpcDir);
    d->lastModified = dt;
}

QDateTime RpcDir::created() const
{
    Q_D(const RpcDir);
    return d->created;
}

void RpcDir::setCreated(const QDateTime &dt)
{
    Q_D(RpcDir);
    d->created = dt;
}

QDateTime RpcDir::lastAccess() const
{
    Q_D(const RpcDir);
    return d->lastAccess;
}

void RpcDir::setLastAccess(const QDateTime &dt)
{
    Q_D(RpcDir);
    d->lastAccess = dt;
}

QList<RpcDirFileEntry> RpcDir::entries() const
{
    Q_D(const RpcDir);
    return d->entries;
}

void RpcDir::setEntries(const QList<RpcDirFileEntry> &entries)
{
    Q_D(RpcDir);
    d->entries = entries;
}

QVariantMap RpcDir::saveState()
{
    Q_D(const RpcDir);
    QVariantMap state;
    state.insert("name", d->name);
    state.insert("size", d->size);
    if (d->created.isValid()) {
        state.insert("ctime", d->created);
    }
    if (d->lastModified.isValid()) {
        state.insert("mtime", d->lastModified);
    }
    if (d->lastAccess.isValid()) {
        state.insert("atime", d->lastAccess);
    }
    QVariantList entrieObjList;
    for (const RpcDirFileEntry &entry : d->entries) {
        QVariantMap entryObj;
        entryObj.insert("path", entry.path);
        entryObj.insert("size", entry.size);
        if (entry.created.isValid()) {
            entryObj.insert("ctime", entry.created);
        }
        if (entry.lastModified.isValid()) {
            entryObj.insert("mtime", entry.lastModified);
        }
        if (entry.lastAccess.isValid()) {
            entryObj.insert("atime", entry.lastAccess);
        }
        entryObj.insert("isdir", entry.isdir);
        entrieObjList.append(entryObj);
    }
    state.insert("entries", entrieObjList);
    return state;
}

bool RpcDir::restoreState(const QVariantMap &state)
{
    Q_D(RpcDir);
    bool ok;
    d->name = state.value("name").toString();
    d->size = state.value("size").toULongLong(&ok);
    if (!ok || d->name.isEmpty()) {
        return false;
    }
    d->created = state.value("ctime").toDateTime();
    d->lastModified = state.value("mtime").toDateTime();
    d->lastAccess = state.value("atime").toDateTime();
    const QVariantList &entryObjList = state.value("entries").toList();
    d->entries.clear();
    for (const QVariant &t : entryObjList) {
        const QVariantMap &entryObj = t.toMap();
        if (entryObj.isEmpty()) {
            qDebug() << "entry obj should not be empty.";
            return false;
        }
        RpcDirFileEntry entry;
        entry.path = entryObj.value("path").toString();
        entry.size = entryObj.value("size").toULongLong(&ok);
        if (!ok) {
            qDebug() << "invalid rpc dir entry size.";
            return false;
        }
        entry.isdir = entryObj.value("isdir").toBool();
        entry.created = entryObj.value("ctime").toDateTime();
        entry.lastModified = entryObj.value("mtime").toDateTime();
        entry.lastAccess = entryObj.value("atime").toDateTime();
        d->entries.append(entry);
    }
    return true;
}

END_LAFRPC_NAMESPACE
