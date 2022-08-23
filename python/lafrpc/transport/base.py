import weakref
from ..network import DataChannel


class BaseTransport:
    is_secure = False
    name = "base"

    def __init__(self, rpc):
        self.rpc_ref = weakref.ref(rpc)

    def start_server(self, server_address):
        raise NotImplementedError()

    def connect(self, address, timeout) -> DataChannel:
        raise NotImplementedError()

    def make_raw_socket(self, address):
        raise NotImplementedError()

    def get_raw_socket(self, connection_id):
        raise NotImplementedError()

    def can_handle(self, address):
        raise NotImplementedError()

    def schema(self):
        raise NotImplementedError()

    def get_url_template(self):
        raise NotImplementedError()

    def setup_channel(self, request, channel):
        pass

