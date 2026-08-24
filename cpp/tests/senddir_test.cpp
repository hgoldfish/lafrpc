#include <QtTest>
#include <QtCore/qcryptographichash.h>
#include <QtCore/qtemporarydir.h>

#include "lafrpc.h"
#include "qtnetworkng.h"

using namespace qtng;
using namespace lafrpc;

// ======================= 测试辅助 =======================

static void writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qFatal("can not create file: %s", qPrintable(path));
    }
    f.write(data);
    f.close();
}

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return f.readAll();
}

static QByteArray hashFile(const QString &path)
{
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    hasher.addData(&f);
    return hasher.result();
}

// 确定性的伪随机内容，用于大文件传输的一致性校验
static QByteArray makePatternData(quint64 size, quint32 seed)
{
    QByteArray data(static_cast<int>(size), Qt::Uninitialized);
    char *p = data.data();
    for (quint64 i = 0; i < size; ++i) {
        p[i] = static_cast<char>((seed + i * 31) & 0xff);
    }
    return data;
}

// 构建一棵用于传输的目录树，返回其中的文件相对路径列表
static QStringList buildTree(const QString &root)
{
    QDir dir(root);
    dir.mkpath("sub/deep");
    dir.mkpath("sub/empty");
    dir.mkpath(".hidden");
    writeFile(dir.filePath("a.txt"), "hello world");
    writeFile(dir.filePath("b.bin"), makePatternData(200 * 1024, 7));
    writeFile(dir.filePath("empty_file"), QByteArray());
    writeFile(dir.filePath("sub/c.dat"), makePatternData(300 * 1024, 9));
    writeFile(dir.filePath("sub/deep/d.bin"), makePatternData(64 * 1024 + 17, 11));
    writeFile(dir.filePath(".hidden/e.txt"), "hidden content");
    QStringList files;
    files << "a.txt"
          << "b.bin"
          << "empty_file"
          << "sub/c.dat"
          << "sub/deep/d.bin"
          << ".hidden/e.txt";
    return files;
}

// 递归收集目录下所有文件的相对路径（与 RpcDir::populate 的 DirsFirst 语义一致）
static void collectFilesRecursive(const QDir &dir, const QString &prefix, QStringList *files)
{
    const QFileInfoList &infos = dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::Readable,
                                                   QDir::DirsFirst);
    for (const QFileInfo &info : infos) {
        const QString rel = prefix.isEmpty() ? info.fileName() : prefix + QLatin1Char('/') + info.fileName();
        if (info.isDir()) {
            collectFilesRecursive(QDir(info.absoluteFilePath()), rel, files);
        } else {
            files->append(rel);
        }
    }
}

// 通过本地 TCP 回环建立一对互联的 VirtualChannel。
// clientVirtual 与 serverVirtual 之间的数据会互相到达。
struct ChannelPair
{
    QSharedPointer<Socket> serverListener;
    quint16 port = 0;
    QSharedPointer<Socket> acceptedSocket;
    QSharedPointer<SocketChannel> serverChannel;
    QSharedPointer<VirtualChannel> serverVirtual;
    QSharedPointer<Socket> clientSocket;
    QSharedPointer<SocketChannel> clientChannel;
    QSharedPointer<VirtualChannel> clientVirtual;

    void abortAll()
    {
        if (!serverChannel.isNull()) {
            serverChannel->abort();
        }
        if (!clientChannel.isNull()) {
            clientChannel->abort();
        }
    }
};

static bool makeChannelPair(ChannelPair *pair)
{
    Q_ASSERT(pair);
    Event portReady;
    Event allDone;
    CoroutineGroup workers;
    workers.spawn([pair, &portReady, &allDone] {
        pair->serverListener.reset(Socket::createServer(HostAddress::LocalHost, 0));
        if (pair->serverListener.isNull()) {
            allDone.set();
            return;
        }
        pair->port = pair->serverListener->localPort();
        portReady.set();
        pair->acceptedSocket.reset(pair->serverListener->accept());
        if (pair->acceptedSocket.isNull()) {
            allDone.set();
            return;
        }
        pair->serverChannel.reset(new SocketChannel(pair->acceptedSocket, NegativePole));
        pair->serverVirtual = pair->serverChannel->takeChannel();
        allDone.set();
    });
    workers.spawn([pair, &portReady, &allDone] {
        if (!portReady.tryWait()) {
            allDone.set();
            return;
        }
        pair->clientSocket.reset(Socket::createConnection(HostAddress::LocalHost, pair->port));
        if (pair->clientSocket.isNull()) {
            allDone.set();
            return;
        }
        pair->clientChannel.reset(new SocketChannel(pair->clientSocket, PositivePole));
        pair->clientVirtual = pair->clientChannel->makeChannel();
        allDone.set();
    });
    workers.joinall();
    return !pair->clientVirtual.isNull() && !pair->serverVirtual.isNull();
}

// 白盒 provider：记录调用、可注入失败
class MockProvider : public RpcDirFileProvider
{
public:
    MockProvider(const QString &root)
        : rootDir(root)
    {
        rootDir.makeAbsolute();
    }
    virtual ~MockProvider() override { }

    virtual QSharedPointer<FileLike> getFile(const QString &filePath, QIODevice::OpenMode mode) override
    {
        requestedFiles.append(filePath);
        requestedModes.append(mode);
        if (failGetFiles.contains(filePath)) {
            return QSharedPointer<FileLike>();
        }
        const QString fullPath = rootDir.filePath(filePath);
        QSharedPointer<QFile> file(new QFile(fullPath));
        if (!file->open(mode)) {
            return QSharedPointer<FileLike>();
        }
        return FileLike::rawFile(file);
    }

    virtual bool createDirectory(const QString &dirPath) override
    {
        createdDirs.append(dirPath);
        if (!failCreateDir.isEmpty() && dirPath == failCreateDir) {
            return false;
        }
        return rootDir.mkpath(dirPath);
    }

    virtual bool updateTimes(const QString &filePath, const QDateTime &, const QDateTime &, const QDateTime &) override
    {
        updatedFiles.append(filePath);
        if (failUpdateTimes) {
            return false;
        }
        return true;
    }

