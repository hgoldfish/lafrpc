#ifndef LAFRPC_SENDDIR_H
#define LAFRPC_SENDDIR_H

#include <QtCore/qsharedpointer.h>
#include <QtCore/qfile.h>
#include "base.h"

BEGIN_LAFRPC_NAMESPACE

struct RpcDirFileEntry
{
    RpcDirFileEntry();

    QString path;
    quint64 size;
    QDateTime created;
    QDateTime lastModified;
    QDateTime lastAccess;
    bool isdir;
};


struct CallbackInfo
{
    CallbackInfo(const QString &filePath, qint32 bs, quint64 fileRead, quint64 fileSize, quint64 totalRead, quint64 totalSize)
        : filePath(filePath)
        , currentRead(bs)
        , currentFileRead(fileRead)
        , currentFileSize(fileSize)
        , totalRead(totalRead)
        , totalSize(totalSize) {}
    QString filePath;
    qint32 currentRead;
    quint64 currentFileRead;
    quint64 currentFileSize;
    quint64 totalRead;
    quint64 totalSize;
};


class RpcDirPrivate;
class RpcDirFileProvider;
class RpcDir: public UseStream
{
public:
    typedef std::function<bool(CallbackInfo)> ProgressCallback;
    explicit RpcDir(const QString &path);
    RpcDir();
    virtual ~RpcDir() override;
public:
    bool populate();
    bool isValid() const;

    bool writeToPath(const QString &path, ProgressCallback progressCallback=nullptr);
    bool readFromPath(const QString &path, ProgressCallback progressCallback=nullptr);
    bool readFromPath(ProgressCallback progressCallback=nullptr);

    bool writeTo(QSharedPointer<RpcDirFileProvider> provider, ProgressCallback progressCallback=nullptr);
    bool readFrom(QSharedPointer<RpcDirFileProvider> provider, ProgressCallback progressCallback=nullptr);
public:
    QString name() const;
    void setName(const QString &name);
    quint64 size() const;
    void setSize(quint64 size);
    QDateTime lastModified() const;
    void setLastModified(const QDateTime &dt);
    QDateTime created() const;
    void setCreated(const QDateTime &dt);
    QDateTime lastAccess() const;
    void setLastAccess(const QDateTime &dt);
    QList<RpcDirFileEntry> entries() const;
    void setEntries(const QList<RpcDirFileEntry> &entries);
public:
    QVariantMap saveState();
    bool restoreState(const QVariantMap &state);
    static QString lafrpcKey() { return "RpcDir"; }
public:
    RpcDirPrivate * const d_ptr;
    Q_DECLARE_PRIVATE(RpcDir)
};


class RpcDirFileProvider
{
public:
    virtual ~RpcDirFileProvider();
public:
    virtual QSharedPointer<qtng::FileLike> getFile(const QString &filePath, QIODevice::OpenMode mode) = 0;
    virtual bool createDirectory(const QString &dirPath);
    virtual bool updateTimes(const QString &filePath, const QDateTime &created,
                             const QDateTime &lastModified, const QDateTime &lastAccess);
};


class NativeRpcDirFileProvider: public RpcDirFileProvider
{
public:
    NativeRpcDirFileProvider(const QString &root)
        : rootDir(root) { rootDir.makeAbsolute(); }
    virtual QSharedPointer<qtng::FileLike> getFile(const QString &filePath, QIODevice::OpenMode mode) override;
    virtual bool createDirectory(const QString &dirPath) override;
    virtual bool updateTimes(const QString &filePath, const QDateTime &created, const QDateTime &lastModified, const QDateTime &lastAccess) override;
    QString makePath(const QString &filePath);
public:
    QDir rootDir;
};


END_LAFRPC_NAMESPACE

Q_DECLARE_METATYPE(QSharedPointer<LAFRPC_NAMESPACE::RpcDir>)

#endif
