import struct
import logging
import weakref
import socket
import uuid
import time
from typing import Dict

from lafrpc.scheduler import BaseScheduler

__all__ = ["DataChannel", "SocketDataChannel", "VirtualDataChannel"]

logger = logging.getLogger(__name__)

debug_protocol = False


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


class DataChannel:
    PositivePole = 1
    NegativePole = -1
    DataChannelNumber = 1
    CommandChannelNumber = 0

    def __init__(self, pole, io_scheduler: BaseScheduler):
        self.channels: Dict[int, weakref.ref] = {}
        assert pole in (DataChannel.PositivePole, DataChannel.NegativePole)
        self.pole = pole
        if pole == DataChannel.PositivePole:
            self.next_channel_number = 2
        else:
            self.next_channel_number = 2 ** 32 - 1
        self.broken = False
        self.pending_channels = io_scheduler.Queue()
        self.receiving_queue = io_scheduler.Queue()
        self.capacity = 0xff
        self.go_through = io_scheduler.Event()
        self.go_through.set()
        self.io_scheduler = io_scheduler
        self.lock = io_scheduler.Lock()

        self._max_packet_size = 1024 * 64
        self._payload_size_hint = 1400
        self._name = ""

    def __repr__(self):
        pattern = "<{2} (name = {0}, state = {1})>, "
        if not self.broken:
            state = "ok"
        else:
            state = "closed"
        return pattern.format(self._name, state, self.__class__.__name__)

    @property
    def header_size(self):
        raise NotImplementedError()

    @property
    def max_packet_size(self):
        return self._max_packet_size

    @max_packet_size.setter
    def max_packet_size(self, max_packet_size):
        with self.lock:
            self._max_packet_size = max_packet_size
            self._payload_size_hint = min(self._payload_size_hint, max_packet_size - self.header_size)

    @property
    def payload_size_hint(self):
        return self._payload_size_hint

    @payload_size_hint.setter
    def payload_size_hint(self, payload_size_hint):
        with self.lock:
            self._payload_size_hint = min(payload_size_hint, self._max_packet_size - self.header_size)

    @property
    def name(self):
        return self._name

    @name.setter
    def name(self, name):
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

    def take_channel(self, channel_number=None):
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
            if self.receiving_queue.empty() and self.broken:
                raise IOError()
        packet = self.receiving_queue.get()
        if isinstance(packet, IOError):
            # self.close() #为了处理对端主动close()，doReceive()不会销毁整个Channel.
            raise packet
        if self.receiving_queue.qsize() == (self.capacity - 1):
            command = pack_go_through_request()
            self._put_packet_in_sending_queue(0, command)
        return packet

    def send_packet(self, packet) -> None:
        with self.lock:
            if len(packet) > self.max_packet_size:
                logger.debug("packet size is too large than {}".format(self.max_packet_size))
                raise IOError()
        self.go_through.wait()
        self._send_packet(DataChannel.DataChannelNumber, packet)

    def send_packet_async(self, packet) -> None:
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
            channel = None
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
                    channel.close()
                    return False
        return True

    def _handle_command(self, packet):
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
                    if channel_number not in self.channels:
                        return True
                    channel = self.channels[channel_number]()
                if channel is None:
                    del self.channels[channel_number]
                    self._notify_channel_closed(channel_number)
            elif command["command"] == DESTROY_CHANNEL_REQUEST:
                channel_number = command["channel_number"]
                if debug_protocol:
                    logger.debug("对端要求关闭虚拟通道，号码是: %d", channel_number)
                self.take_channel(channel_number)
                if channel_number in self.channels:
                    self._clean_channel(channel_number, send_destroy_packet=False)
                    channel = self.channels[channel_number]()
                    if channel is not None:
                        channel._parent = None
                        channel.close()
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

    def _notify_channel_closed(self, channel_number):
        if self.broken:
            return
        command = pack_destroy_channel_request(channel_number)
        self._put_packet_in_sending_queue(0, command)

    def _make_channel(self, channel_number: int, pole: int):
        channel = VirtualDataChannel(self, channel_number, pole, self.io_scheduler)
        channel._max_packet_size = self._max_packet_size - 4
        channel._payload_size_hint = self._payload_size_hint - 4
        channel.capacity = self.capacity
        self.channels[channel_number] = weakref.ref(channel, _CleanChannelHelper(self))
        return channel

    def __enter__(self):
        return self

    # noinspection PyUnusedLocal
    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()

    def is_ok(self):
        return not self.broken

    def close(self):
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
                    channel.close()

    def get_peer_address(self):
        raise NotImplementedError()

    def _send_packet(self, channel_number, packet):
        raise NotImplementedError()

    def _put_packet_in_sending_queue(self, channel_number, packet):
        raise NotImplementedError()

    def _clean_channel(self, channel_number, send_destroy_packet):
        raise NotImplementedError()

    def _clean_sending_packet(self, channel_number, check_packet):
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
        self._keepalive_timeout = 10 * 1000
        self._keepalive_interval = 2 * 1000
        io_scheduler.spawn_with_name("sending-{0}".format(self.name), self._do_send)
        io_scheduler.spawn_with_name("receiving-{0}".format(self.name), self._do_receive)
        io_scheduler.spawn_with_name("keepalive-{0}".format(self.name), self._do_keepalive)
        self.last_keepalive_timestamp = int(time.time() * 1000)
        self.last_active_timestamp = int(time.time() * 1000)

    @property
    def header_size(self):
        return 8

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

    def close(self):
        with self.lock:
            if self.broken:
                return
            self.broken = True
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
        super(SocketDataChannel, self).close()

    def _send_packet(self, channel_number, packet):
        assert isinstance(packet, bytes), "the type of packet must be bytes."
        with self.lock:
            if self.broken:
                raise IOError()
        done = self.io_scheduler.Event()
        self.sending_queue.put((channel_number, packet, done))
        result = done.wait()  # can throw exception
        if not result:
            raise IOError("send packet got false result.")

    def _put_packet_in_sending_queue(self, channel_number, packet):
        with self.lock:
            if self.broken:
                return False
        self.sending_queue.put((channel_number, packet, None))

    def _do_send(self):
        while True:
            try:
                channel_number, packet, done = self.sending_queue.get()
            except self.io_scheduler.Exit:
                if debug_protocol:
                    logger.debug("等待发送队列数据到达前线程退出。")
                return self.close()
            if self.broken:
                if debug_protocol:
                    logger.debug("连接已经关闭。直接报告错误。通道号是%d。", channel_number)
                if done:
                    done.send_exception(IOError())
                return self.close()
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
                return self.close()
            except self.io_scheduler.Exit as e:
                if debug_protocol:
                    logger.debug("写数据时线程退出。")
                if done:
                    done.send_exception(IOError(e))
                return self.close()
            except Exception as e:
                if done:
                    done.send_exception(IOError(e))
                logger.exception("写数据时发生其它错误。")
                return self.close()
            if done:
                done.send(True)
            self.last_active_timestamp = time.time() * 1000

    def _do_receive(self):
        st = struct.Struct("!II")
        header_size = st.size
        while True:
            try:
                header = recvall(self.connection, header_size)
                packet_size, channel_number = st.unpack(header)
                if packet_size > self._max_packet_size:
                    logger.error("packet_size %d is larger than %d", packet_size, self._max_packet_size)
                    return self.close()
                packet = recvall(self.connection, packet_size)
                if len(packet) != packet_size:
                    logger.error("接收数据时提前结束。")
                    return self.close()
                if debug_protocol:
                    logger.debug("received packet: %d", header_size + len(packet))
            except (socket.error, IOError):
                if debug_protocol:
                    logger.debug("can not receive socket channel packet.")
                return self.close()
            except self.io_scheduler.Exit:
                if debug_protocol:
                    logger.debug("coroutine exit while receiving channel packet.")
                return self.close()
            except:
                logger.exception("接收数据时抛出未知异常。")
                return self.close()

            if not self._handle_incoming_packet(channel_number, packet):
                if debug_protocol:
                    logger.debug("can not handle incoming packet.")
                return self.close()
            self.last_active_timestamp = int(time.time() * 1000)

    def _do_keepalive(self):
        while True:
            self.io_scheduler.sleep(1.0)
            if not self._keepalive_timeout:
                return
            now = int(time.time() * 1000)
            if now - self.last_active_timestamp > self._keepalive_timeout:
                if debug_protocol:
                    logger.debug("socket channel is timeouted: %d", now - self.last_active_timestamp)
                self.close()
                return
            if now > self.last_keepalive_timestamp and now - self.last_keepalive_timestamp > self._keepalive_interval:
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
        self.close()

    @property
    def header_size(self):
        return 4

    def close(self):
        with self.lock:
            if self.broken:
                return
            self.broken = True

        for i in range(self.receiving_queue.getting()):
            self.receiving_queue.put(IOError())
        if self._parent:
            self._parent._clean_channel(self.channel_number, True)

    def is_ok(self):
        with self.lock:
            return not self.broken and self._parent is not None and self._parent.is_ok()

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