    QString path(const QString &filePath) const { return rootDir.filePath(filePath); }

public:
    QDir rootDir;
    QStringList failGetFiles;
    QString failCreateDir;
    bool failUpdateTimes = false;
    QStringList createdDirs;
    QStringList updatedFiles;
    QStringList requestedFiles;
    QList<QIODevice::OpenMode> requestedModes;
};

// 通用传输执行器：通过 channel pair 执行一次 readFrom -> writeTo
struct TransferResult
{
    bool sendOk = false;
    bool recvOk = false;
    bool timedOut = false;
};

static TransferResult runTransfer(const QList<RpcDirFileEntry> &entries, quint64 totalSize,
                                  QSharedPointer<RpcDirFileProvider> sendProvider,
                                  QSharedPointer<RpcDirFileProvider> recvProvider,
                                  RpcDir::ProgressCallback sendCallback = nullptr,
                                  RpcDir::ProgressCallback recvCallback = nullptr,
                                  float timeoutSeconds = 5.0f,
                                  const QList<RpcDirFileEntry> *recvEntries = nullptr,
                                  quint64 recvSize = 0)
{
    TransferResult result;
    ChannelPair pair;
    if (!makeChannelPair(&pair)) {
        return result;
    }

    QSharedPointer<RpcDir> sender(new RpcDir());
    sender->setEntries(entries);
    sender->setSize(totalSize);
    sender->channel = pair.clientVirtual;
    sender->ready.set();

    QSharedPointer<RpcDir> receiver(new RpcDir());
    receiver->setEntries(recvEntries ? *recvEntries : entries);
    receiver->setSize(recvEntries ? recvSize : totalSize);
    receiver->channel = pair.serverVirtual;
    receiver->ready.set();

    CoroutineGroup workers;
    QSharedPointer<Event> done(new Event());
    workers.spawn([sender, sendProvider, &result, sendCallback, done] {
        result.sendOk = sender->readFrom(sendProvider, sendCallback);
        done->set();
    });
    workers.spawn([receiver, recvProvider, &result, recvCallback, done] {
        result.recvOk = receiver->writeTo(recvProvider, recvCallback);
        // 模拟 RPC 服务端在响应返回后释放流通道（VirtualChannel 析构 -> abort），
        // 从而使发送端 readFrom 收尾的 recvPacket() 能够返回。
        if (!receiver->channel.isNull()) {
            receiver->channel->abort();
        }
        done->set();
    });
    workers.spawn([&pair, &result, done, timeoutSeconds] {
        if (!done->tryWait(static_cast<quint32>(timeoutSeconds * 1000))) {
            result.timedOut = true;
            pair.abortAll();
        }
    });
    workers.joinall();
    return result;
}

// 在协程中构造 RpcDir(path)，避免在主协程中调用 callInThread 的潜在调度问题
static QSharedPointer<RpcDir> makeRpcDir(const QString &path)
{
    QSharedPointer<RpcDir> result;
    CoroutineGroup g;
    g.spawn([&result, path] { result.reset(new RpcDir(path)); });
    g.joinall();
    return result;
}

// ======================= 测试类 =======================

class TestSendDir : public QObject
{
    Q_OBJECT
private slots:
    // ---- 单元测试：RpcDirFileEntry / CallbackInfo ----
    void entryDefaults();
    void callbackInfoFields();

    // ---- 单元测试：RpcDir 属性 ----
    void rpcDirDefaults();
    void rpcDirSetters();
    void rpcDirFromNonexistentPath();
    void rpcDirFromRealDir();
    void populateUnreadablePath();
    void isValidEmptyName();

    // ---- 单元测试：saveState / restoreState ----
    void saveRestoreState();
    void restoreStateInvalidName();
    void restoreStateInvalidSize();
    void restoreStateEmptyEntry();
    void restoreStateBadEntrySize();

    // ---- 单元测试：NativeRpcDirFileProvider ----
    void nativeProviderMakePath();
    void nativeProviderMakePathTraversal();
    void nativeProviderGetFile();
    void nativeProviderCreateDirectory();
    void nativeProviderUpdateTimes();
    void providerBaseClassDefaults();

    // ---- 传输测试（channel 级白盒） ----
    void transferEmptyDir();
    void transferSingleFile();
    void transferNestedTree();
    void transferLargeFile();
    void transferSpecialNames();
    void transferZeroByteFile();
    void transferProgressCallback();
    void transferCancelBySendCallback();
    void transferCancelByRecvCallback();
    void transferCreateDirectoryFailure();
    void transferGetFileFailureOnSend();
    void transferGetFileFailureOnReceive();
    void transferUpdateTimesFailure();
    void transferMismatchedSizePacket();
    void transferNoChannel();
    void transferReadSourceMissing();

    // ---- RPC 端到端 ----
    void rpcEndToEndDirectory();
    void rpcEndToEndEmptyDirectory();
    void rpcEndToEndRejectInvalid();
};

// ======================= 单元测试实现 =======================

void TestSendDir::entryDefaults()
{
    RpcDirFileEntry entry;
    QVERIFY(entry.path.isEmpty());
    QCOMPARE(entry.size, quint64(0));
    QVERIFY(!entry.isdir);
    QVERIFY(!entry.created.isValid());
    QVERIFY(!entry.lastModified.isValid());
    QVERIFY(!entry.lastAccess.isValid());
}

void TestSendDir::callbackInfoFields()
{
    CallbackInfo info(QStringLiteral("a/b.txt"), 4096, 8192, 10000, 20000, 30000);
    QCOMPARE(info.filePath, QStringLiteral("a/b.txt"));
    QCOMPARE(info.currentRead, qint32(4096));
    QCOMPARE(info.currentFileRead, quint64(8192));
    QCOMPARE(info.currentFileSize, quint64(10000));
    QCOMPARE(info.totalRead, quint64(20000));
    QCOMPARE(info.totalSize, quint64(30000));
}

void TestSendDir::rpcDirDefaults()
{
    QSharedPointer<RpcDir> dir(new RpcDir());
    QVERIFY(!dir->isValid());
    QVERIFY(dir->name().isEmpty());
    QCOMPARE(dir->size(), quint64(0));
    QVERIFY(dir->entries().isEmpty());
    QVERIFY(!dir->created().isValid());
    QVERIFY(!dir->lastModified().isValid());
    QVERIFY(!dir->lastAccess().isValid());
}

