import socket
import uuid
import logging
import ssl
from typing import Optional, Tuple

from .deferred import Signal
from .network import DataChannel
from .utils import RegisterServicesMixin
from .base import RpcException, RpcDisconnectedException, RpcSerializationException, RpcRemoteException, \
                  RpcInternalException
from .scheduler import BaseScheduler, available_schedulers
from .serialization import BaseSerialization, available_serializations
from .transport import BaseTransport, avaliable_transports
from .peer import Peer

logger = logging.getLogger(__name__)

debug_protocol = True

# TODO support these profiles:
# IOSchedulerProfiles = ("thread", "gevent", "eventlet")
# SerializationProfiles = ("json", "msgpack", "qdatastream", "pickle", "pickle_lite", "protobuf", "xml")
# TransportProfiles = ("tcp", "kcp", "http")


class RpcProfileException(RpcInternalException):
    pass


class RpcProfileManager(object):
    io_schedulers = {}
    serializations = {}

    @staticmethod
    def create(profile):
        if profile in RpcProfileManager.io_schedulers:
            return RpcProfileManager.io_schedulers[profile]()
        elif profile in RpcProfileManager.serializations:
            return RpcProfileManager.serializations[profile]()
        else:
            raise RpcException("profile is not registered")

    @staticmethod
    def create_io_scheduler(*names) -> Optional[BaseScheduler]:
        for name in names:
            if name in RpcProfileManager.io_schedulers:
                return RpcProfileManager.io_schedulers[name]()
        return None

    @staticmethod
    def create_serialization(*names) -> Optional[BaseSerialization]:
        for name in names:
            if name in RpcProfileManager.serializations:
                return RpcProfileManager.serializations[name]()
        return None

    @staticmethod
    def register(name, profile):
        success = False
        if issubclass(profile, BaseScheduler):
            RpcProfileManager.io_schedulers[name] = profile
            success = True
        if issubclass(profile, BaseSerialization):
            RpcProfileManager.serializations[name] = profile
            success = True
        if not success:
            raise RpcProfileException("invalid profile.")

    @staticmethod
    def check_conflict(profiles):
        if len(set(RpcProfileManager.io_schedulers.keys()) & set(profiles)) > 1:
            return True
        if len(set(RpcProfileManager.serializations.keys()) & set(profiles)) > 1:
            return True
        return False

    @staticmethod
    def init():
        for name, clazz in available_schedulers.items():
            RpcProfileManager.register(name, clazz)

        for name, clazz in available_serializations.items():
            RpcProfileManager.register(name, clazz)


RpcProfileManager.init()


