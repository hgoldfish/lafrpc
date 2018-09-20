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
    typedef std::function<void(qint64 bs, quint64 count, quint64 total)> ProgressCallback;
    explicit RpcFile(const QString &filePath);
    explicit RpcFile();
    virtual ~RpcFile() override;
public:
    bool isValid() const;
    bool writeToPath(const QString &path, ProgressCallback progressCallback = 0);
    bool readFromPath(const QString &path, ProgressCallback progressCallback = 0);
    bool writeTo(QFile &f, ProgressCallback progressCallback = 0);
    bool readFrom(QFile &f, ProgressCallback progressCallback = 0);
public:
    int size() const;
public:
    QVariantMap saveState();
    bool restoreState(const QVariantMap &state);
    static QString lafrpcKey() { return "RpcFile"; }
public:
    RpcFilePrivate * const d_ptr;
    Q_DECLARE_PRIVATE(RpcFile)
};

END_LAFRPC_NAMESPACE

Q_DECLARE_METATYPE(QSharedPointer<LAFRPC_NAMESPACE::RpcFile>)

#endif