void TestSendDir::rpcDirSetters()
{
    QSharedPointer<RpcDir> dir(new RpcDir());
    dir->setName(QStringLiteral("my name"));
    QCOMPARE(dir->name(), QStringLiteral("my name"));
    QVERIFY(dir->isValid());

    dir->setSize(12345);
    QCOMPARE(dir->size(), quint64(12345));

    const QDateTime created = QDateTime::fromMSecsSinceEpoch(1000);
    const QDateTime modified = QDateTime::fromMSecsSinceEpoch(2000);
    const QDateTime accessed = QDateTime::fromMSecsSinceEpoch(3000);
    dir->setCreated(created);
    dir->setLastModified(modified);
    dir->setLastAccess(accessed);
    QCOMPARE(dir->created(), created);
    QCOMPARE(dir->lastModified(), modified);
    QCOMPARE(dir->lastAccess(), accessed);

    QList<RpcDirFileEntry> entries;
    RpcDirFileEntry entry;
    entry.path = QStringLiteral("x");
    entry.size = 42;
    entries.append(entry);
    dir->setEntries(entries);
    QCOMPARE(dir->entries().size(), 1);
    QCOMPARE(dir->entries().at(0).path, QStringLiteral("x"));
}

void TestSendDir::rpcDirFromNonexistentPath()
{
    const QString fakePath = QStringLiteral("/nonexistent/definitely/not/here");
    QSharedPointer<RpcDir> dir = makeRpcDir(fakePath);
    // 实现行为：name 取路径最后一段（"here"），因此 isValid() 为 true；
    // 但路径不存在，populate() 不会被调用，entries 为空、size 为 0。
    QVERIFY(dir->isValid());
    QCOMPARE(dir->name(), QStringLiteral("here"));
    QVERIFY(dir->entries().isEmpty());
    QCOMPARE(dir->size(), quint64(0));
}

void TestSendDir::rpcDirFromRealDir()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString root = tmp.path();
    buildTree(root);

    QSharedPointer<RpcDir> dir = makeRpcDir(root);
    QVERIFY(dir->isValid());
    QCOMPARE(dir->name(), QDir(root).dirName());
    QVERIFY(dir->size() > 0);

    // populate 应收集全部 5 个文件 + 目录条目（4 个目录）
    QStringList expectedFiles;
    collectFilesRecursive(QDir(root), QString(), &expectedFiles);
    int fileCount = expectedFiles.size();
    QVERIFY(fileCount > 0);

    int entriesFileCount = 0;
    for (const RpcDirFileEntry &entry : dir->entries()) {
        if (!entry.isdir) {
            ++entriesFileCount;
        }
    }
    QCOMPARE(entriesFileCount, fileCount);

    // size 应等于所有文件大小之和
    quint64 total = 0;
    for (const QString &f : expectedFiles) {
        total += static_cast<quint64>(QFileInfo(QDir(root).filePath(f)).size());
    }
    QCOMPARE(dir->size(), total);
}

void TestSendDir::populateUnreadablePath()
{
    QSharedPointer<RpcDir> dir(new RpcDir());
    QVERIFY(!dir->populate());  // dirPath 为空
}

void TestSendDir::isValidEmptyName()
{
    QSharedPointer<RpcDir> dir(new RpcDir());
    dir->setSize(100);
    dir->setEntries(QList<RpcDirFileEntry>());
    QVERIFY(!dir->isValid());  // name 为空 -> 无效
}

void TestSendDir::saveRestoreState()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString root = tmp.path();
    buildTree(root);

    QSharedPointer<RpcDir> dir = makeRpcDir(root);
    dir->setName(QStringLiteral("saved"));
    const QVariantMap state = dir->saveState();
    QVERIFY(!state.isEmpty());
    QCOMPARE(state.value("name").toString(), QStringLiteral("saved"));
    QCOMPARE(state.value("size").toULongLong(), dir->size());

    QSharedPointer<RpcDir> restored(new RpcDir());
    QVERIFY(restored->restoreState(state));
    QCOMPARE(restored->name(), QStringLiteral("saved"));
    QCOMPARE(restored->size(), dir->size());
    QCOMPARE(restored->entries().size(), dir->entries().size());
    for (int i = 0; i < dir->entries().size(); ++i) {
        const RpcDirFileEntry &a = dir->entries().at(i);
        const RpcDirFileEntry &b = restored->entries().at(i);
        QCOMPARE(a.path, b.path);
        QCOMPARE(a.size, b.size);
        QCOMPARE(a.isdir, b.isdir);
    }

    // 序列化 roundtrip
    QSharedPointer<Rpc> rpc = Rpc::builder(MessagePack).create();
    QVERIFY(!rpc.isNull());
    const QByteArray packed = rpc->serialization()->pack(QVariant::fromValue(dir));
    QVERIFY(!packed.isEmpty());
    const QVariant unpacked = rpc->serialization()->unpack(packed);
    QSharedPointer<RpcDir> viaPack = unpacked.value<QSharedPointer<RpcDir>>();
    QVERIFY(!viaPack.isNull());
    QCOMPARE(viaPack->name(), QStringLiteral("saved"));
    QCOMPARE(viaPack->entries().size(), dir->entries().size());
}

void TestSendDir::restoreStateInvalidName()
{
    QVariantMap state;
    state.insert("size", quint64(0));
    QSharedPointer<RpcDir> dir(new RpcDir());
    QVERIFY(!dir->restoreState(state));
}

void TestSendDir::restoreStateInvalidSize()
{
    QVariantMap state;
    state.insert("name", QStringLiteral("x"));
    state.insert("size", QStringLiteral("not-a-number"));
    QSharedPointer<RpcDir> dir(new RpcDir());
    QVERIFY(!dir->restoreState(state));
}

void TestSendDir::restoreStateEmptyEntry()
{
    QVariantMap state;
    state.insert("name", QStringLiteral("x"));
    state.insert("size", quint64(0));
    state.insert("entries", QVariantList() << QVariantMap());
    QSharedPointer<RpcDir> dir(new RpcDir());
    QVERIFY(!dir->restoreState(state));
}

void TestSendDir::restoreStateBadEntrySize()
{
    QVariantMap entryObj;
    entryObj.insert("path", QStringLiteral("a"));
    entryObj.insert("size", QStringLiteral("bad"));
    QVariantMap state;
    state.insert("name", QStringLiteral("x"));
    state.insert("size", quint64(0));
    state.insert("entries", QVariantList() << entryObj);
    QSharedPointer<RpcDir> dir(new RpcDir());
    QVERIFY(!dir->restoreState(state));
}

