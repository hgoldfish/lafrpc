import os
import io
import logging
import hashlib
from queue import Empty
from .base import UseStream, RpcException
from .serialization import register_class


logger = logging.getLogger(__name__)


@register_class
class RpcFile(UseStream):
    name = None  # 文件名:os.path.basename()
    size = 0  # 文件长度:os.path.getsize()
    atime = 0  # 最后访问时间:os.path.getatime()
    mtime = 0  # 最后修改时间:os.path.getmtime()
    ctime = 0  # 创建时间:os.path.getctime()
    hash = b""
    isdir = False  # 方便判断RpcFile或者RpcDir

    BLOCK_SIZE = 1024 * 32

    class Failed(RpcException):
        pass

    def __init__(self, path = None, do_hash = False):
        if path is None:
            return
        self.name = os.path.basename(path)
        self.size = os.path.getsize(path)
        self.atime = os.path.getatime(path)
        self.ctime = os.path.getctime(path)
        self.mtime = os.path.getmtime(path)
        if do_hash:
            m = hashlib.sha256()
            try:
                with io.open(path, "rb") as f:
                    while True:
                        buf = f.read(1024 * 8)
                        if not buf:
                            break
                        m.update(buf)
                    self.hash = m.digest()
            except IOError:
                logger.info("can not open file: %s", path)
                self.hash = b""
        else:
            self.hash = b""

    def write_to_path(self, path):
        # 接收网络传输过来的数据，写到path指向的文件里面。
        fout = io.open(path, "wb", buffering = 0)
        with fout:
            if self.size == 0:
                return   # and close the file, leaving a null file.
            self.wait_for_ready()
            logger.debug("use sendfile: %s", "yes" if self.raw_socket else 'no')
            if self.raw_socket is None:
                yield from self.write_to(fout, 0)
            else:
                yield from self._sendfile(self.raw_socket, fout, self.size)

        os.utime(path, (self.atime, self.mtime))

    def read_from_path(self, path):
        # 从path指向的文件读取数据，并发送到网络。
        if self.size == 0:
            return
        fin = io.open(path, "rb", buffering = 0)
        with fin:
            self.wait_for_ready()
            logger.debug("use sendfile: %s", "yes" if self.raw_socket else 'no')
            if self.raw_socket is None:
                yield from self.read_from(fin)
            else:
                yield from self._sendfile(fin, self.raw_socket, self.size)

    def write_to(self, fout, begin = 0):
        if self.size == 0:
            return
        self.wait_for_ready()
        count = begin
        self.channel.capacity = 32
        if self.hash:
            hasher = hashlib.sha256()
        else:
            hasher = None
        # 接着传输文件内容
        while True:
            try:
                buf = self.channel.recv_packet()
            except IOError:  # 文件传输完毕发送方会关闭这个通道。
                break
            if not buf:
                break
            if hasher:
                hasher.update(buf)
            try:
                fout.write(buf)
            except IOError:
                raise RpcFile.Failed("disk if full.")
            count += len(buf)
            yield len(buf)
            if count >= self.size:
                break
        if count != self.size:
            raise RpcFile.Failed("file is trunked.")
        if hasher:
            my_hash = hasher.digest()
            if my_hash != self.hash:
                raise RpcFile.Failed("file is corrupted")
        # noinspection PyBroadException
        try:
            os.utime(fout.name, (self.atime, self.mtime))
        except (AttributeError, OSError):
            pass
        except Exception:
            logger.exception("can not set file's mtime & atime.")

    def read_from(self, fin, no_wait:bool=False):
        if self.size == 0:
            return
        self.wait_for_ready()
        self.channel.capacity = 32

        count = 0

        # 接下来就开始发送文件内容了。
        while True:
            try:
                buf = fin.read(self.BLOCK_SIZE)
            except IOError:
                logger.info("can not read file.")
                break
            if not buf:
                break
            try:
                self.channel.send_packet(buf)
            except IOError:
                logger.debug("can not write channel.")
                break
            count += len(buf)
            yield len(buf)
        if count != self.size:
            raise RpcFile.Failed("can not read file.")

        # ensure all data sent.
        try:
            self.channel.recv_packet(no_wait=no_wait)
        except Empty:
            pass
        except IOError:
            pass

    def _sendfile(self, source, target, total_bytes_to_send):
        if self.size == 0:
            return

        hasher = None
        if hasattr(source, "recv"):
            read_source = source.recv
        else:
            read_source = source.read
        if hasattr(target, "sendall"):
            write_target = target.sendall
        else:
            write_target = target.write
            if self.hash:
                hasher = hashlib.sha256()
        if hasattr(target, "recv"):
            read_target = target.recv
        else:
            read_target = target.read

        count = 0
        while True:
            try:
                buf = read_source(min(self.BLOCK_SIZE, total_bytes_to_send - self.sent_bytes))
            except IOError:
                logger.info("can not read source.")
                break
            if not buf:
                break
            if hasher:
                hasher.update(buf)
            try:
                write_target(buf)
            except IOError:
                logger.info("can not write target.")
                break
            count += len(buf)
            yield len(buf)
        if count != self.size:
            raise RpcFile.Failed("can not read file.")
        if hasher:
            my_hash = hasher.digest()
            if my_hash != self.hash:
                raise RpcFile.Failed("file is corrupted")

        # ensure all data sent.
        try:
            read_target(1)
        except IOError:
            pass

    def __getstate__(self):
        return {
            "name": self.name,
            "size": self.size,
            "atime": self.atime,
            "mtime": self.mtime,
            "ctime": self.ctime,
            "hash": self.hash,
        }

    def __setstate__(self, d):
        self.name = d["name"]
        self.size = d["size"]
        self.atime = d["atime"]
        self.mtime = d["mtime"]
        self.ctime = d["ctime"]
        self.hash = d["hash"]
        return True

    __lafrpc_id__ = "RpcFile"
