import itertools
import logging
import weakref
from typing import Optional

from lafrpc.network import VirtualDataChannel
from .base import RpcException, RpcDisconnectedException, RpcInternalException, RpcRemoteException, \
    RpcSerializationException, UseStream, Request, Response, is_exported
from .deferred import Signal, Deferred
from .serialization import BaseSerialization, SerializationException, DeserializationException
from .utils import RegisterServicesMixin, RpcProxy, DeferredRpcProxy, create_uuid

logger = logging.getLogger(__name__)

debug_protocol = False


def recv(connection, size):
    buf = b""
    while len(buf) < size:
        t = connection.recv(size - len(buf))
        if not t:
            raise RpcDisconnectedException()
        buf += t
    assert len(buf) == size
    return buf


def pack_request(serialization: BaseSerialization, request: Request, protocol_version: int):
    if not isinstance(request.id, bytes):
        request_id = request.id.encode("utf-8")
    else:
        request_id = request.id
    assert isinstance(request.raw_socket, bytes)
    data = [
        1,
        request_id,
        request.method_name,
        request.args,
        request.kwargs,
        request.header,
        request.channel,
        request.raw_socket,
    ]
    if protocol_version >= 2:
        data.append(request.one_way)

    return serialization.pack(data)


def pack_response(serialization: BaseSerialization, response: Response):
    if not isinstance(response.id, bytes):
        response_id = response.id.encode("utf-8")
    else:
        response_id = response.id
    assert isinstance(response.raw_socket, bytes)

    data = [
        2,
        response_id,
        response.result,
        response.exception,
        response.channel,
        response.raw_socket
    ]
    return serialization.pack(data)


def unpack_request_or_response(serialization: BaseSerialization, data: bytes):
    d = serialization.unpack(data)
    if 8 <= len(d) <= 9 and d[0] == 1:
        request = Request()
        request.id = d[1]
        request.method_name = d[2]
        request.args = d[3]
        request.kwargs = d[4]
        request.header = d[5]
        request.channel = d[6]
        request.raw_socket = d[7]
        if len(d) >= 9:
            request.one_way = d[8]
        if not request.is_ok():
            if debug_protocol:
                logger.debug("%r", d)
            raise DeserializationException("invalid packet.")
        return request
    elif len(d) == 6 and d[0] == 2:
        response = Response()
        response.id = d[1]
        response.result = d[2]
        response.exception = d[3]
        response.channel = d[4]
        response.raw_socket = d[5]
        if not response.is_ok():
            if debug_protocol:
                logger.debug("%r", d)
            raise DeserializationException("invalid packet.")
        return response
    else:
        raise DeserializationException("invalid packet.")


def wait_deferred(io_scheduler, df):
    event = io_scheduler.Event()
    df.add_callbacks(event.send, event.send_exception)
    return event.wait()


# noinspection PyUnusedLocal
def default_make_header_callback(method_name):
    return {}


# noinspection PyUnusedLocal
def default_auth_header_callback(header, method_name):
    return True