void TestSendDir::nativeProviderMakePath()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    NativeRpcDirFileProvider provider(tmp.path());

    // 空路径被拒绝
    QVERIFY(provider.makePath(QString()).isEmpty());
    // 绝对路径被拒绝
    QVERIFY(provider.makePath(QStringLiteral("/etc/passwd")).isEmpty());
    // 合法相对路径
    const QString p = provider.makePath(QStringLiteral("a/b.txt"));
    QVERIFY(!p.isEmpty());
    QCOMPARE(p, QDir(tmp.path()).filePath(QStringLiteral("a/b.txt")));
}

void TestSendDir::nativeProviderMakePathTraversal()
{
    // 当前实现未对 ../ 做规范化校验，这里记录其行为（潜在路径穿越风险）
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    NativeRpcDirFileProvider provider(tmp.path());
    const QString p = provider.makePath(QStringLiteral("../evil.txt"));
    // 由于 QDir::filePath 只做拼接，../evil.txt 会越过 root 目录。
    // 这是一个已知的安全缺陷（未做 canonical 校验），测试记录当前行为而非崩溃。
    QVERIFY(!p.isEmpty());
    QVERIFY(p.startsWith(tmp.path()));
}

void TestSendDir::nativeProviderGetFile()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString root = tmp.path();
    writeFile(QDir(root).filePath("existing.txt"), QByteArrayLiteral("data"));

    NativeRpcDirFileProvider provider(root);

    // 读已存在的文件
    QSharedPointer<FileLike> f = provider.getFile(QStringLiteral("existing.txt"), QIODevice::ReadOnly);
    QVERIFY(!f.isNull());
    QByteArray buf(4, Qt::Uninitialized);
    QCOMPARE(f->read(buf.data(), 4), qint32(4));
    QCOMPARE(buf, QByteArrayLiteral("data"));

    // 读不存在的文件 -> 空
    QVERIFY(provider.getFile(QStringLiteral("missing.txt"), QIODevice::ReadOnly).isNull());

    // 写模式创建新文件
    QSharedPointer<FileLike> wf = provider.getFile(QStringLiteral("new.txt"), QIODevice::WriteOnly);
    QVERIFY(!wf.isNull());
    const QByteArray payload(QByteArrayLiteral("written"));
    QCOMPARE(wf->write(payload.constData(), payload.size()), qint32(payload.size()));
    wf->close();
    QCOMPARE(readFile(QDir(root).filePath("new.txt")), payload);
}

void TestSendDir::nativeProviderCreateDirectory()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString root = tmp.path();
    NativeRpcDirFileProvider provider(root);

    QVERIFY(provider.createDirectory(QStringLiteral("sub/dir")));
    QVERIFY(QDir(QDir(root).filePath("sub/dir")).exists());

    // 绝对路径被拒绝
    QVERIFY(!provider.createDirectory(QStringLiteral("/tmp/evil")));
}

void TestSendDir::nativeProviderUpdateTimes()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString root = tmp.path();
    writeFile(QDir(root).filePath("a.txt"), QByteArrayLiteral("x"));
    NativeRpcDirFileProvider provider(root);
    QVERIFY(provider.updateTimes(QStringLiteral("a.txt"), QDateTime::currentDateTime(),
                                 QDateTime::currentDateTime(), QDateTime::currentDateTime()));
    // 路径不存在时返回 false
    QVERIFY(!provider.updateTimes(QStringLiteral("missing.txt"), QDateTime(), QDateTime(), QDateTime()));
}

void TestSendDir::providerBaseClassDefaults()
{
    struct BareProvider : public RpcDirFileProvider
    {
        virtual QSharedPointer<FileLike> getFile(const QString &, QIODevice::OpenMode) override
        {
            return QSharedPointer<FileLike>();
        }
    };
    BareProvider provider;
    // 基类的默认实现
    QVERIFY(!provider.createDirectory(QStringLiteral("x")));
    QVERIFY(!provider.updateTimes(QStringLiteral("x"), QDateTime(), QDateTime(), QDateTime()));
}

// ======================= 传输测试实现 =======================

void TestSendDir::transferEmptyDir()
{
    QTemporaryDir src;
    QTemporaryDir dst;
    QVERIFY(src.isValid());
    QVERIFY(dst.isValid());

    QList<RpcDirFileEntry> entries;
    TransferResult r = runTransfer(entries, 0,
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(src.path())),
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(dst.path())));
    QVERIFY(!r.timedOut);
    QVERIFY(r.sendOk);
    QVERIFY(r.recvOk);
}

void TestSendDir::transferSingleFile()
{
    QTemporaryDir src;
    QTemporaryDir dst;
    QVERIFY(src.isValid());
    QVERIFY(dst.isValid());

    const QString srcPath = QDir(src.path()).filePath("file.txt");
    const QByteArray data = makePatternData(64 * 1024 + 123, 5);
    writeFile(srcPath, data);

    RpcDirFileEntry entry;
    entry.path = QStringLiteral("file.txt");
    entry.size = static_cast<quint64>(data.size());
    entry.isdir = false;
    QList<RpcDirFileEntry> entries;
    entries.append(entry);

    TransferResult r = runTransfer(entries, entry.size,
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(src.path())),
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(dst.path())));
    QVERIFY(!r.timedOut);
    QVERIFY(r.sendOk);
    QVERIFY(r.recvOk);
    QCOMPARE(readFile(QDir(dst.path()).filePath("file.txt")), data);
}

