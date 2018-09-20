#include <QtCore/QCoreApplication>
#include "../laf_rpc.h"


using namespace laf_rpc;

const QString addr = "ssl://127.0.0.1:7942";

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
        QSharedPointer<Rpc> rpc = Rpc::use("server", "msgpack");
        if(rpc.isNull()) {
            qDebug() << "can not create rpc server.";
            return;
        }

        qtng::PrivateKey key = qtng::PrivateKey::generate(qtng::PrivateKey::Rsa, 2048);
        QMultiMap<qtng::Certificate::SubjectInfo, QString> info;
        info.insert(qtng::Certificate::CommonName, "Goldifsh");
        info.insert(qtng::Certificate::CountryName, "CN");
        info.insert(qtng::Certificate::Organization, "GigaCores");
        const QDateTime &now = QDateTime::currentDateTime();
        const qtng::Certificate &cert = qtng::Certificate::generate(key, qtng::MessageDigest::Sha256,
                                                        293424, now, now.addYears(10), info);
        qtng::SslConfiguration config;
        config.setPrivateKey(key);
        config.setLocalCertificate(cert);
        rpc->setSslConfiguration(config);

        const RpcFunction &shutdown = [rpc](const QVariantList &args, const QVariantMap &kwargs) -> QVariant {
            Q_UNUSED(args);
            Q_UNUSED(kwargs);
            rpc->close();
            return true;
        };
        QSharedPointer<Demo> demo(new Demo());
        QSharedPointer<Echo> echo(new Echo());
        rpc->registerFunction(sum, "sum");
        rpc->registerFunction(shutdown, "shutdown");
        rpc->registerInstance(demo, "demo");
        rpc->registerInstance(echo, "echo");
        rpc->startServer(addr, true);
    }
};


class ClientCoroutine: public qtng::Coroutine
{
public:
    virtual void run()
    {
        msleep(100);
        qDebug() << "client started.";
        QSharedPointer<Rpc> rpc = Rpc::use("client", "msgpack");
        if(rpc.isNull()) {
            qDebug() << "can not create rpc client.";
            return;
        }
        QSharedPointer<Peer> peer = rpc->connect(addr);
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
    }
};


int main(int argc, char **argv)
{
    qtng::CoroutineGroup operations;
    operations.start(new ServerCoroutine(), "server");
    operations.start(new ClientCoroutine(), "cient");
    operations.joinall();
    return 0;
}

#include "simple_test.moc"
