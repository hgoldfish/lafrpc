#include <QtCore/QCoreApplication>
#include "lafrpc.h"


using namespace lafrpc;

//const QString ServerAddress = "https://127.0.0.1:8443/lafrpc";
//const QString ClientAddress = "ssl://127.0.0.1:8443/";
const QString ServerAddress = "kcp+ssl://127.0.0.1:8443/";
const QString ClientAddress = "kcp+ssl://127.0.0.1:8443/";

class Demo: public QObject
{
    Q_OBJECT
public slots:
    QString sayHello(const QString &name)
    {
        if(name.isEmpty()) {
            throw RpcRemoteException();
        }
        count += 1;
        return QString::fromUtf8("hello, %1").arg(name);
    }
private:
    int count;
};


class Echo: public Callable
{
public:
    QVariant call(const QString &methodName, const QVariantList &args, const QVariantMap &kwargs) override
    {
        Q_UNUSED(kwargs);
        if(methodName == "echo") {
            const QString &s = args.value(0).toString();
            if(s.isEmpty()) {
                throw RpcRemoteException();
            }
            return echo(s);
        }
        throw RpcRemoteException();
    }

    QString echo(const QString &s)
    {
        return s;
    }
};


QVariant sum(const QVariantList &args, const QVariantMap &kwargs)
{
    Q_UNUSED(kwargs);
    double t = 0;
    for(const QVariant &arg: args) {
        bool ok;
        double v = arg.toDouble(&ok);
        if(!ok) {
            throw RpcRemoteException();
        }
        t += v;
    }
    return t;
}


class ServerCoroutine: public qtng::Coroutine
{
public:
    virtual void run()
    {
        QSharedPointer<Rpc> rpc = Rpc::builder(MessagePack)
                .myPeerName("server")
                .sslConfiguration(qtng::SslConfiguration::testPurpose("lafrpc", "CN", "QtNetworkNg"))
                .create();
        if(rpc.isNull()) {
            qDebug() << "can not create rpc server.";
            return;
        }
        const RpcFunction &shutdown = [rpc](const QVariantList &, const QVariantMap &) -> QVariant {
            qtng::callInEventLoopAsync([rpc] {
                rpc->shutdown();
            }, 500);
            qDebug() << "shuting down server.";
            return true;
        };
        QSharedPointer<Demo> demo(new Demo());
        QSharedPointer<Echo> echo(new Echo());
        rpc->registerFunction(sum, "sum");
        rpc->registerFunction(shutdown, "shutdown");
        rpc->registerInstance(demo, "demo");
        rpc->registerInstance(echo, "echo");
        rpc->startServer(ServerAddress, true);
        qDebug() << "server exit.";
    }
};


class ClientCoroutine: public qtng::Coroutine
{
public:
    virtual void run()
    {
        msleep(100);
        qDebug() << "client started.";
        QSharedPointer<Rpc> rpc = Rpc::builder(MessagePack).myPeerName("client").create();
        if(rpc.isNull()) {
            qDebug() << "can not create rpc client.";
            return;
        }
        QSharedPointer<Peer> peer = rpc->connect(ClientAddress);
        if(peer.isNull()) {
            qDebug() << "can not connect to rpc server.";
            return;
        }
        for(int i = 0; i < 10; ++i) {
            QVariantList args = { QString::fromUtf8("goldfish") };
            QVariant result = peer->call("demo.sayHello", args);
            qDebug() << result;
        }
        for(int i = 0; i < 10; ++i) {
            QVariantList args = { QString::number(i) };
            qDebug() << peer->call("echo.echo", args);
        }
        {
            QVariantList args = {1,2,3,4,5,6,7,8,9,10};
            qDebug() << peer->call("sum", args);
        }
        peer->call("shutdown");
        qDebug() << "client exit.";
    }
};


int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    qtng::CoroutineGroup operations;
    operations.start(new ServerCoroutine(), "server");
    operations.start(new ClientCoroutine(), "cient");
    operations.joinall();
    return 0;
}

#include "simple_test.moc"
