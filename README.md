### laf_rpc

A simple RPC implemented based on QtNetworkNg. Many features:

* Using TCP protocol
* Long connection
* Connected peers can call each other.
* Support sending file


Here comes an hello world example:

The server source:

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
        QSharedPointer<Hello> hello(new Hello());
        rpc->registerInstance(hello, "demo");
        rpc->startServer("tcp://127.0.0.1:8002");
        return 0;
    }

    #include "server.moc"

The client source:

    // client.cpp
    #include "lafrpc.h"

    using namespace lafrpc;

    int main(int argc, char **argv)
    {
        QSharedPointer<Rpc> rpc = RpcBuilder(MessagePack).myPeerName("client").create();
        QSharedPointer<Peer> peer = rpc->connect("tcp://127.0.0.1:8002");
        if (peer.isNull()) {
            qDebug() << "can not connect to peer.";
            return 2;
        }
        QString result = peer->call("demo.sayHello", "Goldfish").toString();
        qDebug() << result;
        return 0;
    }

The cmake build file:

    CMAKE_MINIMUM_REQUIRED(VERSION 3.3.0 FATAL_ERROR)

    PROJECT(pbook)

    SET(CMAKE_AUTOMOC ON)

    FIND_PACKAGE(Qt5Core REQUIRED)

    ADD_SUBDIRECTORY(lafrpc/cpp/ lafrpc)

    ADD_EXECUTABLE(server server.cpp)
    ADD_EXECUTABLE(client client.cpp)

    TARGET_LINK_LIBRARIES(server Qt5::Core lafrpc)
    TARGET_LINK_LIBRARIES(client Qt5::Core lafrpc)

Now your project have 3 files:

    hello/
        server.cpp
        client.cpp
        CMakeLists.cpp

Clone this project to project directory:

    git clone --recursive https://github.com/hgoldfish/lafrpc

Then you have 3 files and a subdirectory:

    hello/
        server.cpp
        client.cpp
        CMakeLists.txt
        laf_rpc/

Build and test:

    mkdir build
    cmake ..
    make -j8
    ./server
    ./client   # in another console.
