from lafrpc import Rpc, exported, RpcFile

addr = "tcp://127.0.0.1:7293"


@exported
def download_file():
    f = RpcFile(__file__)

    def send_file():
        for bs in f.read_from_path(__file__):
            print("sent bytes = {0}".format(bs))

    server_rpc.io_scheduler.spawn(send_file)
    return f


def test():
    client_rpc = Rpc.use("thread", "pickle")
    peer = client_rpc.connect(addr)
    if not peer:
        print("can not connect to server.")
        return
    f = peer.as_proxy().download_file()
    for bs in f.write_to_path("test.dat"):
        print("read bytes = {}".format(bs))


if __name__ == "__main__":
    server_rpc = Rpc.use("thread", "pickle")
    server_rpc.register_function(download_file)
    server_rpc.start_servers(addr, block=False)
    test()