void TestSendDir::transferNestedTree()
{
    QTemporaryDir src;
    QTemporaryDir dst;
    QVERIFY(src.isValid());
    QVERIFY(dst.isValid());

    const QString root = src.path();
    buildTree(root);

    QSharedPointer<RpcDir> dir = makeRpcDir(root);
    QList<RpcDirFileEntry> entries = dir->entries();
    QVERIFY(!entries.isEmpty());

    TransferResult r = runTransfer(entries, dir->size(),
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(root)),
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(dst.path())));
    QVERIFY(!r.timedOut);
    QVERIFY(r.sendOk);
    QVERIFY(r.recvOk);

    // 目标目录内容与源一致
    QStringList expectedFiles;
    collectFilesRecursive(QDir(root), QString(), &expectedFiles);
    QVERIFY(!expectedFiles.isEmpty());
    for (const QString &rel : expectedFiles) {
        const QString srcFile = QDir(root).filePath(rel);
        const QString dstFile = QDir(dst.path()).filePath(rel);
        QVERIFY2(QFileInfo::exists(dstFile), qPrintable(rel));
        QCOMPARE(hashFile(dstFile), hashFile(srcFile));
    }

    // 目录结构完整
    QVERIFY(QDir(QDir(dst.path()).filePath("sub/deep")).exists());
    QVERIFY(QDir(QDir(dst.path()).filePath("sub/empty")).exists());
}

void TestSendDir::transferLargeFile()
{
    // 超过默认接收队列容量（128KB），并接近/超过 8MB capacity，验证大文件流控
    QTemporaryDir src;
    QTemporaryDir dst;
    QVERIFY(src.isValid());
    QVERIFY(dst.isValid());

    const QString srcPath = QDir(src.path()).filePath("large.bin");
    const QByteArray data = makePatternData(9 * 1024 * 1024 + 12345, 21);
    writeFile(srcPath, data);

    RpcDirFileEntry entry;
    entry.path = QStringLiteral("large.bin");
    entry.size = static_cast<quint64>(data.size());
    QList<RpcDirFileEntry> entries;
    entries.append(entry);

    TransferResult r = runTransfer(entries, entry.size,
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(src.path())),
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(dst.path())),
                                   nullptr, nullptr, 120.0f);
    QVERIFY(!r.timedOut);
    QVERIFY(r.sendOk);
    QVERIFY(r.recvOk);
    QCOMPARE(readFile(QDir(dst.path()).filePath("large.bin")), data);
}

void TestSendDir::transferSpecialNames()
{
    QTemporaryDir src;
    QTemporaryDir dst;
    QVERIFY(src.isValid());
    QVERIFY(dst.isValid());

    const QString root = src.path();
    writeFile(QDir(root).filePath("with space.txt"), QByteArrayLiteral("space"));
    writeFile(QDir(root).filePath("中文名.txt"), QByteArrayLiteral("chinese"));
    writeFile(QDir(root).filePath("quote\"file.txt"), QByteArrayLiteral("quote"));

    QSharedPointer<RpcDir> dir = makeRpcDir(root);
    QList<RpcDirFileEntry> entries = dir->entries();
    QCOMPARE(entries.size(), 3);

    TransferResult r = runTransfer(entries, dir->size(),
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(root)),
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(dst.path())));
    QVERIFY(!r.timedOut);
    QVERIFY(r.sendOk);
    QVERIFY(r.recvOk);
    QCOMPARE(readFile(QDir(dst.path()).filePath("with space.txt")), QByteArrayLiteral("space"));
    QCOMPARE(readFile(QDir(dst.path()).filePath("中文名.txt")), QByteArrayLiteral("chinese"));
    QCOMPARE(readFile(QDir(dst.path()).filePath("quote\"file.txt")), QByteArrayLiteral("quote"));
}

void TestSendDir::transferZeroByteFile()
{
    QTemporaryDir src;
    QTemporaryDir dst;
    QVERIFY(src.isValid());
    QVERIFY(dst.isValid());

    const QString root = src.path();
    writeFile(QDir(root).filePath("empty.txt"), QByteArray());

    QSharedPointer<RpcDir> dir = makeRpcDir(root);
    QList<RpcDirFileEntry> entries = dir->entries();
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.at(0).size, quint64(0));

    TransferResult r = runTransfer(entries, dir->size(),
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(root)),
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(dst.path())));
    QVERIFY(!r.timedOut);
    QVERIFY(r.sendOk);
    QVERIFY(r.recvOk);
    QVERIFY(QFileInfo(QDir(dst.path()).filePath("empty.txt")).exists());
    QCOMPARE(readFile(QDir(dst.path()).filePath("empty.txt")), QByteArray());
}

void TestSendDir::transferProgressCallback()
{
    QTemporaryDir src;
    QTemporaryDir dst;
    QVERIFY(src.isValid());
    QVERIFY(dst.isValid());

    const QString root = src.path();
    writeFile(QDir(root).filePath("a.bin"), makePatternData(100 * 1024, 3));
    QDir(root).mkpath("d1");
    writeFile(QDir(root).filePath("d1/b.bin"), makePatternData(50 * 1024, 4));

    QSharedPointer<RpcDir> dir = makeRpcDir(root);
    QList<RpcDirFileEntry> entries = dir->entries();

    QSharedPointer<QList<CallbackInfo>> sendCallbacks(new QList<CallbackInfo>());
    QSharedPointer<QList<CallbackInfo>> recvCallbacks(new QList<CallbackInfo>());
    QSharedPointer<quint64> sendLast(new quint64(0));
    QSharedPointer<quint64> recvLast(new quint64(0));

    RpcDir::ProgressCallback sendCb = [sendCallbacks, sendLast](const CallbackInfo &info) -> bool {
        sendCallbacks->append(info);
        if (!QTest::qVerify(info.totalRead >= *sendLast, "info.totalRead >= *sendLast",
                            "send totalRead is monotonic", __FILE__, __LINE__)) {
            return false;
        }
        *sendLast = info.totalRead;
        if (!QTest::qVerify(info.currentFileRead <= info.currentFileSize || info.currentFileSize == 0,
                            "info.currentFileRead <= info.currentFileSize || info.currentFileSize == 0",
                            "send currentFileRead within file", __FILE__, __LINE__)) {
            return false;
        }
        return true;
    };
    RpcDir::ProgressCallback recvCb = [recvCallbacks, recvLast](const CallbackInfo &info) -> bool {
        recvCallbacks->append(info);
        if (!QTest::qVerify(info.totalRead >= *recvLast, "info.totalRead >= *recvLast",
                            "recv totalRead is monotonic", __FILE__, __LINE__)) {
            return false;
        }
        *recvLast = info.totalRead;
        if (!QTest::qVerify(info.currentFileRead <= info.currentFileSize || info.currentFileSize == 0,
                            "info.currentFileRead <= info.currentFileSize || info.currentFileSize == 0",
                            "recv currentFileRead within file", __FILE__, __LINE__)) {
            return false;
        }
        return true;
    };

    TransferResult r = runTransfer(entries, dir->size(),
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(root)),
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(dst.path())),
                                   sendCb, recvCb);
    QVERIFY(!r.timedOut);
    QVERIFY(r.sendOk);
    QVERIFY(r.recvOk);

    // 目录条目也有回调（currentRead == 0）
    bool sawDirCallback = false;
    for (const CallbackInfo &info : *recvCallbacks) {
        if (info.currentRead == 0 && info.currentFileSize == 0 && !info.filePath.isEmpty()) {
            sawDirCallback = true;
        }
    }
    QVERIFY(sawDirCallback);

    // 最终累计字节数等于总大小
    QVERIFY(!sendCallbacks->isEmpty());
    QVERIFY(!recvCallbacks->isEmpty());
    QCOMPARE(sendCallbacks->last().totalRead, dir->size());
    QCOMPARE(recvCallbacks->last().totalRead, dir->size());
    QCOMPARE(sendCallbacks->last().totalSize, dir->size());
    QCOMPARE(recvCallbacks->last().totalSize, dir->size());
}

