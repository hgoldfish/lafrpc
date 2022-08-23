import functools
import weakref
import logging


__all__ = ["UseStream", "exported", "is_exported", "TransferTimeout", "ConnectTimeOut",
           "RpcException", "RpcDisconnectedException", "RpcRemoteException",
           "RpcSerializationException", "RpcInternalException", "Request", "Response"]

logger = logging.getLogger(__name__)


ConnectTimeOut = 3  # 局域网的速度一般比较快，所以要求在3秒内响应客户端请求与数据传输
TransferTimeout = 30  # 要求在30秒内完成一个操作。这个时间不包括后续的UseSocket操作时间。


class UseStream:
    """当调用结束后，需要继续使用连接的参数类型继承这个接口。这个体系里面有三个角色。
    一是RPC本身，负责协议的传输与序列化。当它检测到UseStream接口，就启动服务提供者。
    二是服务提供者，负责协议的处理。比如文件传输协议。提供了文件传输时的暂停、续传等功能。
    三是上层应用，一般是提供一些具体的运行参数，或者提供一些回调函数，当服务提供者发生某些事件的时候就通知它。
    UseStream这个接口完全由服务提供者来实现。"""

    ServerSide = 0x01
    ClientSide = 0x02
    ParamInRequest = 0x04
    ValueOfResponse = 0x08

    channel = None
    place = ServerSide | ValueOfResponse
    prefer_raw_socket = False
    raw_socket = None
    ready = None
    rpc_ref = None

    def _init(self, rpc, place, channel, raw_socket):
        # this function called twice internal
        self.rpc_ref = weakref.ref(rpc)
        if self.ready is None:
            self.ready = rpc.io_scheduler.Event()
        # place指示这个UseStream类型的使用场景。channel则是供节点双方通信的通道。
        self.place = place
        self.channel = channel
        self.raw_socket = raw_socket

    def _set_ready(self):
        # 准备开始协议控制。这个方法会在通知上层应用之前调用。
        self.ready.set()

    def wait_for_ready(self):
        # this function is for derived class
        self.ready.wait()


class RpcException(Exception):
    pass


class RpcDisconnectedException(RpcException):
    pass


class RpcInternalException(RpcException):
    pass


class RpcRemoteException(RpcException):
    def __init__(self, message = ""):
        self.message = message

    def __getstate__(self):
        return {"message": self.message}

    def __setstate__(self, state):
        self.message = state.get("message")
        return isinstance(self.message, str)

    __lafrpc_id__ = "RpcRemoteException"


class RpcSerializationException(RpcException):
    pass


class Request:
    """RPC提交给服务端的请求的类型。"""

    id = b""
    method_name = ""
    args = tuple()
    kwargs = dict()
    header = dict()
    channel = 0
    raw_socket = b""


    def is_ok(self):
        if not self.method_name or not self.id:
            logger.debug("Request.method_name or id is empty.")
            return False
        if not isinstance(self.id, bytes):
            logger.debug("Request.id is not bytes.")
            return False
        if not isinstance(self.method_name, str):
            logger.debug("Request.method_name is not string.")
            return False
        if len(self.method_name) > 1024:
            logger.debug("Request.method_name is too long.")
            return False
        if len(self.id) > 64:
            logger.debug("Request.id is too long.")
            return False

        if self.args is not None:
            if not isinstance(self.args, (tuple, list)):
                logger.debug("Request.args is not list.")
                return False
        if self.kwargs is not None:
            if not isinstance(self.kwargs, dict):
                logger.debug("Request.kwargs is not dict.")
                return False
        if self.header is not None:
            if not isinstance(self.header, dict):
                logger.debug("Request.header is not dict")
                return False
        if self.channel is not None and self.channel != 0:
            if not isinstance(self.channel, int):
                logger.debug("Request.channel is not integer.")
                return False
        if self.raw_socket is not None:
            if not isinstance(self.raw_socket, (bytes, str)):
                logger.debug("Request.raw_socket is not either bytes or string.")
                return False
        return True


class Response:
    """RPC调用后从服务端返回的类型。"""
    id = b""
    result = None
    exception = None
    channel = 0
    raw_socket = b""

    def is_ok(self):
        if not self.id:
            logger.debug("Response.id is empty.")
            return False
        if not isinstance(self.id, bytes):
            logger.debug("Response.id is not bytes.")
            return False
        if len(self.id) > 64:
            logger.debug("Response.id is too long.")
            return False
        if self.exception is not None and not isinstance(self.exception, RpcRemoteException):
            logger.debug("Response.exception is invalid.")
            return False
        if not isinstance(self.channel, int) or self.channel < 0:
            logger.debug("Response.channel is not unsigned int.")
            return False
        if not isinstance(self.raw_socket, bytes):
            logger.debug("Response.raw_socket is not bytes.")
            return False
        return True


def exported(func_or_name):
    """一个标注，用于标识对象的方法将被导出到Rpc。使用方法：
    class MyService:
        @exported
        def echo(self, s):
            return s

    :param func: exporting function
    """
    if callable(func_or_name):
        func = func_or_name
        func._laf_rpc_exported = True
        return func
    else:
        name = func_or_name
        def wrapper(func):
            func._laf_rpc_exported_name = name
            func._laf_rpc_exported = True
            return func
        return wrapper


def is_exported(func):
    """Rpc内部判断某个函数是否被导出到Rpc
    :param func: exporting function to check.
    """
    return getattr(func, "_laf_rpc_exported", False)

