import struct
import logging
import weakref
import socket
import uuid
import time
from enum import Enum
from typing import Dict

from lafrpc.scheduler import BaseScheduler

__all__ = ["DataChannel", "SocketDataChannel", "VirtualDataChannel"]

logger = logging.getLogger(__name__)

debug_protocol = False

DefaultPacketSize = 1024 * 4


def recvall(connection, size):
    buf = b""
    while len(buf) < size:
        t = connection.recv(size - len(buf))
        if not t:
            raise IOError()
        buf += t
    assert len(buf) == size
    return buf


class _CleanChannelHelper:
    def __init__(self, parent):
        self.parent = weakref.ref(parent)

    def __call__(self, ref):
        if self.parent() is not None:
            try:
                # noinspection PyProtectedMember
                self.parent()._clean_channel(ref.channel_number)
            except AttributeError:
                pass


MAKE_CHANNEL_REQUEST = 1
CHANNEL_MADE_REQUEST = 2
DESTROY_CHANNEL_REQUEST = 3
SLOW_DOWN_REQUEST = 4
GO_THROUGH_REQUEST = 5
KEEPALIVE_REQUEST = 6


def pack_make_channel_request(channel_number):
    return struct.pack("!BI", MAKE_CHANNEL_REQUEST, channel_number)


def pack_channel_made_request(channel_number):
    return struct.pack("!BI", CHANNEL_MADE_REQUEST, channel_number)


def pack_destroy_channel_request(channel_number):
    return struct.pack("!BI", DESTROY_CHANNEL_REQUEST, channel_number)


def pack_slow_down_request():
    return struct.pack("!B", SLOW_DOWN_REQUEST)


def pack_go_through_request():
    return struct.pack("!B", GO_THROUGH_REQUEST)


def pack_keepalive_request():
    return struct.pack("!B", KEEPALIVE_REQUEST)


def unpack_command(bs):
    if len(bs) == struct.calcsize("!BI"):
        command, channel_number = struct.unpack("!BI", bs)
        if command not in (MAKE_CHANNEL_REQUEST, CHANNEL_MADE_REQUEST, DESTROY_CHANNEL_REQUEST):
            return None
        return {
            "command": command,
            "channel_number": channel_number,
        }
    elif len(bs) == struct.calcsize("!B"):
        command, = struct.unpack("!B", bs)
        if command not in (SLOW_DOWN_REQUEST, GO_THROUGH_REQUEST, KEEPALIVE_REQUEST):
            return None
        return {
            "command": command,
        }
    else:
        return None


class ChannelError(Enum):
    RemotePeerClosedError = (1, "The remote peer closed the connection")
    KeepaliveTimeoutError = (2, "The remote peer didn't send keepalive packet for a long time.")
    ReceivingError = (3, "Can not receive packet from remote peer")
    SendingError = (4, "Can not send packet to remote peer.")
    InvalidCommand = (5, "Can not parse packet header.")
    InvalidPacket = (6, "Can not parse command or unknown command.")
    UserShutdown = (7, "Programmer shutdown channel manually.")
    PluggedChannelError = (8, "The plugged channel has error.")
    PakcetTooLarge = (9, "The packet is too large.")

    UnknownError = (100, "Caught unknown error.")
    ProgrammingError = (101, "The QtNetwork programmer do a stupid thing.")
    NoError = (0, "")

    def __init__(self, code, description):
        self.code, self.description = code, description


