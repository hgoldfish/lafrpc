import logging
import weakref

logger = logging.getLogger(__name__)


class WeakCallback:
    def __init__(self, callback):
        self.callback_self = weakref.ref(callback.__self__)
        self.callback_func = callback.__func__

    def is_ok(self):
        return self.callback_self() is not None

    def __call__(self, *args, **kwargs):
        if self.callback_self() is None:
            raise TypeError("Dead reference to callback.")
        return self.callback_func(self.callback_self(), *args, **kwargs)

    def __repr__(self):
        return "WeakCallback: {0!r} for {1!r}".format(self.callback_func, self.callback_self())


class NormalCallback:
    def __init__(self, callback):
        self.callback = callback

    @staticmethod
    def is_ok():
        return True

    def __call__(self, *args, **kwargs):
        return self.callback(*args, **kwargs)


# noinspection PyTypeChecker
def make_callback(callback) -> NormalCallback:
    if callback is None:
        return None
    if isinstance(callback, (WeakCallback, NormalCallback)):
        return callback
    if getattr(callback, "__self__", None) is not None:
        return WeakCallback(callback)
    else:
        return NormalCallback(callback)


class Deferred:
    """
    类似于twisted的Deferred。不过有以下不同：
    XXX 回调参数本身如果是types.MethodType，就变成弱引用。参数永远不会是弱引用，调用者需要自己保证不会引用循环引用
    原因与signal.connect()相同，避免循环引用。在实际使用中，应该尽量不要用types.FunctionType
    """

    def __init__(self, support_cancel=False):
        self.stack = []
        self.canceled = False
        if support_cancel:
            self.on_canceled = Deferred(support_cancel=False)

    # 几个用于添加回调函数的方法
    def add_callback(self, callback, *args, **kwargs):
        self.add_callbacks(callback, None, args, kwargs, [], {})

    def add_errback(self, errback, *args, **kwargs):
        self.add_callbacks(None, errback, [], {}, args, kwargs)

    def add_both(self, callback, *args, **kwargs):
        self.add_callbacks(callback, callback, args, kwargs, args, kwargs)

    def add_callbacks(self, callback, errback, callback_args: list = None, callback_kwargs: dict = None,
                      errback_args: list = None, errback_kwargs: dict = None):
        callback = make_callback(callback)
        errback = make_callback(errback)
        self.stack.append((callback, errback, callback_args or [], callback_kwargs or {},
                           errback_args or [], errback_kwargs or {}))

    def clear(self):
        self.stack = []

    def callback(self, result=None):
        self.run(result, True)

    def errback(self, error=None):
        self.run(error, False)

    def run(self, result, ok):
        for callback, errback, callback_args, callback_kwargs, errback_args, \
                errback_kwargs in self.stack:
            # FIXME I don't remember why these two lines is commented out.
            #            if self.canceled:
            #                return
            if ok and callback is not None and callback.is_ok():
                try:
                    result = callback(result, *callback_args, **callback_kwargs)
                except Exception as e:
                    logger.exception("Deferred发现错误：%s。", str(e))
                    ok = False
                    result = e
            elif not ok and errback is not None and errback.is_ok():
                try:
                    result = errback(result, *errback_args, **errback_kwargs)
                    ok = True
                except Exception as e:
                    logger.exception("Deferred发现错误：%s。", str(e))
                    result = e
        if not ok:
            if len(result.args) > 0:
                text = result.args[0]
            else:
                text = str(result)
            logger.exception("Deferred发现未处理的错误：%s，错误类型是%r。", text, type(result))
        self.clear()

    def cancel(self):
        if not self.canceled:
            self.canceled = True
            self.on_canceled.callback(None)


# FIXME 目前定义 Signal 的方式一般在 __init__() 方法之外，所有的对象共享一个 Signal，那样子肯定是不行的。
class Signal:
    def __init__(self, *argument_names):
        self.callbacks = []
        self.argument_names = argument_names

    def emit(self, *args, **kwargs):
        for callback, bound_args, bound_kwargs in self.callbacks:
            if not callback.is_ok():
                continue
            new_args = bound_args + args
            new_kwargs = dict(bound_kwargs)
            new_kwargs.update(kwargs)
            # noinspection PyBroadException
            try:
                callback(*new_args, **new_kwargs)
            except Exception:
                logger.exception("signal catch unhandled exception.")

    def add_callback(self, callback, *args, **kwargs):
        self.callbacks.append((make_callback(callback), args, kwargs))

    connect = add_callback
