import logging
import time
import os
import socket
import ipaddress
from urllib.parse import urlparse
from ..network import DataChannel, SocketDataChannel
from .base import BaseTransport


logger = logging.getLogger(__name__)


debug_protocol = True


def recv(connection, size):
    buf = b""
    while len(buf) < size:
        t = connection.recv(size - len(buf))
        if not t:
            raise IOError()
        buf += t
    assert len(buf) == size
    return buf


class TcpTransport(BaseTransport):
    def __init__(self, rpc):
        BaseTransport.__init__(self, rpc)
        # TODO close timeout connections.
        self.raw_connections = {}
        self.lock = rpc.io_scheduler.Lock()

    def can_handle(self, address: str) -> bool:
        return address.startswith("tcp://")

    def schema(self) -> str:
        return "tcp"

    def get_url_template(self) -> str:
        return "tcp://{0}:{1}"

    def _make_socket(self, address: str) -> (socket.socket, str, int):    # throws ValueError
        result = urlparse(address)
        host, port = result.hostname, result.port
        port = int(port)
        rpc = self.rpc_ref()
        if not rpc:
            raise ValueError()
        ip = ipaddress.ip_address(host)
        if ip.version == 4:
            family = socket.AF_INET
        elif ip.version == 6:
            family = socket.AF_INET6
        else:
            raise ValueError("unknown ip address family:" + host)
        return rpc.io_scheduler.Socket(family, socket.SOCK_STREAM), host, port

    def start_server(self, server_address: str) -> bool:
        try:
            server_socket, host, port = self._make_socket(server_address)
        except ValueError:
            logger.error("{0} is not an valid tcp address.".format(server_address))
            return False
        server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            server_socket.bind((host, port))
        except OSError:
            logger.error("can not bind to {0}:{1}".format(host, port))
            return False
        server_socket.listen(100)
        while True:
            try:
                request, remote_address = server_socket.accept()
                remote_address = self.get_url_template().format(*remote_address)
            except socket.error:
                logger.exception("can not accept new connections.")
                return False
            if self.rpc_ref() is None:
                return True
            self.rpc_ref().io_scheduler.spawn(self.handle_request, request, remote_address)

    def handle_request(self, request: socket.socket, remote_address: str):
        try:
            header = recv(request, 2)
        except IOError:
            logger.debug("can not receive request header.")
            return
        rpc = self.rpc_ref()
        if rpc is None:
            return

        if header == b"\x4e\x67":
            channel = SocketDataChannel(request, SocketDataChannel.NegativePole, rpc.io_scheduler)
            self.setup_channel(request, channel)
            # noinspection PyBroadException
            try:
                rpc.prepare_peer(channel, peer_address = remote_address)
            except socket.error:
                logger.exception("request is disconnected while prepare peer.")
                return
            except Exception:
                logger.exception("prepare_peer is failed.")
                return
        elif header == b"\x33\x74":
            try:
                connection_id = recv(request, 16)
                request.sendall(b"\xf3\x97")
            except (socket.error, IOError):
                logger.info("request is disconnected while prepare peer.")
                return
            try:
                # hack for eventlet and gevent socket
                fd = request.fd
                fd.setblocking(True)
            except AttributeError:
                # hack for plain python socket
                try:
                    request.setblocking(True)
                except AttributeError:
                    pass
            with self.lock:
                self.raw_connections[connection_id] = (request, time.time())
        else:
            logger.info("接收到非法的连接。")

    def connect(self, remote_address, timeout = None):
        try:
            request, host, port = self._make_socket(remote_address)
        except ValueError:
            logger.error("{0} is not an valid tcp address.".format(remote_address))
            return None

        try:
            request.settimeout(timeout)
            request.connect((host, port))
        except socket.error:
            message = "can not connect to remote host: {}".format(remote_address)
            if debug_protocol:
                logger.debug(message)
            return None

        try:
            request.sendall(b"\x4e\x67")
        except socket.error:
            if debug_protocol:
                logger.debug("调用服务端节点的远程方法时连接断开。未能初始化。")
            return None
        rpc = self.rpc_ref()
        if rpc is None:
            return None
        channel = SocketDataChannel(request, DataChannel.PositivePole, rpc.io_scheduler)
        self.setup_channel(request, channel)
        return channel

    def make_raw_socket(self, remote_address, timeout = None):
        try:
            request, host, port = self._make_socket(remote_address)
        except ValueError:
            message = "{0} is not an valid tcp address.".format(remote_address)
            logger.error(message)
            return None, b""
        try:
            request.settimeout(timeout)
            request.connect((host, port))
        except socket.error:
            message = "can not connect to {}.".format(remote_address)
            if debug_protocol:
                logger.debug(message)
            return None, b""
        connection_id = os.urandom(16)
        try:
            request.sendall(b"\x33\x74" + connection_id)
        except socket.error:
            message = "can not send header to {}.".format(remote_address)
            if debug_protocol:
                logger.debug(message)
            return None, b""
        return request, connection_id

    def get_raw_socket(self, connection_id):
        with self.lock:
            try:
                connection, timestamp = self.raw_connections.pop(connection_id)
                return connection
            except (ValueError, KeyError):
                return None

    def setup_channel(self, request, channel: SocketDataChannel):
        rpc = self.rpc_ref()
        if not rpc:
            return
        channel.keepalive_timeout = rpc.keepalive_timeout
        channel.max_packet_size = rpc.max_packet_size
