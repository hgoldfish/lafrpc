import logging

from gevent.exceptions import InvalidSwitchError
from greenlet import GreenletExit
from gevent import spawn as gevent_spawn, Timeout, getcurrent, sleep as gevent_sleep
from gevent.event import Event
from gevent.queue import Queue as GeventQueue, Full, ItemWaiter
from gevent.lock import RLock as GeventLock
from .base import BaseScheduler

# noinspection PyUnresolvedReferences,PyProtectedMember,PyPep8Naming
from gevent.socket import socket as GeventSocket
# noinspection PyUnresolvedReferences
from gevent.ssl import SSLContext as GeventSSLContext


logger = logging.getLogger(__name__)


class GeventQueueWithGetting(GeventQueue):
    def getting(self):
        try:
            return len(self.getters)
        except AttributeError:
            return 0

    def return_(self, item):
        if self._maxsize == -1 or self.qsize() < self._maxsize:
            self.queue.insert(0, item)
            if getattr(self, "getters", None):
                self._schedule_unlock()
        elif self.hub is getcurrent(): # pylint:disable=undefined-variable
            # We're in the mainloop, so we cannot wait; we can switch to other greenlets though.
            # Check if possible to get a free slot in the queue.
            while getattr(self, "getters", []) and self.qsize() and self.qsize() >= self._maxsize:
                getter = getattr(self, "getters", []).popleft()
                getter.switch(getter)
            if self.qsize() < self._maxsize:
                self._put(item)
                return
            raise Full
        else:
            waiter = ItemWaiter(item, self)
            self.putters.append(waiter)
            try:
                if getattr(self, "getters", []):
                    self._schedule_unlock()
                result = waiter.get()
                if result is not waiter:
                    raise InvalidSwitchError("Invalid switch into Queue.put: %r" % (result, ))
            finally:
                try:
                    self.putters.remove(waiter)
                except ValueError:
                    pass

    def return_forcely(self, item):
        self.queue.insert(0, item)
        if getattr(self, "getters", []):
            self._schedule_unlock()

    def return_many_forcely(self, items):
        for item in items:
            self.queue.insert(0, item)
        if getattr(self, "getters", []):
            self._schedule_unlock()


class GeventEvent(Event):
    def __init__(self):
        Event.__init__(self)
        self.result = None
        self.exception = None

    def send(self, result):
        self.result = result
        self.exception = None
        self.set()

    def send_exception(self, exception):
        self.result = None
        self.exception = exception

    def wait(self, timeout=None):
        Event.wait(self, timeout)
        if self.exception:
            raise self.exception
        else:
            return self.result


class GeventScheduler(BaseScheduler):
    spawn_function = staticmethod(gevent_spawn)
    sleep = staticmethod(gevent_sleep)
    Queue = GeventQueueWithGetting
    Event = GeventEvent
    Timeout = Timeout
    Lock = GeventLock
    Exit = GreenletExit
    Socket = GeventSocket
    SSLContext = GeventSSLContext

    def get_current(self):
        return getcurrent()
