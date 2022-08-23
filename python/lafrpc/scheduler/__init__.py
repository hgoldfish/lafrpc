from .base import BaseScheduler
from .thread import ThreadScheduler

__all__ = ["BaseScheduler", "ThreadScheduler", "available_schedulers"]


available_schedulers = {}

try:
    # noinspection PyUnresolvedReferences
    from .eventlet import EventletScheduler
    available_schedulers["eventlet"] = EventletScheduler
    del EventletScheduler
except ImportError:
    pass

try:
    # noinspection PyUnresolvedReferences
    from .gevent import GeventScheduler
    available_schedulers["gevent"] = GeventScheduler
    del GeventScheduler
except ImportError:
    pass

try:
    # noinspection PyUnresolvedReferences
    from .thread import ThreadScheduler
    available_schedulers["thread"] = ThreadScheduler
    del ThreadScheduler
except ImportError:
    pass