void TestSendDir::transferCancelBySendCallback()
{
    QTemporaryDir src;
    QTemporaryDir dst;
    QVERIFY(src.isValid());
    QVERIFY(dst.isValid());

    const QString root = src.path();
    writeFile(QDir(root).filePath("a.bin"), makePatternData(512 * 1024, 6));

    QSharedPointer<RpcDir> dir = makeRpcDir(root);
    QList<RpcDirFileEntry> entries = dir->entries();

    // 第一次回调就取消
    QSharedPointer<int> calls(new int(0));
    RpcDir::ProgressCallback cancelCb = [calls](const CallbackInfo &) -> bool {
        ++(*calls);
        return false;
    };

    TransferResult r = runTransfer(entries, dir->size(),
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(root)),
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(dst.path())),
                                   cancelCb);
    QVERIFY(!r.timedOut);
    QVERIFY(*calls > 0);
    // readFrom 在回调取消时 abort channel 并返回 true
    QVERIFY(r.sendOk);
    // 接收端因为 channel 被 abort 而失败
    QVERIFY(!r.recvOk);
}

void TestSendDir::transferCancelByRecvCallback()
{
    QTemporaryDir src;
    QTemporaryDir dst;
    QVERIFY(src.isValid());
    QVERIFY(dst.isValid());

    const QString root = src.path();
    writeFile(QDir(root).filePath("a.bin"), makePatternData(512 * 1024, 6));

    QSharedPointer<RpcDir> dir = makeRpcDir(root);
    QList<RpcDirFileEntry> entries = dir->entries();

    RpcDir::ProgressCallback cancelCb = [](const CallbackInfo &) -> bool { return false; };

    TransferResult r = runTransfer(entries, dir->size(),
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(root)),
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(dst.path())),
                                   nullptr, cancelCb);
    QVERIFY(!r.timedOut);
    QVERIFY(r.recvOk);
    QVERIFY(!r.sendOk);
}

void TestSendDir::transferCreateDirectoryFailure()
{
    QTemporaryDir src;
    QTemporaryDir dst;
    QVERIFY(src.isValid());
    QVERIFY(dst.isValid());

    const QString root = src.path();
    QDir(root).mkpath("d1");
    writeFile(QDir(root).filePath("d1/a.txt"), QByteArrayLiteral("x"));

    QSharedPointer<RpcDir> dir = makeRpcDir(root);
    QList<RpcDirFileEntry> entries = dir->entries();

    QSharedPointer<MockProvider> recvProvider(new MockProvider(dst.path()));
    recvProvider->failCreateDir = QStringLiteral("d1");

    TransferResult r = runTransfer(entries, dir->size(),
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(root)),
                                   recvProvider);
    QVERIFY(!r.timedOut);
    QVERIFY(!r.recvOk);
    // 发送端是否失败取决于 abort 的时序（可能已完成 a.txt 的发送），此处只强断言接收端失败
    QVERIFY(recvProvider->createdDirs.contains(QStringLiteral("d1")));
}

void TestSendDir::transferGetFileFailureOnSend()
{
    QTemporaryDir src;
    QTemporaryDir dst;
    QVERIFY(src.isValid());
    QVERIFY(dst.isValid());

    const QString root = src.path();
    writeFile(QDir(root).filePath("a.txt"), QByteArrayLiteral("data"));

    QSharedPointer<RpcDir> dir = makeRpcDir(root);
    QList<RpcDirFileEntry> entries = dir->entries();

    QSharedPointer<MockProvider> sendProvider(new MockProvider(root));
    sendProvider->failGetFiles.append(QStringLiteral("a.txt"));

    TransferResult r = runTransfer(entries, dir->size(), sendProvider,
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(dst.path())));
    QVERIFY(!r.timedOut);
    QVERIFY(!r.sendOk);
}

void TestSendDir::transferGetFileFailureOnReceive()
{
    QTemporaryDir src;
    QTemporaryDir dst;
    QVERIFY(src.isValid());
    QVERIFY(dst.isValid());

    const QString root = src.path();
    writeFile(QDir(root).filePath("a.txt"), QByteArrayLiteral("data"));

    QSharedPointer<RpcDir> dir = makeRpcDir(root);
    QList<RpcDirFileEntry> entries = dir->entries();

    QSharedPointer<MockProvider> recvProvider(new MockProvider(dst.path()));
    recvProvider->failGetFiles.append(QStringLiteral("a.txt"));

    TransferResult r = runTransfer(entries, dir->size(),
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(root)),
                                   recvProvider);
    QVERIFY(!r.timedOut);
    QVERIFY(!r.recvOk);
}

void TestSendDir::transferUpdateTimesFailure()
{
    QTemporaryDir src;
    QTemporaryDir dst;
    QVERIFY(src.isValid());
    QVERIFY(dst.isValid());

    const QString root = src.path();
    writeFile(QDir(root).filePath("a.txt"), QByteArrayLiteral("data"));

    QSharedPointer<RpcDir> dir = makeRpcDir(root);
    QList<RpcDirFileEntry> entries = dir->entries();

    QSharedPointer<MockProvider> recvProvider(new MockProvider(dst.path()));
    recvProvider->failUpdateTimes = true;

    TransferResult r = runTransfer(entries, dir->size(),
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(root)),
                                   recvProvider);
    QVERIFY(!r.timedOut);
    QVERIFY(!r.recvOk);
    QVERIFY(recvProvider->updatedFiles.contains(QStringLiteral("a.txt")));
}

