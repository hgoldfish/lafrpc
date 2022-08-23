from .base import SerializationException, DeserializationException, register_class, unregister_class, \
        register_extension, BaseSerialization, PickleSerialization


__all__ = ["SerializationException", "DeserializationException", "register_class",
           "BaseSerialization", "available_serializations", "register_extension", "unregister_class"]


available_serializations = {
    "pickle": PickleSerialization
}

try:
    # noinspection PyUnresolvedReferences
    from .json import JsonSerialization
    available_serializations["json"] = JsonSerialization
except ImportError:
    pass


try:
    # noinspection PyUnresolvedReferences
    from .qt import QDataStreamSerialization
    available_serializations["qdatastream"] = QDataStreamSerialization
except ImportError:
    pass


try:
    # noinspection PyUnresolvedReferences
    from .msgpack import MsgpackSerialization
    available_serializations["msgpack"] = MsgpackSerialization
except ImportError:
    pass
