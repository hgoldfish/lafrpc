import hashlib
import io
import logging
import os
import struct
import threading
import socket
from .base import UseStream

__all__ = ["RpcDir"]

logger = logging.getLogger(__name__)
debugProtocol = False


def empty_generator():
    for i in range(0):
        yield i


def recv(connection, size):
    buf = b""
    while len(buf) < size:
        t = connection.recv(size - len(buf))
        if not t:
            raise socket.error()
        buf += t
    assert len(buf) == size
    return buf


class RpcDirFileEntry:
    path = ""
    size = 0
    mtime = 0
    ctime = 0
    atime = 0
    isdir = False

    __lafrpc_id__ = "RpcDirFileEntry"

    def __savestate__(self):
        return {
            "path": self.path,
            "size": self.size,
            "mtime": self.mtime,
            "ctime": self.ctime,
            "atime": self.atime,
        }

    def __setstate__(self, d):
        self.path = d["path"]
        self.size = d["size"]
        self.ctime = d["ctime"]
        self.mtime = d["mtime"]
        self.atime = d["atime"]
        return bool(self.path) and self.size >= 0 and self.ctime > 0 and self.mtime > 0 and self.atime > 0

    def __str__(self):
        return "<RpcDir_FileEntry: {0}, {1}>".format(self.path, self.size)


class RpcDirDirEntry:
    path = ""
    isdir = True

    __lafrpc_id__ = "RpcDirDirEntry"

    def __savestate__(self):
        return {"path": self.path}

    def __setstate__(self, d):
        self.path = d["path"]
        return bool(self.path)

    def __str__(self):
        return "<RpcDir_DirEntry: {0}>".format(self.path)


