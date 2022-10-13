import collections
import queue as __queue__
import sys

from greenlet import getcurrent

from gevent._hub_local import get_hub_noargs as get_hub
from gevent._waiter import Waiter
from gevent.exceptions import InvalidSwitchError
from gevent.timeout import Timeout

greenlet_init = lambda: None
_Full = Full = __queue__.Full
_Empty = Empty = __queue__.Empty

__all__ = ['ItemWaiter', 'Full', 'Queue']

def _safe_remove(deq, item):
    # For when the item may have been removed by
    # Queue._unlock
    try:
        deq.remove(item)
    except ValueError:
        pass

class ItemWaiter(Waiter): 
    __slots__ = (
        'item',
        'queue',
    )

    def __init__(self, item, queue):
        Waiter.__init__(self) # pylint:disable=undefined-variable
        self.item = item
        self.queue = queue

    def put_and_switch(self):
        self.queue._put(self.item)
        self.queue = None
        self.item = None
        return self.switch(self)

class Queue(object):
    __slots__ = (
        '_maxsize',
        'getters',
        'putters',
        'hub',
        '_event_unlock',
        'queue',
        '__weakref__',
    )

    def __init__(self, maxsize=None, items=(), _warn_depth=2):
        if maxsize is not None and maxsize <= 0:
            if maxsize == 0:
                import warnings
                warnings.warn(
                    'Queue(0) now equivalent to Queue(None); if you want a channel, use Channel',
                    DeprecationWarning,
                    stacklevel=_warn_depth)
            maxsize = None

        self._maxsize = maxsize if maxsize is not None else -1
        # Explicitly maintain order for getters and putters that block
        # so that callers can consistently rely on getting things out
        # in the apparent order they went in. This was once required by
        # imap_unordered. Previously these were set() objects, and the
        # items put in the set have default hash() and eq() methods;
        # under CPython, since new objects tend to have increasing
        # hash values, this tended to roughly maintain order anyway,
        # but that's not true under PyPy. An alternative to a deque
        # (to avoid the linear scan of remove()) might be an
        # OrderedDict, but it's 2.7 only; we don't expect to have so
        # many waiters that removing an arbitrary element is a
        # bottleneck, though.
        self.getters = collections.deque()
        self.putters = collections.deque()
        self.hub = get_hub()
        self._event_unlock = None
        self.queue = self._create_queue(items)

    @property
    def maxsize(self):
        return self._maxsize if self._maxsize > 0 else None

    @maxsize.setter
    def maxsize(self, nv):
        # QQQ make maxsize into a property with setter that schedules unlock if necessary
        if nv is None or nv <= 0:
            self._maxsize = -1
        else:
            self._maxsize = nv

    def copy(self):
        return type(self)(self.maxsize, self.queue)

    def _create_queue(self, items=()):
        return collections.deque(items)

    def _get(self):
        return self.queue.popleft()

    def _peek(self):
        return self.queue[0]

    def _put(self, item):
        self.queue.append(item)

    def __repr__(self):
        return '<%s at %s%s>' % (type(self).__name__, hex(id(self)), self._format())

    def __str__(self):
        return '<%s%s>' % (type(self).__name__, self._format())

    def _format(self):
        result = []
        if self.maxsize is not None:
            result.append('maxsize=%r' % (self.maxsize, ))
        if getattr(self, 'queue', None):
            result.append('queue=%r' % (self.queue, ))
        if self.getters:
            result.append('getters[%s]' % len(self.getters))
        if self.putters:
            result.append('putters[%s]' % len(self.putters))
        if result:
            return ' ' + ' '.join(result)
        return ''

    def qsize(self):
        """Return the size of the queue."""
        return len(self.queue)

    def __len__(self):
        """
        Return the size of the queue. This is the same as :meth:`qsize`.

        .. versionadded: 1.1b3

            Previously, getting len() of a queue would raise a TypeError.
        """

        return self.qsize()

    def __bool__(self):
        """
        A queue object is always True.

        .. versionadded: 1.1b3

           Now that queues support len(), they need to implement ``__bool__``
           to return True for backwards compatibility.
        """
        return True

    def __nonzero__(self):
        # Py2.
        # For Cython; __bool__ becomes a special method that we can't
        # get by name.
        return True

    def empty(self):
        """Return ``True`` if the queue is empty, ``False`` otherwise."""
        return not self.qsize()

    def full(self):
        """Return ``True`` if the queue is full, ``False`` otherwise.

        ``Queue(None)`` is never full.
        """
        return self._maxsize > 0 and self.qsize() >= self._maxsize

    def put(self, item, block=True, timeout=None):
        """Put an item into the queue.

        If optional arg *block* is true and *timeout* is ``None`` (the default),
        block if necessary until a free slot is available. If *timeout* is
        a positive number, it blocks at most *timeout* seconds and raises
        the :class:`Full` exception if no free slot was available within that time.
        Otherwise (*block* is false), put an item on the queue if a free slot
        is immediately available, else raise the :class:`Full` exception (*timeout*
        is ignored in that case).
        """
        if self._maxsize == -1 or self.qsize() < self._maxsize:
            # there's a free slot, put an item right away
            self._put(item)
            if self.getters:
                self._schedule_unlock()
        elif self.hub is getcurrent(): # pylint:disable=undefined-variable
            # We're in the mainloop, so we cannot wait; we can switch to other greenlets though.
            # Check if possible to get a free slot in the queue.
            while self.getters and self.qsize() and self.qsize() >= self._maxsize:
                getter = self.getters.popleft()
                getter.switch(getter)
            if self.qsize() < self._maxsize:
                self._put(item)
                return
            raise Full
        elif block:
            waiter = ItemWaiter(item, self)
            self.putters.append(waiter)
            timeout = Timeout._start_new_or_dummy(timeout, Full)
            try:
                if self.getters:
                    self._schedule_unlock()
                result = waiter.get()
                if result is not waiter:
                    raise InvalidSwitchError("Invalid switch into Queue.put: %r" % (result, ))
            finally:
                timeout.cancel()
                _safe_remove(self.putters, waiter)
        else:
            raise Full

    def put_nowait(self, item):
        """Put an item into the queue without blocking.

        Only enqueue the item if a free slot is immediately available.
        Otherwise raise the :class:`Full` exception.
        """
        self.put(item, False)


    def __get_or_peek(self, method, block, timeout):
        # Internal helper method. The `method` should be either
        # self._get when called from self.get() or self._peek when
        # called from self.peek(). Call this after the initial check
        # to see if there are items in the queue.

        if self.hub is getcurrent(): # pylint:disable=undefined-variable
            # special case to make get_nowait() or peek_nowait() runnable in the mainloop greenlet
            # there are no items in the queue; try to fix the situation by unlocking putters
            while self.putters:
                # Note: get() used popleft(), peek used pop(); popleft
                # is almost certainly correct.
                self.putters.popleft().put_and_switch()
                if self.qsize():
                    return method()
            raise Empty

        if not block:
            # We can't block, we're not the hub, and we have nothing
            # to return. No choice...
            raise Empty

        waiter = Waiter() # pylint:disable=undefined-variable
        timeout = Timeout._start_new_or_dummy(timeout, Empty)
        try:
            self.getters.append(waiter)
            if self.putters:
                self._schedule_unlock()
            result = waiter.get()
            if result is not waiter:
                raise InvalidSwitchError('Invalid switch into Queue.get: %r' % (result, ))
            return method()
        finally:
            timeout.cancel()
            _safe_remove(self.getters, waiter)

    def get(self, block=True, timeout=None):
        """Remove and return an item from the queue.

        If optional args *block* is true and *timeout* is ``None`` (the default),
        block if necessary until an item is available. If *timeout* is a positive number,
        it blocks at most *timeout* seconds and raises the :class:`Empty` exception
        if no item was available within that time. Otherwise (*block* is false), return
        an item if one is immediately available, else raise the :class:`Empty` exception
        (*timeout* is ignored in that case).
        """
        if self.qsize():
            if self.putters:
                self._schedule_unlock()
            return self._get()

        return self.__get_or_peek(self._get, block, timeout)

    def get_nowait(self):
        """Remove and return an item from the queue without blocking.

        Only get an item if one is immediately available. Otherwise
        raise the :class:`Empty` exception.
        """
        return self.get(False)

    def peek(self, block=True, timeout=None):
        """Return an item from the queue without removing it.

        If optional args *block* is true and *timeout* is ``None`` (the default),
        block if necessary until an item is available. If *timeout* is a positive number,
        it blocks at most *timeout* seconds and raises the :class:`Empty` exception
        if no item was available within that time. Otherwise (*block* is false), return
        an item if one is immediately available, else raise the :class:`Empty` exception
        (*timeout* is ignored in that case).
        """
        if self.qsize():
            # This doesn't schedule an unlock like get() does because we're not
            # actually making any space.
            return self._peek()

        return self.__get_or_peek(self._peek, block, timeout)

    def peek_nowait(self):
        """Return an item from the queue without blocking.

        Only return an item if one is immediately available. Otherwise
        raise the :class:`Empty` exception.
        """
        return self.peek(False)

    def _unlock(self):
        while True:
            repeat = False
            if self.putters and (self._maxsize == -1 or self.qsize() < self._maxsize):
                repeat = True
                putter = self.putters.popleft()
                try:
                    self._put(putter.item)
                except: # pylint:disable=bare-except
                    putter.throw(*sys.exc_info())
                else:
                    putter.switch(putter)
            if self.getters and self.qsize():
                repeat = True
                getter = self.getters.popleft()
                getter.switch(getter)
            if not repeat:
                self._event_unlock = None
                return

    def _schedule_unlock(self):
        if not self._event_unlock:
            self._event_unlock = self.hub.loop.run_callback(self._unlock)

    def __iter__(self):
        return self

    def __next__(self):
        result = self.get()
        if result is StopIteration:
            raise result
        return result

    next = __next__ # Py2
