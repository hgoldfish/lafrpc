import fnmatch
import logging
import queue
import socket
import ssl
import threading
import time

logger = logging.getLogger(__name__)


class ThreadExit(Exception):
    pass


class WorkerThread(threading.Thread):
    exit = False

    def __init__(self, func, args, kwargs):
        super(WorkerThread, self).__init__(daemon=False)
        self.func = func
        self.args, self.kwargs = args, kwargs
        self.exiting = False

    def run(self):
        # noinspection PyBroadException
        try:
            self.func(*self.args, **self.kwargs)
        except ThreadExit:
            pass
        except Exception as e:
            logger.exception("%r got an unhandled exception: %r", self.func, e)
        finally:
            del self.func, self.args, self.kwargs

    def kill(self):
        self.exiting = True


def thread_spawn(func, *args, **kwargs):
    t = WorkerThread(func, args, kwargs)
    t.start()
    return t


class ThreadTimeout(BaseException):
    def __init__(self, secs):
        self.secs = secs

    def __enter__(self):
        pass

    def __exit__(self, exc_type, exc_val, exc_tb):
        pass


class ThreadQueue(queue.Queue):
    def getting(self):
        # noinspection PyProtectedMember
        return len(self.not_empty._waiters)

    def return_forcely(self, item):
        with self.mutex:
            self.queue.insert(0, item)
            self.unfinished_tasks += 1
            self.not_empty.notify()

    def return_many_forcely(self, items):
        if not items:
            return
        with self.mutex:
            for item in reversed(items):
                self.queue.insert(0, item)
            self.unfinished_tasks += len(items)
            self.not_empty.notify()

    def return_(self, item):
        with self.not_full:
            if self.maxsize > 0:
                self.not_full.wait()
            self.queue.insert(item)
            self.unfinished_tasks += 1
            self.not_empty.notify()


class ThreadEvent(threading.Event):
    def __init__(self):
        threading.Event.__init__(self)
        self.result = None
        self.exception = None

    def send(self, result):
        self.result = result
        self.exception = None
        self.set()

    def send_exception(self, exception):
        self.result = None
        self.exception = exception
        self.set()

    def wait(self, timeout=None):
        threading.Event.wait(self, timeout)
        if self.exception is not None:
            raise self.exception
        else:
            return self.result


class BaseScheduler:
    spawn_function = staticmethod(thread_spawn)
    sleep = staticmethod(time.sleep)
    Event = ThreadEvent
    Lock = threading.RLock
    Exit = ThreadExit
    Timeout = ThreadTimeout
    Queue = ThreadQueue
    Socket = socket.socket
    SSLContext = ssl.SSLContext

    def __init__(self):
        self.tasks = {}

    def get(self, name: str):
        return self.tasks.get(name)

    def has(self, name: str):
        return name in self.tasks

    def spawn(self, func, *args, **kwargs):
        task = self.spawn_function(func, *args, **kwargs)
        task_name = "task@" + str(id(task))
        self.tasks[task_name] = task
        return task

    def spawn_with_name(self, name: str, func, *args, **kwargs):
        task = self.spawn_function(func, *args, **kwargs)
        self.tasks[name] = task
        return task

    def kill(self, task_name: str, remove=True):
        if task_name not in self.tasks:
            return False
        task = self.tasks.get(task_name)
        if remove:
            try:
                del self.tasks[task_name]
            except KeyError:
                pass

        if task:  # task is not exists or task is not running
            task.kill()
        return True

    def kill_all(self, remove=True):
        for task_id, task in self.tasks.items():
            if task:
                task.kill()
        if remove:
            self.tasks = {}
        return True

    def kill_many(self, pattern: str, remove=True):
        to_kills = []
        if remove:
            left_tasks = {}
            for task_name, task in self.tasks.items():
                if fnmatch.fnmatch(task_name, pattern):
                    to_kills.append(task)
                else:
                    left_tasks[task_name] = task
            self.tasks = left_tasks
        else:
            for task_name, task in self.tasks.items():
                if fnmatch.fnmatch(task_name, pattern):
                    to_kills.append(task)
        for task in to_kills:
            task.kill()
        return len(to_kills) > 0

    def join(self, task_name: str) -> bool:
        if task_name not in self.tasks:
            return False
        task = self.tasks[task_name]
        task.join()
        return True

    def get_current(self):
        raise NotImplementedError()

    def get_current_id(self) -> int:
        t = self.get_current()
        if t:
            return id(t)
        else:
            return 0
