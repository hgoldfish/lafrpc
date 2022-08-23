import msgpack
import logging
from datetime import datetime
from msgpack.ext import Timestamp
from .base import BaseSerialization, SerializationException, DeserializationException


logger = logging.getLogger(__name__)


PrimitiveTypes = (int, float, bool, str, type(None), bytes)


def convert_datetime_to_msgpack_timestamp(d):
    if isinstance(d, PrimitiveTypes):
        return d
    elif isinstance(d, datetime):
        return Timestamp.from_datetime(d)
    elif isinstance(d, (list, tuple)):
        return [convert_datetime_to_msgpack_timestamp(e) for e in d]
    elif isinstance(d, dict):
        result = {}
        for key, value in d.items():
            result[key] = convert_datetime_to_msgpack_timestamp(value)
        return result
    else:
        logger.error("unknown type %s for msgpack serializer.", )
        raise SerializationException()


def convert_msgpack_timestamp_to_datetime(d):
    if isinstance(d, PrimitiveTypes):
        return d
    elif isinstance(d, Timestamp):
        return d.to_datetime()
    elif isinstance(d, (list, tuple)):
        return [convert_msgpack_timestamp_to_datetime(e) for e in d]
    elif isinstance(d, dict):
        result = {}
        for key, value in d.items():
            result[key] = convert_msgpack_timestamp_to_datetime(value)
        return result
    else:
        logger.error("unknown type %s for msgpack deserializer.", )
        raise DeserializationException()


class MsgpackSerialization(BaseSerialization):
    def pack(self, obj):
        d = self.save_state(obj)
        return msgpack.dumps(convert_datetime_to_msgpack_timestamp(d), use_bin_type=True)

    def unpack(self, bs):
        d = msgpack.loads(bs, raw=False)
        return self.restore_state(convert_msgpack_timestamp_to_datetime(d))
