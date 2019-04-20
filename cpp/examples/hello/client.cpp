// client.cpp
#include "lafrpc.h"

using namespace lafrpc;

int main(int argc, char **argv)
{
    QSharedPointer<Rpc> rpc = RpcBuilder(MessagePack).myPeerName("client").create();
    if(rpc.isNull()) {
        qDebug() << "can not create rpc server.";
        return 1;
    }
    QSharedPointer<Peer> peer = rpc->connect("tcp://127.0.0.1:8002");
    if (peer.isNull()) {
        qDebug() << "can not connect to peer.";
        return 2;
    }
    QString result = peer->call("demo.sayHello", "Goldfish").toString();
    qDebug() << result;
    return 0;
}