class Rpc(RegisterServicesMixin):
    RpcException = RpcException
    RpcDisconnectedException = RpcDisconnectedException
    RpcInternalException = RpcInternalException
    RpcRemoteException = RpcRemoteException
    RpcSerializationException = RpcSerializationException

    DefaultIOScheduler = "thread"
    DefaultSerialization = "pickle"

    timeout = 10.0
    peer_version = 1
    max_packet_size = 1024 * 64
    keepalive_timeout = 30.0

    new_peer: Signal

    def __init__(self, serialization: BaseSerialization, io_scheduler: BaseScheduler,
                 my_peer_name = None):
        RegisterServicesMixin.__init__(self)
        if not my_peer_name:
            self.my_peer_name = str(uuid.uuid4())
        else:
            self.my_peer_name = my_peer_name

        self.peers = {}
        self.server_address_list = []
        self.known_addresses = {}
        self.local_store = {}
        self.waiters = {}
        self.lock = io_scheduler.Lock()
        self.transports = []
        self.io_scheduler = io_scheduler
        assert isinstance(serialization, BaseSerialization)
        self.serialization = serialization
        self.new_peer = Signal(Peer)
        self.connecting_events = {}
        self.make_header_callback = None
        self.auth_header_callback = None
        for name, clazz in avaliable_transports.items():
            self.transports.append(clazz(self))
        serialization.register_class(RpcRemoteException)

    @classmethod
    def use(cls, *profile_names, my_peer_name = None):
        if RpcProfileManager.check_conflict(profile_names):
            raise RpcProfileException("conflict profiles: {0}".format(",".join(profile_names)))
        io_scheduler = RpcProfileManager.create_io_scheduler(*profile_names)
        serialization = RpcProfileManager.create_serialization(*profile_names)
        if io_scheduler is None or serialization is None:
            raise RpcInternalException(f"can not create rpc using {','.join(profile_names)}")
        rpc = cls(serialization, io_scheduler, my_peer_name)
        return rpc

    def set_my_peer_name(self, my_peer_name):
        with self.lock:
            self.my_peer_name = my_peer_name

    def load_key(self, certfile, keyfile, password = None):
        # noinspection PyUnresolvedReferences
        ssl_context = self.io_scheduler.SSLContext(ssl.PROTOCOL_TLSv1_1)
        ssl_context.load_default_certs()
        ssl_context.load_cert_chain(certfile, keyfile, password)
        ssl_context.check_hostname = False
        # noinspection PyUnresolvedReferences
        ssl_context.verify_mode = ssl.CERT_NONE
        for transport in self.transports:
            if transport.is_secure:
                transport.ssl_context = ssl_context

    def _find_transport(self, address) -> Optional[BaseTransport]:
        for transport in self.transports:
            if transport.can_handle(address):
                return transport
        return None

    @staticmethod
    def _make_worker_name(server_address):
        worker_name = "server-" + str(hash(server_address))
        return worker_name

    def start_servers(self, *server_address_list, block = False):
        result = []
        greenlets = []
        with self.lock:
            for server_address in server_address_list:
                if server_address in self.server_address_list:
                    result.append(True)
                    continue
                # noinspection PyBroadException
                try:
                    transport = self._find_transport(server_address)
                    if transport is None:
                        logger.debug("no transport for " + server_address)
                        result.append(False)
                    else:
                        worker_name = self._make_worker_name(server_address)
                        t = self.io_scheduler.spawn_with_name(worker_name, transport.start_server, server_address)
                        greenlets.append(t)
                        self.server_address_list.append(server_address)
                        result.append(True)
                except Exception:
                    result.append(False)
                    logger.exception("can not create server.")
        if block:
            for g in greenlets:
                g.join()
        return result

    def start_server(self, server_address, block = False):
        return self.start_servers(server_address, block = block)[0]

    def stop_servers(self, *server_address_list):
        with self.lock:
            if len(server_address_list) == 0:
                server_address_list = list(self.server_address_list)
            result = []
            for server_address in server_address_list:
                worker_name = self._make_worker_name(server_address)
                success = self.io_scheduler.kill(worker_name)
                result.append(success)
                self.server_address_list.remove(server_address)
        return result

    def stop_server(self, server_address):
        return self.stop_servers(server_address)[0]

    def handle_request(self, request: socket.socket, remote_address: str):
        transport = self._find_transport(remote_address)
        if transport is None:
            raise RpcInternalException()
        # noinspection PyUnresolvedReferences
        transport.handle_request(request, remote_address)

    def shutdown(self):
        self.stop_servers()
        with self.lock:
            for peer in list(self.peers.values()):
                if hasattr(peer, "close"):
                    peer.close()
            self.peers = {}
        self.io_scheduler.kill_all()
        self.clear_services()
    close = shutdown

    def join(self):
        for server_address in self.server_address_list:
            worker_name = self._make_worker_name(server_address)
            self.io_scheduler.join(worker_name)

    def make_raw_socket(self, peer_name):
        with self.lock:
            known_address = self.known_addresses.get(peer_name)
            if not known_address:
                if debug_protocol:
                    logger.debug("the address of %s is not known.", peer_name)
                return None, b""
        transport = self._find_transport(known_address)
        if not transport:
            return None, b""
        return transport.make_raw_socket(known_address)

    def take_raw_socket(self, peer_name, connection_id):
        with self.lock:
            peer_address = self.known_addresses.get(peer_name)
            if not peer_address:
                if debug_protocol:
                    logger.debug("the address of %s is not known.", peer_name)
                return None
        transport = self._find_transport(peer_address)
        if not transport:
            return None
        connection = transport.get_raw_socket(connection_id)
        if not connection and debug_protocol:
            logger.debug("can not find connection with %s id from %s.", connection_id, transport.name)
        return connection

    def connect(self, peer_name_or_address):
        peer_name = None
        peer_address = None
        with self.lock:
            if peer_name_or_address in self.peers:
                peer_name = peer_name_or_address
                peer = self.peers[peer_name]
                if peer.is_ok():
                    return peer
                else:
                    del self.peers[peer_name]
            if peer_name_or_address in self.known_addresses:
                peer_name = peer_name_or_address
                peer_address = self.known_addresses[peer_name_or_address]
            elif "://" in peer_name_or_address:
                peer_address = peer_name_or_address
                for peer in self.peers.values():
                    if peer.address == peer_address:
                        if peer.is_ok():
                            return peer
                        else:
                            del self.peers[peer.name]
                            break
            else:
                if debug_protocol:
                    logger.debug("unknown address for peer: {}".format(peer_name_or_address))
                return None

        with self.lock:
            if peer_address in self.connecting_events:
                event = self.connecting_events[peer_address]
                wait_event = True
            else:
                event = self.io_scheduler.Event()
                self.connecting_events[peer_address] = event
                wait_event = False

        if wait_event:
            event.wait(self.timeout)
            with self.lock:
                for peer in self.peers.values():
                    if peer.address == peer_address:
                        return peer
                else:
                    return None
        else:
            transport = self._find_transport(peer_address)
            channel = transport.connect(peer_address)
            if channel is None:
                return None
            try:
                peer = self.prepare_peer(channel, peer_name, peer_address)
            finally:
                event.set()
                with self.lock:
                    del self.connecting_events[peer_address]
            return peer

    def is_connected(self, peer_name):
        with self.lock:
            return peer_name in self.peers

    def is_connecting(self, peer_address):
        with self.lock:
            return peer_address in self.connecting_events

    def proxy(self, peer_name):
        with self.lock:
            peer = self.peers.get(peer_name)
            if peer is not None:
                return peer.as_proxy()
        peer = self.connect(peer_name)
        if peer is not None:
            return peer.as_proxy()
        else:
            return None

    # def kill_small_connection(self, its_peer_name: str, channel: DataChannel):
    #     if self.my_peer_name > its_peer_name:
    #         with self.lock:
    #             peer = self.peers.get(its_peer_name)
    #         if peer is not None:
    #             if debug_protocol:
    #                 logger.debug("{} peer is exists already.".format(its_peer_name))
    #             if peer.is_ok():
    #                 try:
    #                     channel.send_packet(b"haha")
    #                     channel.close()
    #                 except IOError:
    #                     if debug_protocol:
    #                         logger.debug("some error occurred.")
    #                     return None
    #                 return peer
    #             else:
    #                 with self.lock:
    #                     peer.close()
    #                     del self.peers[its_peer_name]
    #         channel.send_packet_async(b"gaga")
    #     else:
    #         flag = channel.recv_packet()
    #         if flag == b"haha":
    #             channel.close()
    #             with self.lock:
    #                 peer = self.peers[its_peer_name]
    #             if peer is None:
    #                 with self.lock:
    #                     if its_peer_name in self.waiters:
    #                         waiter = self.waiters[its_peer_name]
    #                     else:
    #                         waiter = self.io_scheduler.Event()
    #                         self.waiters[its_peer_name] = waiter
    #                 try:
    #                     waiter.wait(self.timeout)
    #                 except self.io_scheduler.Timeout:
    #                     if debug_protocol:
    #                         logger.debug("timeout")
    #                     return None
    #                 finally:
    #                     with self.lock:
    #                         try:
    #                             del self.waiters[its_peer_name]
    #                         except KeyError:
    #                             pass
    #                 peer = self.peers.get(its_peer_name)
    #                 if peer is None:
    #                     if debug_protocol:
    #                         logger.debug("can not connect.")
    #                     return None
    #             if not peer.is_ok():
    #                 peer.close()
    #                 with self.lock:
    #                     del self.peers[its_peer_name]
    #                 if debug_protocol:
    #                     logger.debug("can not connect.")
    #                 return None
    #             else:
    #                 return peer
    #         elif flag == b"gaga":
    #             pass
    #         else:
    #             assert its_peer_name not in self.peers
    #             channel.close()
    #             if debug_protocol:
    #                 logger.debug("unknown flag:" + repr(flag))
    #             return None

    def prepare_peer(self, channel, peer_name = None, peer_address = None):
        my_header = {"peer_name": self.my_peer_name, "version": self.peer_version}
        # noinspection PyBroadException
        try:
            channel.send_packet_async(self.serialization.pack(my_header))
            packet = channel.recv_packet()
            its_header = self.serialization.unpack(packet)
        except IOError:
            if debug_protocol:
                logger.debug("can not send/receive peer headers.")
            return None
        except Exception:
            logger.exception("can not initialize peer.")
            return None
        if peer_name is not None and peer_name != its_header["peer_name"]:
            if debug_protocol:
                logger.debug("peer {} return mismatched peer_name: {}".format(peer_name, its_header["peer_name"]))
            return None
        its_peer_name = its_header["peer_name"]

        peer = Peer(its_peer_name, channel, self)
        peer.set_services(self.get_services())
        if self.auth_header_callback:
            peer.auth_header_callback = self.auth_header_callback
        if self.make_header_callback:
            peer.make_header_callback = self.make_header_callback
        if peer_address:
            with self.lock:
                self.known_addresses[peer.name] = peer_address
            peer.address = peer_address
        # TODO connect peer's disconnected to self._remove_peer
        peer.disconnected.connect(self._remove_peer)
        with self.lock:
            self.peers[its_peer_name] = peer
            waiter = self.waiters.get(its_peer_name, None)
        if waiter is not None:
            waiter.set()
        # TODO emit new_peer event
        self.new_peer.emit(peer)
        return peer

    def _remove_peer(self, peer):
        with self.lock:
            for name, peer_ in self.peers.items():
                if peer is peer_:
                    del self.peers[name]
                    return

    def get_current_peer(self):
        with self.lock:
            # noinspection PyBroadException
            try:
                current_peer, _ = self.local_store.get(self.io_scheduler.get_current_id(), None)
                return current_peer
            except ValueError:
                return None
            except Exception:
                logger.exception("some error occurred.")
                return None

    def _set_current_peer_and_header(self, peer, header):
        with self.lock:
            self.local_store[self.io_scheduler.get_current_id()] = (peer, header)

    def _del_current_peer_and_header(self):
        with self.lock:
            try:
                del self.local_store[self.io_scheduler.get_current_id()]
            except KeyError:
                pass

    def get_rpc_header(self):
        with self.lock:
            # noinspection PyBroadException
            try:
                _, header = self.local_store.get(self.io_scheduler.get_current_id(), None)
                return header or {}
            except ValueError:
                return {}
            except Exception:
                logging.exception("some error occurred.")
                return {}
