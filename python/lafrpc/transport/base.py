import logging
import os
import socket
import time
import weakref
from typing import Optional

from ..network import DataChannel, SocketDataChannel

logger = logging.getLogger(__name__)


class BaseTransport:
    is_secure = False
    name = "base"

    def __init__(self, rpc):
        self.rpc_ref = weakref.ref(rpc)
        # TODO close timeout connections.
        self.raw_connections = {}
        self.lock = rpc.io_scheduler.Lock()
        self.debug_protocol = False

    def recvall(self, connection: socket.socket, size: int) -> bytes:
        buf = b""
        while len(buf) < size:
            t = connection.recv(size - len(buf))
            if not t:
                raise IOError()
            buf += t
        assert len(buf) == size
        return buf

    def handle_request(self, request: socket.socket, remote_address: str) -> bool:
        try:
            header = self.recvall(request, 2)
        except IOError:
            logger.debug("can not receive request header.")
            return False
        rpc = self.rpc_ref()
        if rpc is None:
            return False

        if header == b"\x4e\x67":
            channel = SocketDataChannel(request, SocketDataChannel.NegativePole, rpc.io_scheduler)
            self.setup_channel(request, channel)
            # noinspection PyBroadException
            try:
                rpc.prepare_peer(channel, peer_address=remote_address)
            except socket.error:
                logger.exception("request is disconnected while prepare peer.")
                return False
            except:
                logger.exception("prepare_peer is failed.")
                return False
        elif header == b"\x33\x74":
            try:
                connection_id = self.recvall(request, 16)
                request.sendall(b"\xf3\x97")
            except (socket.error, IOError):
                logger.info("request is disconnected while prepare peer.")
                return False
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
            logger.info("invalid connection that sent invalid magic number.")
            return False
        return True

    def connect(self, remote_address: str, timeout: float = None) -> Optional[DataChannel]:
        request = self.create_connection(remote_address, timeout)
        if not request:
            return None

        try:
            request.sendall(b"\x4e\x67")
        except socket.error:
            if self.debug_protocol:
                logger.debug("调用服务端节点的远程方法时连接断开。未能初始化。")
            return None

        rpc = self.rpc_ref()
        if rpc is None:
            return None
        channel = SocketDataChannel(request, DataChannel.PositivePole, rpc.io_scheduler)
        self.setup_channel(request, channel)
        return channel

    def make_raw_socket(self, remote_address: str, timeout=None):
        request = self.create_connection(remote_address, timeout)
        if not request:
            return None, b""

        connection_id = os.urandom(16)
        try:
            request.sendall(b"\x33\x74" + connection_id)
        except socket.error:
            if self.debug_protocol:
                logger.debug("can not send header to {}.".format(remote_address))
            return None, b""
        return request, connection_id

    def get_raw_socket(self, connection_id):
        with self.lock:
            try:
                connection, timestamp = self.raw_connections.pop(connection_id)
                return connection
            except (ValueError, KeyError):
                return None

    def setup_channel(self, request: socket.socket, channel: SocketDataChannel):
        rpc = self.rpc_ref()
        if not rpc:
            return
        channel.keepalive_timeout = rpc.keepalive_timeout
        channel.max_packet_size = rpc.max_packet_size

    def create_connection(self, remote_address: str, timeout: float) -> Optional[socket.socket]:
        raise NotImplementedError()

    def can_handle(self, address):
        raise NotImplementedError()

    def schema(self):
        raise NotImplementedError()

    def get_url_template(self):
        raise NotImplementedError()
