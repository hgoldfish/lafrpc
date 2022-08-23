import json

from .base import BaseSerialization


def convert_bytes_to_base64(d):
    pass


def convert_base64_to_bytes(d):
    pass


class JsonSerialization(BaseSerialization):
    def pack(self, obj):
        bs = json.dumps(self.save_state(obj))
        return bs.encode("utf-8")

    def unpack(self, bs: bytes):
        d = json.loads(bs.decode("utf-8"))
        return self.restore_state(d)

