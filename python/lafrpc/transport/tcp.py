import logging
import time
import os
import socket
import ipaddress
from typing import Optional
from urllib.parse import urlparse
from ..network import DataChannel, SocketDataChannel
from .base import BaseTransport


logger = logging.getLogger(__name__)


debug_protocol = True


class TcpTransport(BaseTransport):
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

    def create_connection(self, remote_address: str, timeout: float) -> Optional[socket.socket]:
        try:
            request, host, port = self._make_socket(remote_address)
        except ValueError:
            logger.error("{0} is not an valid tcp address.".format(remote_address))
            return None
        try:
            request.settimeout(timeout)
            request.connect((host, port))
        except socket.error:
            if self.debug_protocol:
                logger.debug("can not connect to {}.".format(remote_address))
            return None
        return request

    def start_server(self, server_address: str):
        # TODO 使用 create_server() 创建 BaseStreamServer，以便兼容 http transport.
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