class DataChannel:
    PositivePole = 1
    NegativePole = -1
    DataChannelNumber = 1
    CommandChannelNumber = 0

    def __init__(self, pole: int, io_scheduler: BaseScheduler):
        self.channels: Dict[int, weakref.ref] = {}
        assert pole in (DataChannel.PositivePole, DataChannel.NegativePole)
        self.pole = pole
        if pole == DataChannel.PositivePole:
            self.next_channel_number = 2
        else:
            self.next_channel_number = 2 ** 32 - 1
        self.error: ChannelError = ChannelError.NoError
        self.pending_channels = io_scheduler.Queue()
        self.receiving_queue = io_scheduler.Queue()
        self.capacity = 0xff
        self.go_through = io_scheduler.Event()
        self.go_through.set()
        self.io_scheduler = io_scheduler
        self.lock = io_scheduler.Lock()
        self._name = ""

    def __repr__(self) -> str:
        pattern = "<{0} (name = {1}, error = {2})>, "
        return pattern.format(self.__class__.__name__, self._name, self.error.description)

    @property
    def header_size(self) -> int:
        raise NotImplementedError()

    @property
    def max_packet_size(self) -> int:
        raise NotImplementedError()

    @property
    def max_payload_size(self) -> int:
        raise NotImplementedError()

    @property
    def payload_size_hint(self) -> int:
        raise NotImplementedError()

    @property
    def name(self) -> str:
        return self._name

    @name.setter
    def name(self, name: str):
        with self.lock:
            self._name = name

    def make_channel(self):
        with self.lock:
            channel_number = self.next_channel_number
            self.next_channel_number += self.pole
            channel = self._make_channel(channel_number, DataChannel.PositivePole)
        packet = pack_make_channel_request(channel_number)
        self._put_packet_in_sending_queue(DataChannel.CommandChannelNumber, packet)
        return channel

    def take_channel(self, channel_number: int=None):
        if not self.is_ok():
            return None
        if channel_number is None:
            return self.pending_channels.get()

        tmp = []
        found = None
        while not self.pending_channels.empty():
            channel = self.pending_channels.get()
            if channel.channel_number == channel_number:
                found = channel
                break
            else:
                tmp.append(channel)
        self.pending_channels.return_many_forcely(tmp)
        return found

    def recv_packet(self) -> bytes:
        with self.lock:
            if self.receiving_queue.empty() and self.error != ChannelError.NoError:
                raise IOError()
        packet = self.receiving_queue.get()
        if isinstance(packet, IOError):
            # self.close() #为了处理对端主动close()，doReceive()不会销毁整个Channel.
            raise packet
        if self.receiving_queue.qsize() == (self.capacity - 1):
            command = pack_go_through_request()
            self._put_packet_in_sending_queue(DataChannel.CommandChannelNumber, command)
        return packet

    def send_packet(self, packet: bytes):
        with self.lock:
            if len(packet) > self.max_packet_size:
                logger.debug("packet size is too large than {}".format(self.max_packet_size))
                raise IOError()
        self.go_through.wait()
        self._send_packet(DataChannel.DataChannelNumber, packet)

    def send_packet_async(self, packet: bytes):
        with self.lock:
            if len(packet) > self.max_packet_size:
                logger.debug("packet size is too large than {}".format(self.max_packet_size))
                raise IOError()
        self._put_packet_in_sending_queue(DataChannel.DataChannelNumber, packet)

    def _handle_incoming_packet(self, channel_number: int, payload: bytes):
        if channel_number == DataChannel.CommandChannelNumber:
            return self._handle_command(payload)
        elif channel_number == DataChannel.DataChannelNumber:
            if self.receiving_queue.qsize() == (self.capacity - 1):
                command = pack_slow_down_request()
                self._put_packet_in_sending_queue(DataChannel.CommandChannelNumber, command)
            self.receiving_queue.put(payload)
        else:
            channel: DataChannel = None
            with self.lock:
                if channel_number in self.channels:
                    channel = self.channels[channel_number]()
                    if not channel:
                        if debug_protocol:
                            logger.debug("读数据时检测到未知的虚拟通道号:%s。，丢弃数据。", channel_number)
                        try:
                            del self.channels[channel_number]
                        except KeyError:
                            pass
            if channel is not None:
                header_size = struct.calcsize("!I")
                try:
                    header, packet = payload[:header_size], payload[header_size:]
                    channel_number, = struct.unpack("!I", header)
                except struct.error:
                    logger.error("can not unpack channel's packet header.")
                    return False
                if not channel._handle_incoming_packet(channel_number, packet):
                    channel.close(ChannelError.InvalidPacket)
                    return False
        return True

    def _handle_command(self, packet: bytes) -> bool:
        # noinspection PyBroadException
        try:
            command = unpack_command(packet)
            if not command:
                logger.error("无法解析的命令: %r", command)
                return False

            if command["command"] == MAKE_CHANNEL_REQUEST:
                channel_number = command["channel_number"]
                if debug_protocol:
                    logger.debug("对端要求创建新的虚拟通道，号码是: %d", channel_number)
                channel = self._make_channel(channel_number, DataChannel.NegativePole)
                self.pending_channels.put(channel)
            elif command["command"] == CHANNEL_MADE_REQUEST:
                channel_number = command["channel_number"]
                if debug_protocol:
                    logger.debug("对端发来提醒，虚拟通道已经创建完毕: %d", channel_number)
                with self.lock:
                    try:
                        channel = self.channels[channel_number]()
                    except KeyError:
                        return True
                if channel is None:
                    del self.channels[channel_number]
                    self._notify_channel_closed(channel_number)
            elif command["command"] == DESTROY_CHANNEL_REQUEST:
                channel_number = command["channel_number"]
                if debug_protocol:
                    logger.debug("对端要求关闭虚拟通道，号码是: %d", channel_number)
                self.take_channel(channel_number)
                try:
                    channel = self.channels[channel_number]()
                except KeyError:
                    return True
                if channel is not None:
                    self._clean_channel(channel_number, send_destroy_packet=False)
                    channel._parent = None
                    channel.close(ChannelError.RemotePeerClosedError)
            elif command["command"] == SLOW_DOWN_REQUEST:
                if debug_protocol:
                    logger.debug("对端要求减速。")
                self.go_through.clear()
            elif command["command"] == GO_THROUGH_REQUEST:
                if debug_protocol:
                    logger.debug("对端放行。")
                self.go_through.set()
            elif command["command"] == KEEPALIVE_REQUEST:
                if debug_protocol:
                    logger.debug("对端心跳。")
            else:
                name = command["command"]
                logger.info("处理通道命令时遇到不认识的命令: {0}".format(name))
                return False
        except Exception:
            logging.exception("unknown error occured while handling channel command.")
            return False
        return True

    def _notify_channel_closed(self, channel_number: int):
        if self.error != ChannelError.NoError:
            return
        command = pack_destroy_channel_request(channel_number)
        self._put_packet_in_sending_queue(0, command)

    def _make_channel(self, channel_number: int, pole: int):
        channel = VirtualDataChannel(self, channel_number, pole, self.io_scheduler)
        channel.capacity = self.capacity
        self.channels[channel_number] = weakref.ref(channel, _CleanChannelHelper(self))
        return channel

    def __enter__(self):
        return self

    # noinspection PyUnusedLocal
    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close(ChannelError.UserShutdown)

    def is_ok(self):
        return self.error == ChannelError.NoError

    def close(self, reason: ChannelError):
        for i in range(self.receiving_queue.getting()):
            self.receiving_queue.put(IOError())
        for i in range(self.pending_channels.getting()):
            self.pending_channels.put(None)
        self.go_through.send(None)
        with self.lock:
            while self.channels:
                channel_number, ref = self.channels.popitem()
                channel = ref()
                if channel is not None:
                    channel._parent = None
                    channel.close(reason)

    def get_peer_address(self):
        raise NotImplementedError()

    def _send_packet(self, channel_number: int, packet: bytes):
        raise NotImplementedError()

    def _put_packet_in_sending_queue(self, channel_number: int, packet: bytes):
        raise NotImplementedError()

    def _clean_channel(self, channel_number: int, send_destroy_packet: bool):
        raise NotImplementedError()

    def _clean_sending_packet(self, channel_number: int, check_packet):
        raise NotImplementedError()


