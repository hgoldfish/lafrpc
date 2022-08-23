import ssl
from .tcp import TcpTransport


class SslTransport(TcpTransport):
    is_secure = True

    def __init__(self, rpc):
        TcpTransport.__init__(self, rpc)
        self.ssl_context = rpc.io_scheduler.SSLContext()
        self.ssl_context.load_default_certs()
        self.ssl_context.check_hostname = False
        self.ssl_context.verify_mode = ssl.CERT_NONE
        self.ssl_context.protocol = ssl.PROTOCOL_TLS

    def can_handle(self, address):
        return address.startswith("ssl://")

    def schema(self):
        return "ssl"

    def get_url_template(self):
        return "ssl://{0}:{1}"

    def _make_socket(self, address):    # throws ValueError
        fd, host, port = TcpTransport._make_socket(self, address)
        return self.ssl_context.wrap_socket(fd), host, port
