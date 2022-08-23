from greenlet import GreenletExit
from eventlet import spawn as eventlet_spawn, \
                     Timeout as EventletTimeout, \
                     getcurrent, \
                     Queue as EventletQueue, \
                     sleep as eventlet_sleep
from eventlet.event import Event as _EventletEvent
from eventlet.green.socket import socket as EventletSocket
from eventlet.green.ssl import SSLContext as EventletSSLContext

# noinspection PyUnresolvedReferences,PyProtectedMember,PyPep8Naming
from eventlet.green.threading import RLock as EventletLock
from .base import BaseScheduler


class EventletEvent:
    def __init__(self):
        self._event = _EventletEvent()

    def is_set(self):
        return self._event.ready()

    def set(self):
        self._event.send(None)

    def clear(self):
        self._event.reset()

    def send(self, result):
        self._event.send(result)

    def send_exception(self, exception):
        return self._event.send_exception(exception)

    def wait(self, timeout = None):
        if timeout:
            with EventletTimeout(timeout):
                return self._event.wait()
        else:
            return self._event.wait()


class EventletScheduler(BaseScheduler):
    spawn_function = staticmethod(eventlet_spawn)
    sleep = staticmethod(eventlet_sleep)
    Event = EventletEvent
    Timeout = EventletTimeout
    Lock = EventletLock
    Exit = GreenletExit
    Queue = EventletQueue
    Socket = EventletSocket
    SSLContext = EventletSSLContext

    def get_current(self):
        return getcurrent()




