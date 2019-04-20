// server.cpp
#include "lafrpc.h"

using namespace lafrpc;


class Hello: public QObject
{
    Q_OBJECT
public slots:
    QString sayHello(const QString &name) { return QStringLiteral("Hello, %1").arg(name); }
};

int main(int argc, char **argv)
{
    QSharedPointer<Rpc> rpc = RpcBuilder(MessagePack).myPeerName("server").create();
    if(rpc.isNull()) {
        qDebug() << "can not create rpc server.";
        return 1;
    }
    QSharedPointer<Hello> hello(new Hello());
    rpc->registerInstance(hello, "demo");
    rpc->startServer("tcp://127.0.0.1:8002");
    return 0;
}

#include "server.moc"
