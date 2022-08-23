import pickle
from datetime import datetime
from ..base import RpcSerializationException


class SerializationException(RpcSerializationException):
    pass


class DeserializationException(RpcSerializationException):
    pass


PrimitiveTypes = (int, float, bool, str, type(None), bytes, datetime)


class BaseSerialization:
    special_sid_key = "__laf_sid__"
    classes = {}
    extensions = []

    @staticmethod
    def register_class(cls):
        if not hasattr(cls, "__lafrpc_id__"):
            raise SerializationException("registered class should have '__lafrpc_id__' property.")
        if not hasattr(cls, "__setstate__"):
            raise SerializationException("registered class should have '__setstate__' method.")
        if not hasattr(cls, "__getstate__"):
            raise SerializationException("registered class should have '__getstate__' method.")
        BaseSerialization.classes[cls.__lafrpc_id__] = cls

    @staticmethod
    def unregister_class(cls):
        del BaseSerialization.classes[cls.__lafrpc_id__]

    @staticmethod
    def register_extension(extension):
        if not hasattr(extension, "setstate"):
            raise SerializationException("registered extension should have 'setstate' function.")
        if not hasattr(extension, "getstate"):
            raise SerializationException("registered extension should have 'getstate' function.")
        BaseSerialization.extensions.append(extension)

    def get_special_key(self, obj):
        h = obj.__lafrpc_id__
        if h in self.classes:
            return h
        else:
            return ""

    def save_state(self, obj):
        if isinstance(obj, PrimitiveTypes):
            return obj
        elif isinstance(obj, (list, tuple)):
            return [self.save_state(e) for e in obj]
        elif isinstance(obj, dict):
            d = {}
            for k, v in obj.items():
                if not isinstance(k, str):
                    raise SerializationException("key of dict can only be str.")
                d[k] = self.save_state(v)
            return d
        else:
            if hasattr(obj, "__getstate__") and hasattr(obj, "__lafrpc_id__"):
                sid = self.get_special_key(obj)
                if not sid:
                    message = "{} has not a valid sid key.".format(type(obj))
                    raise SerializationException(message)
                try:
                    d = obj.__getstate__()
                except KeyError:
                    raise SerializationException("{0}.__getstate__() raise KeyError".format(sid))
                except Exception:
                    raise SerializationException("{0}.__getstate__() raise unknown Exception.".format(sid))
                d[BaseSerialization.special_sid_key] = sid
                for k, v in d.items():
                    if not isinstance(k, str):
                        raise SerializationException("key of dict can only be str.")
                    d[k] = self.save_state(v)
                return d
            else:
                for extension in BaseSerialization.extensions:
                    try:
                        d = extension.getstate(obj, self.special_sid_key)
                    except Exception as e:
                        raise SerializationException("extension.getstate() raise unknown Exception: {0}".format(type(e)))
                    if d is not None:
                        return d
                else:
                    raise SerializationException("can not get state of object({0}).".format(type(obj)))

    def restore_state(self, d):
        if isinstance(d, PrimitiveTypes):
            return d
        elif isinstance(d, list):
            return [self.restore_state(e) for e in d]
        elif isinstance(d, tuple):
            return [self.restore_state(e) for e in d]
        elif isinstance(d, dict):
            sid = d.pop(self.special_sid_key, "")
            d2 = {}
            for k, v in d.items():
                if not isinstance(k, str):
                    raise DeserializationException("key of dict can only be str.")
                d2[k] = self.restore_state(v)
            if sid and sid in self.classes:
                cls = self.classes[sid]
                if hasattr(cls, "__setstate__"):
                    o = cls()
                    if not o.__setstate__(d2):
                        raise DeserializationException("{}.__setstate__() returns false.".format(cls.__name__))
                    return o
                else:
                    raise DeserializationException("can not restore state for object({0}).".format(cls))
            elif sid:
                for extension in BaseSerialization.extensions:
                    try:
                        o = extension.setstate(d2, self.special_sid_key)
                    except Exception as e:
                        raise DeserializationException("extension.setstate() raise unknown Exception: {0}".format(type(e)))
                    if o is not None:
                        return o
                else:
                    raise DeserializationException("can not restore state for object({0}).".format(sid))
            else:
                return d2
        else:
            raise DeserializationException("can not deserialize {} object.".format(type(d)))

    def pack(self, obj):
        raise NotImplementedError()

    def unpack(self, bs):
        raise NotImplementedError()


def register_class(cls):
    BaseSerialization.register_class(cls)
    return cls


def unregister_class(cls):
    BaseSerialization.unregister_class(cls)


def register_extension(cls):
    BaseSerialization.register_extension(extension=cls())


class PickleSerialization(BaseSerialization):
    def pack(self, obj):
        return pickle.dumps(self.save_state(obj))

    def unpack(self, bs: bytes):
        d = pickle.loads(bs)
        return self.restore_state(d)


