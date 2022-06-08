#ifndef LAFRPC_SENDFILE_H
#define LAFRPC_SENDFILE_H

#include <QtCore/qsharedpointer.h>
#include <QtCore/qfile.h>
#include "base.h"

BEGIN_LAFRPC_NAMESPACE

class RpcFilePrivate;
class RpcFile: public UseStream
{
public:
    typedef std::function<bool(qint64 bs, quint64 count, quint64 total)> ProgressCallback;
    explicit RpcFile(const QString &filePath, bool withHash = false);
    explicit RpcFile();
    virtual ~RpcFile() override;
public:
    bool calculateHash();
    bool isValid() const;

    bool writeToPath(const QString &path, ProgressCallback progressCallback = nullptr);
    bool readFromPath(const QString &path, ProgressCallback progressCallback = nullptr);
    bool readFromPath(ProgressCallback progressCallback = nullptr);
    bool writeTo(QSharedPointer<qtng::FileLike> f, ProgressCallback progressCallback = nullptr);
    bool readFrom(QSharedPointer<qtng::FileLike> f, ProgressCallback progressCallback = nullptr);

    bool sendall(const QByteArray &data, ProgressCallback progressCallback = nullptr);
    bool recvall(QByteArray &data, ProgressCallback progressCallback = nullptr);
public:
    QString name() const;
    void setName(const QString &name);
    quint64 size() const;
    void setSize(quint64 size);
    QDateTime modified() const;
    void setModified(const QDateTime &dt);
    QDateTime created() const;
    void setCreated(const QDateTime &dt);
    QDateTime lastAccess() const;
    void setLastAccess(const QDateTime &dt);
    QByteArray hash() const;
    void setHash(const QByteArray &hash);  // sha256
public:
    QVariantMap saveState();
    bool restoreState(const QVariantMap &state);
    static QString lafrpcKey() { return QString::fromLatin1("RpcFile"); }
public:
    RpcFilePrivate * const d_ptr;
    Q_DECLARE_PRIVATE(RpcFile)
};

END_LAFRPC_NAMESPACE

Q_DECLARE_METATYPE(QSharedPointer<LAFRPC_NAMESPACE::RpcFile>)

#endif