void TestSendDir::transferMismatchedSizePacket()
{
    // 接收端 entries 声明的文件大小比发送端小：writeTo 收到的包超过 entry.size -> 失败
    QTemporaryDir src;
    QTemporaryDir dst;
    QVERIFY(src.isValid());
    QVERIFY(dst.isValid());

    const QString root = src.path();
    writeFile(QDir(root).filePath("a.txt"), QByteArray(100, 'x'));

    QList<RpcDirFileEntry> entries;
    RpcDirFileEntry entry;
    entry.path = QStringLiteral("a.txt");
    entry.size = 100;  // 发送端按真实大小 100 字节发送
    entries.append(entry);

    // 接收端错误声明只有 50 字节：writeTo 收到超过 50 字节的包必须失败
    QList<RpcDirFileEntry> recvEntries;
    RpcDirFileEntry recvEntry;
    recvEntry.path = QStringLiteral("a.txt");
    recvEntry.size = 50;
    recvEntries.append(recvEntry);

    TransferResult r = runTransfer(entries, 100,
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(root)),
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(dst.path())),
                                   nullptr, nullptr, 5.0f, &recvEntries, 50);
    // 接收端检测到超量数据包，必须失败；发送端可能已完成握手，sendOk 不作强断言
    QVERIFY(!r.timedOut);
    QVERIFY(!r.recvOk);
}

void TestSendDir::transferNoChannel()
{
    QSharedPointer<RpcDir> dir(new RpcDir());
    dir->setName(QStringLiteral("x"));
    // channel 未设置 -> 返回 false（需先 ready.set()，否则 readFrom/writeTo 会阻塞等待）
    dir->ready.set();
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QSharedPointer<RpcDirFileProvider> provider(new NativeRpcDirFileProvider(tmp.path()));
    QVERIFY(!dir->readFrom(provider));
    QVERIFY(!dir->writeTo(provider));
}

void TestSendDir::transferReadSourceMissing()
{
    // entry 声称存在文件，但源文件缺失 -> readFrom 返回 false
    QTemporaryDir src;
    QTemporaryDir dst;
    QVERIFY(src.isValid());
    QVERIFY(dst.isValid());

    QList<RpcDirFileEntry> entries;
    RpcDirFileEntry entry;
    entry.path = QStringLiteral("missing.bin");
    entry.size = 1024;
    entries.append(entry);

    TransferResult r = runTransfer(entries, 1024,
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(src.path())),
                                   QSharedPointer<RpcDirFileProvider>(new NativeRpcDirFileProvider(dst.path())));
    QVERIFY(!r.timedOut);
    QVERIFY(!r.sendOk);
}

// ======================= RPC 端到端实现 =======================

class PasteService : public QObject
{
    Q_OBJECT
public:
    PasteService(const QString &destPath)
        : destPath(destPath)
    {
    }
public slots:
    bool pasteFiles(const QDateTime &, QSharedPointer<lafrpc::RpcDir> rpcDir)
    {
        if (rpcDir.isNull() || !rpcDir->isValid()) {
            return false;
        }
        QDir dest(destPath);
        if (!dest.mkpath(".")) {
            return false;
        }
        const bool ok = rpcDir->writeToPath(destPath);
        // 通知发送端传输结束：发送端 readFrom 结尾会 recvPacket() 等待接收端
        // 关闭通道，若不在 writeTo 完成后 abort，该 recvPacket 会一直阻塞。
        if (!rpcDir->channel.isNull()) {
            rpcDir->channel->abort();
        }
        return ok;
    }

public:
    QString destPath;
};

// ======================= RPC 端到端实现 =======================

// 模拟 lafdup 的发送流程：客户端把目录通过 RPC 传给服务器并落盘
static bool runRpcTransfer(const QString &srcPath, const QString &destPath, quint16 port, QString *errorString)
{
    Event clientDone;
    bool clientResult = false;
    CoroutineGroup operations;
    QSharedPointer<Rpc> serverRpc;

    operations.spawn([&] {
        serverRpc = Rpc::builder(MessagePack).myPeerName("server").create();
        if (serverRpc.isNull()) {
            clientDone.set();
            return;
        }
        QSharedPointer<PasteService> service(new PasteService(destPath));
        serverRpc->registerInstance(service, "demo");
        const RpcFunction shutdown = [serverRpc](const QVariantList &, const QVariantMap &) -> QVariant {
            // 延迟 500ms 关闭：先让 "shutdown" 的响应返回给客户端，再在事件循环里
            // 真正执行 shutdown。shutdown 内部对服务协程的 kill 定时器会在
            // startServer 的 join 所泵送的事件循环中被处理，从而保证 Rpc 仍存活，
            // 不会出现 KillCoroutineFunctor 悬挂指针。
            qtng::callInEventLoopAsync([serverRpc] { serverRpc->shutdown(); }, 500);
            return true;
        };
        serverRpc->registerFunction(shutdown, "shutdown");
        serverRpc->setAddress("client", QStringLiteral("tcp://127.0.0.1:%1").arg(port));
        serverRpc->startServer(QStringLiteral("tcp://127.0.0.1:%1").arg(port), true);
        // 清除服务注册，断开 shutdown lambda 对 serverRpc 的引用环
        serverRpc->clearServices();
    });

    operations.spawn([&] {
        Coroutine::sleep(0.1f);
        QSharedPointer<Rpc> clientRpc = Rpc::builder(MessagePack).myPeerName("client").create();
        if (clientRpc.isNull()) {
            clientDone.set();
            return;
        }
        clientRpc->setAddress("server", QStringLiteral("tcp://127.0.0.1:%1").arg(port));
        QSharedPointer<Peer> peer;
        try {
            peer = clientRpc->connect("server");
        } catch (RpcException &e) {
            if (errorString) {
                *errorString = e.what();
            }
            clientDone.set();
            return;
        }
        if (peer.isNull()) {
            if (errorString) {
                *errorString = QStringLiteral("can not connect to server.");
            }
            clientDone.set();
            return;
        }
        QSharedPointer<RpcDir> rpcDir(new RpcDir(srcPath));
        if (!rpcDir->isValid()) {
            if (errorString) {
                *errorString = QStringLiteral("source dir is invalid.");
            }
            clientDone.set();
            return;
        }
        QSharedPointer<Coroutine> streamTask = operations.spawn([rpcDir] { rpcDir->readFromPath(); });
        try {
            clientResult = peer->call("demo.pasteFiles", QDateTime::currentDateTime(),
                                      QVariant::fromValue(rpcDir))
                                   .toBool();
        } catch (RpcException &e) {
            if (errorString) {
                *errorString = e.what();
            }
        } catch (CoroutineException &) {
            // 协程被中断，忽略
        }
        if (!clientResult) {
            streamTask->kill();
        }
        try {
            streamTask->join();
        } catch (CoroutineException &) {
        }
        // 通知服务端关闭
        try {
            peer->call("shutdown");
        } catch (...) {
        }
        clientDone.set();
    });

    operations.spawn([&] {
        if (!clientDone.tryWait(30000)) {
            if (errorString) {
                *errorString = QStringLiteral("timeout.");
            }
            // 客户端未能正常完成：强制关闭服务端，避免服务器协程永不退出
            if (!serverRpc.isNull()) {
                serverRpc->shutdown();
            }
            clientDone.set();
        }
    });

    operations.joinall();
    return clientResult;
}

