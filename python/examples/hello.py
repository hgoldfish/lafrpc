import os.path
from lafrpc import Rpc, exported

addr = "ssl://127.0.0.1:8081"

curdir = os.path.abspath(os.path.dirname(__file__))
CERT_FILE = os.path.join(curdir, "test_cert.pem")
KEY_FILE = os.path.join(curdir, "test_key.pem")

class Demo:
    count = 0

    @exported
    def say_hello(self, name):
        self.count += 1
        return "hello, " + name


class Server:
    def __init__(self):
        self.rpc = Rpc.use("gevent", "msgpack")
        self.rpc.register_instance(Demo(), "demo")
        self.rpc.load_key(CERT_FILE, KEY_FILE)

    def start(self):
        return self.rpc.start_server(addr, block = False)  # block default to False

    def shutdown(self):
        self.rpc.close()


def test():
    rpc = Rpc.use("gevent", "msgpack")
    rpc.io_scheduler.sleep(1.0)
    peer = rpc.connect(addr)
    if peer is None:
        print("can not connect to server.")
        return
    else:
        print("peer connected.")
    remote = peer.as_proxy()
    for i in range(10):
        print(remote.demo.say_hello("world"))


if __name__ == "__main__":
    server = Server()
    success = server.start()
    if success:
        print("server is started.")
        try:
            server.rpc.io_scheduler.spawn(test).join()
        except KeyboardInterrupt:
            server.shutdown()
    else:
        print("fail to start server.")



