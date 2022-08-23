# -*- coding: utf-8 -*-
from __future__ import print_function
from __future__ import unicode_literals
from __future__ import division
from __future__ import absolute_import

import logging
import uuid

from .deferred import Deferred


logger = logging.getLogger(__name__)


def create_uuid():
    return str(uuid.uuid4())


class BoundInstance:
    def __init__(self, attrname, rpc_peer):
        # should I make rpc_peer a weak_ref?
        self.attrname, self.rpc_peer = attrname, rpc_peer

    def __call__(self, *args, **kwargs):
        return self.rpc_peer.call(self.attrname, args, kwargs)

    def __getattr__(self, attrname):
        return BoundInstance(self.attrname + "." + attrname, self.rpc_peer)


class RpcProxy:
    def __init__(self, rpc_peer):
        self.rpc_peer = rpc_peer

    def __getattr__(self, attrname):
        return BoundInstance(attrname, self.rpc_peer)


def _defer_func(rpc_peer, attrname, args, kwargs, df):
    try:
        result = rpc_peer.call(attrname, args, kwargs)
    except Exception as e:
        df.errback(e)
    else:
        df.callback(result)


class DeferredBoundInstance:
    def __init__(self, attrname, rpc_peer):
        self.attrname = attrname
        self.rpc_peer = rpc_peer

    def __call__(self, *args, **kwargs):
        try:
            return self._make_call(*args, **kwargs)
        except Exception as e:
            logger.exception("DeferredBoundInstance.makeCall")
            raise

    def _make_call(self, *args, **kwargs):
        df = Deferred()
        task_id = self.rpc_peer.io_scheduler.spawn(_defer_func, self.rpc_peer,
                                                self.attrname, args, kwargs, df)
        df.add_callback(self.rpc_peer.scheduler.kill, task_id)
        return df


class DeferredRpcProxy:
    def __init__(self, rpc_peer):
        self.rpc_peer = rpc_peer

    def __getattr__(self, attrname):
        return DeferredBoundInstance(attrname, self.rpc_peer)


class RegisterServicesMixin:
    def __init__(self):
        self.services = {}

    def clear_services(self):
        self.services = {}

    def register_function(self, func, name = None):
        assert isinstance(name, str) or name is None
        if name is None:
            name = func.__name__
        self.services[name] = func

    def register_instance(self, inst, name):
        assert isinstance(name, str)
        self.services[name] = inst

    def unregister_instance(self, name):
        try:
            del self.services[name]
        except KeyError:
            pass

    unregister_function = unregister_instance

    def get_services(self):
        return self.services

    def set_services(self, services):
        self.services = services.copy()