class SocketDataChannel(DataChannel):
    def __init__(self, connection, pole, io_scheduler):
        DataChannel.__init__(self, pole, io_scheduler)
        self.connection = connection
        try:
            self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except ValueError:
            pass
        self.sending_queue = io_scheduler.Queue()
        self.name = str(uuid.uuid4())
        self._max_payload_size = 1024 * 64 - 8
        self._payload_size_hint = 1400
        self._keepalive_timeout = 10 * 1000
        self._keepalive_interval = 2 * 1000
        io_scheduler.spawn_with_name("sending-{0}".format(self.name), self._do_send)
        io_scheduler.spawn_with_name("receiving-{0}".format(self.name), self._do_receive)
        io_scheduler.spawn_with_name("keepalive-{0}".format(self.name), self._do_keepalive)
        self.last_keepalive_timestamp = int(time.time() * 1000)
        self.last_active_timestamp = int(time.time() * 1000)

    @property
    def header_size(self) -> int:
        return 8

    @property
    def max_packet_size(self) -> int:
        return self._max_payload_size + 8

    @max_packet_size.setter
    def max_packet_size(self, size: int):
        if size <= 0:
            size = DefaultPacketSize
        elif size < 64:
            logger.warning("the max packet size of DataChannel should not lesser than 64.")
            return
        with self.lock:
            self._max_payload_size = size - 8
            self._payload_size_hint = min(self._payload_size_hint, self._max_payload_size)

    @property
    def max_payload_size(self) -> int:
        return self._max_payload_size

    @property
    def payload_size_hint(self) -> int:
        return self._payload_size_hint

    @payload_size_hint.setter
    def payload_size_hint(self, payload_size_hint: int):
        with self.lock:
            self._payload_size_hint = min(payload_size_hint, self._max_payload_size)

    @property
    def keepalive_timeout(self):
        return self._keepalive_timeout / 1000.0

    @keepalive_timeout.setter
    def keepalive_timeout(self, keepalive_timeout):
        self._keepalive_timeout = int(keepalive_timeout * 1000)

    @property
    def keepalive_interval(self):
        return self._keepalive_interval / 1000.0

    @keepalive_interval.setter
    def keepalive_interval(self, keepalive_interval):
        self._keepalive_interval = int(keepalive_interval * 1000)

    def get_peer_address(self):
        try:
            return self.connection.getpeername()
        except socket.error:
            raise IOError()

    def close(self, reason=ChannelError.UserShutdown):
        with self.lock:
            if self.error != ChannelError.NoError:
                return
            self.error = reason
        try:
            self.connection.close()
        except socket.error:
            pass

        while not self.sending_queue.empty():
            channel_number, packet, done = self.sending_queue.get()
            if done:
                done.send_exception(IOError())
        current = self.io_scheduler.get_current()
        if self.io_scheduler.get("sending-{0}".format(self.name)) is not current:
            self.io_scheduler.kill("sending-{0}".format(self.name))
        if self.io_scheduler.get("receiving-{0}".format(self.name)) is not current:
            self.io_scheduler.kill("receiving-{0}".format(self.name))
        if self.io_scheduler.get("keepalive-{0}".format(self.name)) is not current:
            self.io_scheduler.kill("keepalive-{0}".format(self.name))
        super(SocketDataChannel, self).close(reason)

    def _send_packet(self, channel_number, packet):
        assert isinstance(packet, bytes), "the type of packet must be bytes."
        with self.lock:
            if self.error != ChannelError.NoError:
                raise IOError()
        done = self.io_scheduler.Event()
        self.sending_queue.put((channel_number, packet, done))
        result = done.wait()  # can throw exception
        if not result:
            raise IOError("send packet got false result.")

    def _put_packet_in_sending_queue(self, channel_number, packet):
        with self.lock:
            if self.error != ChannelError.NoError:
                return False
        self.sending_queue.put((channel_number, packet, None))

    def _do_send(self):
        while True:
            try:
                channel_number, packet, done = self.sending_queue.get()
            except self.io_scheduler.Exit:
                if debug_protocol:
                    logger.debug("等待发送队列数据到达前线程退出。")
                assert self.error != ChannelError.NoError
                return
            if self.error != ChannelError.NoError:
                if debug_protocol:
                    logger.debug("连接已经关闭。直接报告错误。通道号是%d。", channel_number)
                if done:
                    done.send_exception(IOError())
                return
            # noinspection PyBroadException
            try:
                header = struct.pack(b"!II", len(packet), channel_number)
                buf = header + packet
                self.connection.sendall(buf)
            except socket.error as e:
                if debug_protocol:
                    logger.debug("写数据时连接已经中断。")
                # 以前这里和别的地方不一样，如果done is None，忽略socket错误。 if done: continue，但是后来我不明白为啥这样写。
                if done:
                    done.send_exception(IOError(e))
                return self.close(ChannelError.SendingError)
            except self.io_scheduler.Exit as e:
                if debug_protocol:
                    logger.debug("写数据时线程退出。")
                if done:
                    done.send_exception(IOError(e))
                assert self.error != ChannelError.NoError
                return
            except Exception as e:
                if done:
                    done.send_exception(IOError(e))
                logger.exception("写数据时发生其它错误。")
                return self.close(ChannelError.UnknownError)
            if done:
                done.send(True)
            self.last_keepalive_timestamp = int(time.time() * 1000)

    def _do_receive(self):
        st = struct.Struct("!II")
        header_size = st.size
        while True:
            # noinspection PyBroadException
            try:
                header = recvall(self.connection, header_size)
                payload_size, channel_number = st.unpack(header)
                if payload_size > self._max_payload_size:
                    logger.error("packet_size %d is larger than %d", payload_size, self._max_payload_size)
                    return self.close(ChannelError.PakcetTooLarge)
                payload = recvall(self.connection, payload_size)
            except (socket.error, IOError, OSError):
                if debug_protocol:
                    logger.info("can not receive socket channel packet.")
                return self.close(ChannelError.ReceivingError)
            except self.io_scheduler.Exit:
                if debug_protocol:
                    logger.debug("coroutine exit while receiving channel packet.")
                assert self.error != ChannelError.NoError
                return
            except Exception:
                logger.exception("caugth unexpected exception while receiving data channel packets.")
                return self.close(ChannelError.UnknownError)

            if len(payload) != payload_size:
                if debug_protocol:
                    logger.debug("the connection is closed.")
                return self.close(ChannelError.ReceivingError)
            if debug_protocol:
                logger.debug("received packet: %d", header_size + len(payload))

            if not self._handle_incoming_packet(channel_number, payload):
                if debug_protocol:
                    logger.debug("can not handle incoming packet.")
                return self.close(ChannelError.InvalidCommand)
            self.last_active_timestamp = int(time.time() * 1000)

    def _do_keepalive(self):
        while True:
            self.io_scheduler.sleep(1.0)
            now = int(time.time() * 1000)
            if now - self.last_active_timestamp > self._keepalive_timeout:
                if debug_protocol:
                    logger.debug("socket channel is timeouted: %d", now - self.last_active_timestamp)
                self.close(ChannelError.KeepaliveTimeoutError)
                return
            if now - self.last_keepalive_timestamp > self._keepalive_interval:
                # 虽然在 do_send() 时会更新，但是不一定发送成功。一旦发送不成功，就会往 sending_queue 里面堆太多 keepalive_request
                # 另一个更好的办法检测 sending_queue 的大小，在 cpp 版本里面就是这么干的。
                self.last_keepalive_timestamp = now
                self._put_packet_in_sending_queue(DataChannel.CommandChannelNumber, pack_keepalive_request())

    def _clean_channel(self, channel_number, send_destroy_packet):
        with self.lock:
            try:
                del self.channels[channel_number]
            except KeyError:
                return
        if send_destroy_packet:
            self._notify_channel_closed(channel_number)
        self._clean_sending_packet(channel_number, lambda packet: True)

    def _clean_sending_packet(self, sub_channel_number, sub_check_packet):
        reserved = []
        while not self.sending_queue.empty():
            channel_number, packet, done = self.sending_queue.get()
            if channel_number == sub_channel_number and sub_check_packet(packet):
                if done:
                    done.send_exception(IOError())
            else:
                reserved.append((channel_number, packet, done))
        for e in reserved:
            self.sending_queue.put(e)


