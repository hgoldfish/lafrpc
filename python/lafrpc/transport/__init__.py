from .base import BaseTransport

__all__ = ["BaseTransport", "avaliable_transports"]

avaliable_transports = {}

try:
    # noinspection PyUnresolvedReferences
    from .tcp import TcpTransport
    avaliable_transports["tcp"] = TcpTransport
    del TcpTransport
except ImportError:
    pass


try:
    # noinspection PyUnresolvedReferences
    from .ssl import SslTransport
    avaliable_transports["ssl"] = SslTransport
    del SslTransport
except ImportError:
    pass