class Peer(RegisterServicesMixin):
    disconnected: Signal
    protocol_version = 1

    def __init__(self, name, channel, rpc):
        RegisterServicesMixin.__init__(self)
        self.channel = channel
        self.waiters = {}
        self.make_header_callback = default_make_header_callback
        self.auth_header_callback = default_auth_header_callback
        self.broken = False
        self.properties = {}

        assert bool(name)
        self._peer_name = name
        self.address = None

        self.rpc_ref = weakref.ref(rpc)
        self.lock = rpc.io_scheduler.Lock()
        self.disconnected = Signal(Peer)

        rpc.io_scheduler.spawn_with_name("{0}-handle_packet".format(name), self.handle_packet)

    def shutdown(self):
        with self.lock:
            if self.broken:
                return
            self.broken = True
            rpc = self.rpc_ref()
            if rpc:
                rpc.io_scheduler.kill_many("{0}-*".format(self._peer_name))
            for waiter_id, waiter in self.waiters.items():
                waiter.send(RpcDisconnectedException("Peer is disconnected."))
            self.waiters = {}
        self.channel.close()
        self.disconnected.emit(self)
        RegisterServicesMixin.clear_services(self)

    close = shutdown

    def as_proxy(self):
        return RpcProxy(self)

    def as_deferred_proxy(self):
        return DeferredRpcProxy(self)

    def is_ok(self):
        with self.lock:
            return not self.broken and self.rpc_ref() is not None and self.channel.is_ok()

    def is_active(self):
        with self.lock:
            return len(self.waiters) > 0

    def call(self, method_name, args, kwargs):
        if debug_protocol:
            logger.debug("Peer.call: %s(args=%r, kwargs=%r)", method_name, args, kwargs)
        if not self.is_ok():
            raise RpcDisconnectedException("Peer is disconnected.")
        if not self.channel.is_ok():
            raise RpcDisconnectedException("Channel has been shutdown.")
        try:
            return self._call(method_name, args, kwargs)
        except RpcDisconnectedException:
            self.close()
            raise
        except RpcException:
            raise
        except Exception:
            logger.exception("error occurs while calling RPC remote method.")
            raise RpcInternalException("error occurs while calling RPC remote method.")

    def _call(self, method_name, args, kwargs):
        stream_from_client: UseStream = None
        for param in itertools.chain(args, kwargs.values()):
            if isinstance(param, UseStream):
                stream_from_client = param
                break
        request = Request()
        request.method_name, request.args, request.kwargs = method_name, args, kwargs
        request.id = create_uuid().encode("utf-8")
        try:
            # noinspection PyNoneFunctionAssignment
            request.header = self.make_header_callback(method_name)
        except Exception:
            raise RpcInternalException("can not create header while calling remote method.")
        if not self.is_ok():
            raise RpcDisconnectedException("Peer is disconnected.")

        try:
            if stream_from_client:
                sub_channel1 = self.channel.make_channel()
                request.channel = sub_channel1.channel_number
                if debug_protocol:
                    logger.debug("%r, %r, %r", stream_from_client.prefer_raw_socket, self.rpc_ref(), self._peer_name)
                raw_socket = None
                if stream_from_client.prefer_raw_socket and self.rpc_ref() is not None and self._peer_name:
                    connection, connection_id = self.rpc_ref().make_raw_socket(self._peer_name)
                    if connection:
                        request.raw_socket = connection_id
                        raw_socket = connection
                    else:
                        if debug_protocol:
                            logger.debug("can not create raw socket to %s, maybe "
                                        "firewall exists or resist in different network.")
                # noinspection PyProtectedMember
                stream_from_client._init(self.rpc_ref(), UseStream.ClientSide | UseStream.ParamInRequest,
                                        sub_channel1, raw_socket)
            try:
                request_bytes = pack_request(self.rpc_ref().serialization, request, self.protocol_version)
            except SerializationException:
                raise RpcSerializationException(
                    "can not serialize request while calling remote method: {0}".format(method_name))
            try:
                self.channel.send_packet(request_bytes)
            except IOError:
                if debug_protocol:
                    logger.debug("can not send request to the other peer: %s", self.channel.error.description)
                raise

            if not self.is_ok():
                raise RpcDisconnectedException("rpc is gone.")
        finally:
            if stream_from_client:
                # noinspection PyProtectedMember
                stream_from_client._set_ready()

        io_scheduler = self.rpc_ref().io_scheduler
        waiter = io_scheduler.Event()
        with self.lock:
            self.waiters[request.id] = waiter

        response = None
        try:
            response = waiter.wait()
        except (io_scheduler.Timeout, io_scheduler.Exit):
            raise
        except Exception:
            message = "error occurs while waiting response of remote method: `{0}`".format(method_name)
            raise RpcInternalException(message)
        finally:
            try:
                with self.lock:
                    del self.waiters[request.id]
            except KeyError:
                pass

        if not response or not self.is_ok():
            raise RpcDisconnectedException("rpc is gone.")

        if isinstance(response, RpcException):
            raise response

        if response.exception is not None:
            raise response.exception

        if isinstance(response.result, UseStream):
            stream_from_server: UseStream = response.result
            try:
                sub_channel2 = self.channel.take_channel(response.channel)
                if sub_channel2 is None:
                    message = ("remote method `{0}` returns an UseStream object, " +
                            "but there is no new channel.").format(method_name)
                    raise RpcInternalException(message)
                raw_socket = None
                if response.raw_socket:
                    connection = self.rpc_ref().get_raw_socket(response.raw_socket)
                    if connection is None:
                        message = ("remote method `{0}` returns an UseStream(with raw socket), " +
                                "but there is no raw socket.").format(method_name)
                        raise RpcInternalException(message)
                    raw_socket = connection
                # noinspection PyProtectedMember
                stream_from_server._init(self.rpc_ref(), UseStream.ClientSide | UseStream.ValueOfResponse,
                                        sub_channel2, raw_socket)
            finally:
                # noinspection PyProtectedMember
                stream_from_server._set_ready()
        return response.result

    def handle_packet(self):
        # similar to close() but no kill greenlets.
        def clean():
            with self.lock:
                if self.broken:
                    return
                self.broken = True
                for waiter_id, _waiter in self.waiters.items():
                    _waiter.send(RpcDisconnectedException("channel is shutdown."))
                self.waiters = {}
            if self.channel.is_ok:
                self.channel.close()
            if debug_protocol:
                logger.debug("shutting down channel: %s", self.channel.error.description)
            if isinstance(self, weakref.ProxyType):
                # noinspection PyUnresolvedReferences
                self.disconnected.emit(self.handle_packet.__self__)
            else:
                self.disconnected.emit(self)

        if not self.is_ok():
            return clean()
        # noinspection PyPep8Naming
        Exit = self.rpc_ref().io_scheduler.Exit
        while True:
            try:
                packet = self.channel.recv_packet()
            except IOError:
                if debug_protocol:
                    logger.exception("channel is shutdown while receiving packet: %s", self.channel.error.description)
                return clean()
            except Exit:
                if debug_protocol:
                    logger.exception("channel is requested to shutdown while receiving packet.")
                return clean()
            except Exception:
                if debug_protocol:
                    logger.exception("caught unknown error.")
                return clean()
            if not self.rpc_ref():
                if debug_protocol:
                    logger.debug("rpc is gone.")
                return clean()
            try:
                obj = unpack_request_or_response(self.rpc_ref().serialization, packet)
            except DeserializationException:
                logger.exception("can not deserialize packet.")
                continue

            if isinstance(obj, Request):
                self.rpc_ref().io_scheduler.spawn_with_name(
                    "{0}-{1}".format(self._peer_name, obj.id),
                    self.handle_request, obj)
            elif isinstance(obj, Response):
                response = obj
                with self.lock:
                    waiter = self.waiters.get(response.id)
                if not waiter:
                    logger.warning("waiter is gone.")
                else:
                    waiter.send(response)
            else:
                logger.info("can not handle received packet.")

    def handle_request(self, request):
        if not self.is_ok():
            return
        io_scheduler = self.rpc_ref().io_scheduler
        # noinspection PyBroadException
        try:
            self._handle_request(request)
        except (RpcDisconnectedException, io_scheduler.Exit):
            pass
        except Exception:
            logger.exception("error occurs while handling request")

    def _handle_request(self, request):
        # noinspection PyBroadException
        try:
            if not self.auth_header_callback(request.header or {}, request.method_name):
                logger.info("abandon invalid packet from {0}".format(self._peer_name))
                return
        except Exception:
            logger.exception("can not auth request.header.")
            return

        if not self.is_ok():
            return

        for param in itertools.chain(request.args, request.kwargs.values()):
            if isinstance(param, UseStream):
                stream_from_client = param
                break
        else:
            stream_from_client = None

        response = Response()
        response.id = request.id

        if stream_from_client:
            try:
                sub_channel1 = self.channel.take_channel(request.channel)
                if sub_channel1 is None:
                    logger.error("client send an UseSteam parameter, but there is no channel.")
                    response.exception = RpcRemoteException()
                else:
                    raw_socket = None
                    if request.raw_socket:
                        connection = self.rpc_ref().get_raw_socket(request.raw_socket)
                        if connection is None:
                            logger.error("client send an UseSteam(with raw socket), but there is no raw socket.")
                            response.exception = RpcRemoteException()
                        else:
                            stream_from_client.raw_socket = connection
                    # noinspection PyProtectedMember
                    stream_from_client._init(self.rpc_ref(), UseStream.ServerSide | UseStream.ParamInRequest,
                                            sub_channel1, raw_socket)
            finally:
                # noinspection PyProtectedMember
                stream_from_client._set_ready()

        if response.exception is None:
            # noinspection PyBroadException
            try:
                result = self.lookup_method_and_call(request.method_name, request.args, request.kwargs, request.header)
                if not self.is_ok():
                    return
                if isinstance(result, Deferred):
                    response.result = wait_deferred(self.rpc_ref().io_scheduler, result)
                else:
                    response.result = result
            except self.rpc_ref().io_scheduler.Exit:
                return
            except RpcRemoteException as e:
                if debug_protocol:
                    logger.exception("the method throw a remote exception.")
                response.exception = e
            except RpcDisconnectedException:
                if debug_protocol:
                    logger.debug("the method throw a disconnected exception.")
                return
            except RpcInternalException:
                logger.warning("the method throw an internal exception.")
                response.exception = RpcRemoteException("internal error.")
            except Exception:
                logger.exception("error occurs while processing rpc call: {0}".format(request.method_name))
                response.exception = RpcRemoteException("unknown error.")

        stream_from_server = None
        try:
            if isinstance(response.result, UseStream):
                stream_from_server = response.result
                # 理论上使用 thread io scheduler 的情况下，有可能先运行 RpcFile 的 wait_for_ready() 函数
                # noinspection PyProtectedMember
                stream_from_server._init(self.rpc_ref(), UseStream.ServerSide | UseStream.ValueOfResponse, None, None)
                sub_channel2 = self.channel.make_channel()
                response.channel = sub_channel2.channel_number
                raw_socket = None
                if stream_from_server.prefer_raw_socket:
                    connection, connection_id = self.rpc_ref().make_raw_socket(self._peer_name)
                    if connection is not None:
                        response.raw_socket = connection_id
                        raw_socket = connection
                    else:
                        message = ("can not create raw socket to {0} " +
                                "maybe firewalls exist, or resist in different network.").format(self._peer_name)
                        logger.debug(message)
                # noinspection PyProtectedMember
                stream_from_server._init(self.rpc_ref(), UseStream.ServerSide | UseStream.ValueOfResponse,
                                        sub_channel2, raw_socket)

            response_bytes = pack_response(self.rpc_ref().serialization, response)
            try:
                self.channel.send_packet(response_bytes)
            except IOError:
                if debug_protocol:
                    logger.debug("can not send response packet to the peer: %s", self.channel.error.description)
                raise
        finally:
            if stream_from_server:
                # noinspection PyProtectedMember
                stream_from_server._set_ready()

    def lookup_method_and_call(self, method_name, args, kwargs, header):
        if not self.is_ok():
            raise RpcDisconnectedException("rpc is gone.")
        bound_function = None
        for attr_name in method_name.split("."):
            if bound_function is None:
                bound_function = self.services.get(attr_name, None)
                if bound_function:
                    continue

            if hasattr(bound_function, attr_name):
                method = getattr(bound_function, attr_name)
                if not is_exported(method):
                    if debug_protocol:
                        logger.warning("found method but is not exported: %s", method_name)
                    raise RpcInternalException("method not found: {0}".format(method_name))
                else:
                    bound_function = method
            else:
                for attr in dir(bound_function):
                    method = getattr(bound_function, attr)
                    if not is_exported(method):
                        continue
                    if getattr(method, "_laf_rpc_exported_name", "") == attr_name:
                        setattr(bound_function, attr_name, method)
                        bound_function = method
                        break
                else:
                    raise RpcInternalException("method not found: {0}".format(method_name))

        if bound_function is None:
            raise RpcInternalException("method not found: {0}".format(method_name))

        # noinspection PyProtectedMember
        self.rpc_ref()._set_current_peer_and_header(self, header)
        try:
            return bound_function(*args, **kwargs)
        finally:
            # noinspection PyProtectedMember
            self.rpc_ref()._del_current_peer_and_header()

    def make_channel(self) -> Optional[VirtualDataChannel]:
        if not self.is_ok():
            return None
        return self.channel.make_channel()

    def take_channel(self, channel_number: int) -> Optional[VirtualDataChannel]:
        if not self.is_ok():
            return None
        return self.channel.take_channel(channel_number)

    @property
    def name(self):
        return self._peer_name

    @name.setter
    def name(self, peer_name):
        self._peer_name = peer_name

    def set_property(self, name, value):
        with self.lock:
            self.properties[name] = value

    def get_property(self, name, default_value = None):
        with self.lock:
            if default_value is None:
                return self.properties[name]
            else:
                return self.properties.get(name, default_value)