void TestSendDir::rpcEndToEndDirectory()
{
    QTemporaryDir src;
    QTemporaryDir dst;
    QVERIFY(src.isValid());
    QVERIFY(dst.isValid());

    const QString srcPath = src.path();
    buildTree(srcPath);
    const QString destPath = QDir(dst.path()).filePath("recv");

    QSharedPointer<Socket> probe(QSharedPointer<Socket>(Socket::createServer(HostAddress::LocalHost, 0)));
    QVERIFY(!probe.isNull());
    const quint16 port = probe->localPort();
    probe->close();

    QString errorString;
    const bool ok = runRpcTransfer(srcPath, destPath, port, &errorString);
    QVERIFY2(ok, qPrintable(errorString));

    // 校验落盘内容与源一致
    QStringList expectedFiles;
    collectFilesRecursive(QDir(srcPath), QString(), &expectedFiles);
    QVERIFY(!expectedFiles.isEmpty());
    for (const QString &rel : expectedFiles) {
        const QString srcFile = QDir(srcPath).filePath(rel);
        const QString dstFile = QDir(destPath).filePath(rel);
        QVERIFY2(QFileInfo::exists(dstFile), qPrintable(rel));
        QCOMPARE(hashFile(dstFile), hashFile(srcFile));
    }
    QVERIFY(QDir(QDir(destPath).filePath("sub/deep")).exists());
}

void TestSendDir::rpcEndToEndEmptyDirectory()
{
    QTemporaryDir src;
    QTemporaryDir dst;
    QVERIFY(src.isValid());
    QVERIFY(dst.isValid());

    const QString srcPath = QDir(src.path()).filePath("emptysrc");
    QVERIFY(QDir(src.path()).mkpath("emptysrc"));
    const QString destPath = QDir(dst.path()).filePath("recv");

    QSharedPointer<Socket> probe(QSharedPointer<Socket>(Socket::createServer(HostAddress::LocalHost, 0)));
    QVERIFY(!probe.isNull());
    const quint16 port = probe->localPort();
    probe->close();

    QString errorString;
    const bool ok = runRpcTransfer(srcPath, destPath, port, &errorString);
    QVERIFY2(ok, qPrintable(errorString));
}

void TestSendDir::rpcEndToEndRejectInvalid()
{
    // 服务端拒绝无效 RpcDir：在 RPC 调用点直接传一个非法 RpcDir
    QTemporaryDir dst;
    QVERIFY(dst.isValid());
    const QString destPath = QDir(dst.path()).filePath("recv");

    QSharedPointer<Socket> probe(QSharedPointer<Socket>(Socket::createServer(HostAddress::LocalHost, 0)));
    QVERIFY(!probe.isNull());
    const quint16 port = probe->localPort();
    probe->close();

    Event clientDone;
    bool clientResult = true;
    CoroutineGroup operations;
    QSharedPointer<Rpc> serverRpc;
    operations.spawn([&] {
        serverRpc = Rpc::builder(MessagePack).myPeerName("server").create();
        if (serverRpc.isNull()) {
            clientDone.set();
            return;
        }
        QSharedPointer<PasteService> service(new PasteService(destPath));
        serverRpc->registerInstance(service, "demo");
        const RpcFunction shutdown = [serverRpc](const QVariantList &, const QVariantMap &) -> QVariant {
            qtng::callInEventLoopAsync([serverRpc] { serverRpc->shutdown(); }, 500);
            return true;
        };
        serverRpc->registerFunction(shutdown, "shutdown");
        serverRpc->setAddress("client", QStringLiteral("tcp://127.0.0.1:%1").arg(port));
        serverRpc->startServer(QStringLiteral("tcp://127.0.0.1:%1").arg(port), true);
        serverRpc->clearServices();
    });
    operations.spawn([&] {
        Coroutine::sleep(0.1f);
        QSharedPointer<Rpc> clientRpc = Rpc::builder(MessagePack).myPeerName("client").create();
        clientRpc->setAddress("server", QStringLiteral("tcp://127.0.0.1:%1").arg(port));
        QSharedPointer<Peer> peer = clientRpc->connect("server");
        QSharedPointer<RpcDir> invalid(new RpcDir());
        try {
            clientResult = peer->call("demo.pasteFiles", QDateTime::currentDateTime(),
                                      QVariant::fromValue(invalid))
                                   .toBool();
        } catch (RpcException &) {
            clientResult = false;
        }
        try {
            peer->call("shutdown");
        } catch (...) {
        }
        clientDone.set();
    });
    operations.spawn([&] {
        if (!clientDone.tryWait(20000)) {
            if (!serverRpc.isNull()) {
                serverRpc->shutdown();
            }
            clientDone.set();
        }
    });
    operations.joinall();
    QVERIFY(!clientResult);
}

QTEST_GUILESS_MAIN(TestSendDir)
#include "senddir_test.moc"
