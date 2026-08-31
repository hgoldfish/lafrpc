// std side of the interop test: qtng/rpc server registering "std".
// Usage: ./interop_std_server [port]
#include <cstdio>
#include <memory>
#include <string>

#include "qtng/coroutine.h"
#include "qtng/coroutine_utils.h"
#include "qtng/hostaddress.h"
#include "qtng/socket.h"
#include "qtng/socket_server.h"
#include "qtng/socket_utils.h"

#include "qtng/rpc.h"

using namespace std;
using namespace qtng;
namespace rpc = qtng::rpc;

static int failures = 0;

class StdService: public rpc::Service, public enable_shared_from_this<StdService>
{
public:
    void bindAll()
    {
        bind("reverse", rpc::bindMethod(shared_from_this(), &StdService::reverse));
        bind("mul", rpc::bindMethod(shared_from_this(), &StdService::mul));
    }
    string reverse(const string &s) { return string(s.rbegin(), s.rend()); }
    int64_t mul(int64_t a, int64_t b) { return a * b; }
};

// hand accepted sockets to the rpc hub
class RpcHandoverHandler: public BaseRequestHandler
{
protected:
    void handle() override
    {
        shared_ptr<rpc::Rpc> *hub = static_cast<shared_ptr<rpc::Rpc> *>(server->userData());
        if (!(*hub)->handleRequest(request)) {
            return;
        }
        // Rpc::newPeer is emitted asynchronously, so poll briefly for the
        // peer to be registered, then wait until it disconnects before
        // returning — as soon as handle() returns, the server coroutine
        // calls closeRequest() and closes the socket.
        shared_ptr<rpc::Peer> peer;
        for (int i = 0; i < 100 && !peer; ++i) {
            peer = (*hub)->get("qt-side");
            if (!peer) {
                qtng::Coroutine::msleep(10);
            }
        }
        if (peer) {
            while (peer->isOk()) {
                qtng::Coroutine::msleep(50);
            }
        }
    }
};

int main(int argc, char **argv)
{
    uint16_t port = 17942;
    if (argc > 1) {
        port = static_cast<uint16_t>(atoi(argv[1]));
    }

    shared_ptr<rpc::Rpc> rpcServer = rpc::Rpc::builder().myPeerName("std-side").create();
    shared_ptr<StdService> svc = make_shared<StdService>();
    svc->bindAll();
    rpcServer->registerInstance(svc, "std");

    // call back into the Qt client's "demo" service once a peer appears
    rpcServer->onNewPeer([](shared_ptr<rpc::Peer> peer) {
        if (!peer) {
            return;
        }
        printf("[std] new peer: %s\n", peer->name().c_str());
        fflush(stdout);
        Coroutine::spawn([peer] {
            // give the qt client time to finish its own calls first; both
            // directions share one DataChannel but requests are independent.
            qtng::Coroutine::msleep(300);
            try {
                rpc::ValueList args;
                args.push_back(rpc::Value::str("goldfish"));
                rpc::Value r1 = peer->call("demo.sayHello", args);
                if (r1.type() == rpc::Value::Type::Str && r1.asStr() == "Hello, goldfish") {
                    printf("[std] ok: std->qt demo.sayHello\n");
                } else {
                    printf("[std] FAIL: std->qt demo.sayHello wrong result\n");
                    ++failures;
                }

                rpc::ValueList args2;
                args2.push_back(rpc::Value(static_cast<int64_t>(40)));
                args2.push_back(rpc::Value(static_cast<int64_t>(2)));
                rpc::Value r2 = peer->call("demo.add", args2);
                if (r2.asInt() == 42) {
                    printf("[std] ok: std->qt demo.add\n");
                } else {
                    printf("[std] FAIL: std->qt demo.add wrong result\n");
                    ++failures;
                }
            } catch (const rpc::RpcException &e) {
                printf("[std] FAIL: call into qt failed: %s\n", e.what().c_str());
                ++failures;
            }
            printf(failures == 0 ? "[std] ALL_OK\n" : "[std] FAILED=%d\n", failures);
            fflush(stdout);
        });
    });

    TcpServer<RpcHandoverHandler> server(HostAddress::LocalHost, port);
    server.setUserData(&rpcServer);
    printf("[std] listening on 127.0.0.1:%u\n", static_cast<unsigned>(port));
    fflush(stdout);

    // qt client dials in after a moment
    Coroutine::spawn([] {
        Coroutine::sleep(2.0f);
    });

    server.serveForever();
    return failures == 0 ? 0 : 1;
}
