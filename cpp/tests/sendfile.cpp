#include <QtCore/qcoreapplication.h>
#include <QtCore/qdebug.h>
#include "lafrpc.h"

using namespace qtng;
using namespace lafrpc;

class ClientCoroutine: public Coroutine
{
public:
    virtual void run();
};


class ServerCoroutine: public Coroutine
{
public:
    virtual void run();
};


class Demo: public QObject
{
    Q_OBJECT
public slots:
    QSharedPointer<RpcFile> get()
    {
        const QString filePath("/home/goldfish/video/large.mp4");
        QSharedPointer<RpcFile> f(new RpcFile(filePath));
        f->preferRawSocket = false;
        operations.spawn([f, filePath] {
            f->readFromPath(filePath, [](qint64 bs, quint64 count, quint64 size) -> bool {
                qDebug() << "sending:" << bs << count << size;
                return true;
            });
        });
        return f;
    }
private:
    CoroutineGroup operations;
};


void ClientCoroutine::run()
{
    sleep(0.2f); // wait for server to start.
    QSharedPointer<Rpc> rpc = Rpc::builder(MessagePack).myPeerName("client").create();
    rpc->startServer("tcp://127.0.0.1:7943", false);
    QSharedPointer<Peer> peer = rpc->connect("tcp://127.0.0.1:7942");
    if (peer.isNull()) {
        qDebug("can not connect to server.");
        return;
    } else {
        qDebug("connected to server.");
    }
    QSharedPointer<RpcFile> f = peer->call("demo.get").value<QSharedPointer<RpcFile>>();
    if (f.isNull()) {
        qDebug("server returns invalid result.");
        return;
    } else {
        qDebug() << "got rpc file:" << f->saveState();
    }
    f->writeToPath("/dev/shm/sample.mp4", [](qint64 bs, quint64 count, quint64 size) -> bool{
        qDebug() << "receiving:" << bs << count << size;
        return true;
    });
}


void ServerCoroutine::run()
{
    QSharedPointer<Rpc> rpc = Rpc::builder(MessagePack).myPeerName("server").create();
    QSharedPointer<Demo> demo(new Demo());
    rpc->registerInstance(demo, "demo");
    rpc->setAddress("client", "tcp://127.0.0.1:7943");
    rpc->startServer("tcp://127.0.0.1:7942");
    qDebug("server exited.");
}


int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    CoroutineGroup operations;
    operations.start(new ServerCoroutine, "server");
    operations.start(new ClientCoroutine, "client");
    operations.get("client")->join();
}


#include "sendfile.moc"