class VirtualDataChannel(DataChannel):
    def __init__(self, parent: DataChannel, my_channel_number: int, pole: int, io_scheduler: BaseScheduler):
        DataChannel.__init__(self, pole, io_scheduler)
        self._parent, self.channel_number = parent, my_channel_number
        # TODO connect parent's disconnected to self.close

    def __del__(self):
        self.close(ChannelError.UserShutdown)

    @property
    def header_size(self):
        return 4

    @property
    def max_packet_size(self) -> int:
        if not self._parent:
            return 1400
        else:
            return self._parent.max_payload_size

    @property
    def max_payload_size(self) -> int:
        if not self._parent:
            return 1400
        else:
            return self._parent.max_payload_size - 4

    @property
    def payload_size_hint(self) -> int:
        if not self._parent:
            return 1400
        else:
            return self._parent.payload_size_hint - 4

    def close(self, reason=ChannelError.UserShutdown):
        with self.lock:
            if self.error != ChannelError.NoError:
                return
            self.error = reason

        for i in range(self.receiving_queue.getting()):
            self.receiving_queue.put(IOError())
        if self._parent:
            self._parent._clean_channel(self.channel_number, True)
        super(VirtualDataChannel, self).close(reason)

    def is_ok(self):
        with self.lock:
            return self.error == ChannelError.NoError and self._parent is not None and self._parent.is_ok()

    def get_peer_adddress(self):
        if self._parent:
            return self._parent.get_peer_address()
        else:
            raise IOError()

    def _send_packet(self, channel_number, packet):
        if not self.is_ok():
            raise IOError()
        header = struct.pack("!I", channel_number)
        self._parent._send_packet(self.channel_number, header + packet)

    def _clean_channel(self, channel_number, send_destroy_packet):
        if not self.is_ok():
            return
        with self.lock:
            if channel_number in self.channels:
                del self.channels[channel_number]
            else:
                return

        if send_destroy_packet:
            self._notify_channel_closed(channel_number)

        def check_packet(packet):
            header_size = struct.calcsize(b"!I")
            header = packet[:header_size]
            channel_number_, = struct.unpack(b"!I", header)
            return channel_number == channel_number_
        self._parent._clean_sending_packet(self.channel_number, check_packet)

    def _clean_sending_packet(self, sub_channnel_number, sub_check_packet):
        if not self.is_ok():
            return

        def check_packet(packet):
            header_size = struct.calcsize("!I")
            header, packet = packet[:header_size], packet[header_size:]
            channel_number, = struct.unpack("!I", header)
            if channel_number != sub_channnel_number:
                return False
            return sub_check_packet(packet)
        self._parent._clean_sending_packet(self.channel_number, check_packet)

    def _put_packet_in_sending_queue(self, channel_number, packet):
        if not self.is_ok():
            return
        packet = struct.pack("!I", channel_number) + packet
        self._parent._put_packet_in_sending_queue(self.channel_number, packet)