class RpcDir(UseStream):
    class Failed(Exception):
        pass
    BLOCK_SIZE = 1024 * 32

    name = ""
    entries = []
    size = 0
    isdir = True

    __lafrpc_id__ = "RpcDir"

    def __savestate__(self):
        return {
            "entries": self.entries,
            "size": self.size,
            "name": self.name,
        }

    def __setstate__(self, d):
        self.entries = d["entries"]
        self.size = d["size"]
        self.name = d["name"]
        return bool(self.name)

    def __init__(self, path = None):
        UseStream.__init__(self)
        self.path = path
        self.entries = []
        self.size = 0
        if self.path:
            self.name = os.path.basename(self.path)

    def populate(self):
        if not os.path.exists(self.path) or not os.path.isdir(self.path):
            return
        for root, dirnames, filenames in os.walk(self.path):
            for dirname in dirnames:
                dir_entry = RpcDirDirEntry()
                t = os.path.join(root, dirname)
                dir_entry.path = os.path.relpath(t, self.path)
                dir_entry.path = os.path.normpath(dir_entry.path)
                if os.name == "nt":
                    dir_entry.path = dir_entry.path.replace("\\", "/")
                self.entries.append(dir_entry)
            for filename in filenames:
                file_entry = RpcDirFileEntry()
                t = os.path.join(root, filename)
                file_entry.path = os.path.relpath(t, self.path)
                file_entry.path = os.path.normpath(file_entry.path)
                if os.name == "nt":
                    file_entry.path = file_entry.path.replace("\\", "/")
                file_entry.size = os.path.getsize(t)
                file_entry.mtime = os.path.getmtime(t)
                file_entry.atime = os.path.getatime(t)
                file_entry.ctime = os.path.getctime(t)
                self.entries.append(file_entry)
                self.size += file_entry.size
            yield None

    def send(self):
        if len(self.entries) == 0:
            return
        self.wait_for_ready()
        if self.raw_socket is not None:
            for _ in self.sendDir_socket():
                yield _
        else:
            for _ in self.sendDir_channel():
                yield _

    def recvDir(self, root):
        self.wait_for_ready()
        if self.raw_socket is not None:
            for _ in self.recvDir_socket(root):
                yield _
        else:
            for _ in self.recvDir_channel(root):
                yield _

    def sendDir_channel(self):
        if len(self.entries) == 0:
            return
        self.wait_for_ready()
        #接收者不会处理文件夹和空文件
        for index, entry in enumerate(self.entries):
            if entry.isdir or entry.size == 0:
                yield index, entry, emptyGenerator()
        while True:
            try:
                index = int(self.channel.recvPacket())
                if index == -1:
                    break
                subChannel = self.channel.makeChannel()
                self.channel.sendPacket(str(subChannel.getChannelNumber()).encode("ascii"))
            except (DisconnectedException, ValueError):
                raise RpcDir.Failed("发送文件时网络错误")
            entry = self.entries[index]
            doContinue = ValueContainer(True)
            yield index, entry, self.sendFile_channel(doContinue, entry, subChannel)
            if not doContinue.get():
                return

    def sendFile_channel(self, doContinue, entry, subChannel):
        count = 0
        hasher = hashlib.sha256()

        with subChannel:
            try:
                fin = io.open(os.path.join(self.path, entry.path), "rb")
            except IOError:
                raise RpcDir.Failed("发送文件时不能打开文件: %s", entry.path)
            with fin:
                while True:
                    try:
                        buf = fin.read(self.BLOCK_SIZE)
                    except IOError:
                        raise RpcDir.Failed("发送文件时不能读文件。可能是磁盘错误。")
                    if not buf:
                        break
                        hasher.update(buf)
                    try:
                        subChannel.sendPacket(buf)
                    except DisconnectedException:
                        doContinue.set(False)
                        raise RpcDir.Failed("发送文件时网络错误。")
                    count += len(buf)
                    yield len(buf)
            if count != entry.size:
                raise RpcDir.Failed("发送文件时文件提前结束。")
            #文件内容发送完毕，发送MD5值。
            if debugProtocol:
                logger.debug("文件发送完毕，md5是 %r。", md5.digest())
            try:
                subChannel.sendPacket(md5.digest())
            except DisconnectedException:
                doContinue.set(False)
                raise RpcDir.Failed("发送文件的MD5时网络错误。")

    def recvDir_channel(self, root):
        self.waitForPrepared()
        for index, entry in enumerate(self.entries):
            if isinstance(entry, RpcDir_DirEntry):
                dirpath = os.path.join(root, entry.path)
                if os.path.exists(dirpath):
                    if not os.path.isdir(dirpath):
                        raise RpcDir.Failed("接收文件时不能创建文件夹。发现同名文件。")
                else:
                    try:
                        os.makedirs(dirpath)
                    except IOError:
                        raise RpcDir.Failed("接收文件时不能创建文件夹。可能是磁盘已经满。")
                yield index, entry, emptyGenerator()
            else:
                fullPath = os.path.join(root, entry.path)
                parentDir = os.path.dirname(fullPath)
                if not os.path.exists(parentDir):
                    try:
                        os.makedirs(parentDir)
                    except IOError:
                        raise RpcDir.Failed("接收文件时不能创建文件夹。可能是磁盘已经满。")
                if entry.size == 0:
                    io.open(fullPath, "wb").close()
                    yield index, entry, emptyGenerator()
                else:
                    try:
                        self.channel.sendPacket(str(index).encode("ascii"))
                        channelNumber = int(self.channel.recvPacket())
                        subChannel = self.channel.getChannel(channelNumber)
                        if subChannel is None:
                            raise DisconnectedException()
                    except (DisconnectedException, ValueError, UnicodeError):
                        raise RpcDir.Failed("接收文件时网络错误")
                    yield index, entry, self.recvFile_channel(root, entry, subChannel)
        self.channel.sendPacket(b"-1")

    def recvFile_channel(self, root, entry, subChannel):
        count = 0
        md5 = hashlib.md5()
        subChannel.capacity = 16

        #必须保证上层调用者删除这个iterator时，subChannel同时被关闭。这个要求对于删除正在下载的文件这个功能很重要。
        with subChannel:
            fout = io.open(os.path.join(root, entry.path), "wb")
            with fout:
                #接着传输文件内容
                while True:
                    try:
                        buf = subChannel.recvPacket()
                    except DisconnectedException: #文件传输完毕发送方会关闭这个通道。
                        raise RpcDir.Failed("接收文件时网络错误")
                    try:
                        fout.write(buf)
                    except IOError:
                        logger.info("接收文件时不能写文件。可能是磁盘已经满。")
                        raise RpcDir.Failed("接收文件时不能写文件")
                    md5.update(buf)
                    count += len(buf)
                    yield len(buf)
                    if count >= entry.size:
                        break
            if count != entry.size:
                raise RpcDir.Failed("接收文件时文件提前结束")

            #最后，读取md5并进行比较。
            try:
                if debugProtocol:
                    logger.debug("文件接收完毕，md5是 %r。", md5.digest())
                digest = subChannel.recvPacket()
                if digest != md5.digest():
                    raise RpcDir.Failed("接收文件时发现MD5不一致。")
            except DisconnectedException:
                raise RpcDir.Failed("接收文件时不能读取MD5。")

            try:
                os.utime(os.path.join(root, entry.path), (entry.atime, entry.mtime))
            except OSError:
                pass
            except:
                logger.exception("设置文件的atime和mtime时发生错误。")

    def sendDir_socket(self):
        if len(self.entries) == 0:
            return
        self.waitForPrepared()
        #接收者不会处理文件夹
        for index, entry in enumerate(self.entries):
            if entry.isdir or entry.size == 0:
                yield index, entry, emptyGenerator()
        t = SenddirThread(self.path, self.entries, self.rawSocket)
        t.start()
        try:
            while True:
                try:
                    index, entry, senddirEntry = t.waitForProgress()
                except Finished:
                    return
                except socket.error:
                    raise RpcDir.Failed(self.trUtf8("发生网络错误。"))
                except:
                    logger.exception("发生莫名错误。")
                    raise RpcDir.Failed(self.trUtf8("发生未知错误。"))
                yield index, entry, self.sendFile_socket(senddirEntry)
        finally:
            t.shutdown()

    def recvDir_socket(self, root):
        if len(self.entries) == 0:
            return
        self.waitForPrepared()
        for index, entry in enumerate(self.entries):
            if entry.isdir:
                dirpath = os.path.join(root, entry.path)
                if os.path.exists(dirpath):
                    if not os.path.isdir(dirpath):
                        raise RpcDir.Failed("接收文件时不能创建文件夹。发现同名文件。")
                else:
                    try:
                        os.makedirs(dirpath)
                    except IOError:
                        raise RpcDir.Failed("接收文件时不能创建文件夹。可能是磁盘已经满。")
                yield index, entry, emptyGenerator()
            else:
                fullPath = os.path.join(root, entry.path)
                parentDir = os.path.dirname(fullPath)
                if not os.path.exists(parentDir):
                    try:
                        os.makedirs(parentDir)
                    except IOError:
                        raise RpcDir.Failed("接收文件时不能创建文件夹。可能是磁盘已经满。")
                if entry.size == 0:
                    io.open(fullPath, "wb", buffering = False).close()
                    yield index, entry, emptyGenerator()

        t = RecvdirThread(root, self.entries, self.rawSocket)
        t.start()
        try:
            while True:
                try:
                    index, entry, recvdirEntry = t.waitForProgress()
                except Finished:
                    return
                except socket.error:
                    raise RpcDir.Failed(self.trUtf8("发生网络错误。"))
                except eventlet.SystemExceptions:
                    raise
                except:
                    logger.exception("发生莫名错误。")
                    raise RpcDir.Failed(self.trUtf8("发生未知错误。"))
                yield index, entry, self.recvFile_socket(recvdirEntry)
        finally:
            t.shutdown()

    def sendFile_socket(self, senddirEntry):
        while True:
            try:
                senddirEntry.waitForFinished(0.2)
            except eventlet.Timeout:
                yield senddirEntry.getDelta()
            except IOError:
                raise RpcDir.Failed(self.trUtf8("读取文件时出错。"))
            except socket.error:
                raise RpcDir.Failed(self.trUtf8("发生网络错误。"))
            except eventlet.SystemExceptions:
                raise
            except:
                logger.exception("发生莫名错误。")
                raise RpcDir.Failed(self.trUtf8("发生未知错误。"))
            else:
                yield senddirEntry.getDelta()
                return

    def recvFile_socket(self, recvdirEntry):
        while True:
            try:
                recvdirEntry.waitForFinished(0.2)
            except eventlet.Timeout:
                yield recvdirEntry.getDelta()
            except RemoteError:
                raise RpcDir.Failed(self.trUtf8("对方发生错误。"))
            except IOError:
                raise RpcDir.Failed(self.trUtf8("写入文件时出错。"))
            except socket.error:
                raise RpcDir.Failed(self.trUtf8("发生网络错误。"))
            except eventlet.SystemExceptions:
                raise
            except:
                logger.exception("发生莫名错误。")
                raise RpcDir.Failed(self.trUtf8("发生未知错误。"))
            else:
                yield recvdirEntry.getDelta()
                return


class SenddirEntry: #也被用于RecvdirEntry
    def __init__(self):
        self.finished = eventlet.Event()
        self.previousSentBytes = 0
        self._sentBytes = 0
        self.worker = None

    def waitForFinished(self, timeout):
        if self.worker is not None:
            self.worker.touch()
        with eventlet.Timeout(timeout):
            self.finished.wait()

    def notifyFinished(self, exc = None):
        if exc is None:
            threading_invoke.callMethodInMainThread(self.finished.send, None)
        else:
            threading_invoke.callMethodInMainThread(self.finished.send_exception, exc)

    def getDelta(self):
        totalSent = self.sentBytes
        delta = totalSent - self.previousSentBytes
        self.previousSentBytes = totalSent
        return delta

    def setWorker(self, worker):
        self.worker = worker

    def getSentBytes(self):
        if self.worker is None:
            return self._sentBytes
        return self.worker.sentBytes

    def setSentBytes(self, sentBytes):
        self._sentBytes = sentBytes

    sentBytes = property(getSentBytes, setSentBytes)


class Finished(Exception):
    def __init__(self, exc = None):
        Exception.__init__(self)
        self.exc = exc


class RemoteError(Exception):
    pass
