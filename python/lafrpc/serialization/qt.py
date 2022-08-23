import qdatastream
from .base import BaseSerialization


class QDataStreamSerialization(BaseSerialization):
    def unpack(self, bs):
        s = qdatastream.Deserializer(bs)
        return self.restore_state(s.read_variant())

    def pack(self, obj):
        s = qdatastream.Serializer()
        s.write_variant(self.save_state(obj))
        return s.get_value()
