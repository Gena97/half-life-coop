from collections import defaultdict
import re
import struct


PAIR = re.compile(r'"([^"\n]*)"\s*"([^"\n]*)"')


def vector(text, default=(0, 0, 0)):
    return [float(value) for value in text.split()] if text else list(default)


class Map:
    def __init__(self, name, path):
        self.name = name
        self.data = path.read_bytes()
        if len(self.data) < 124 or struct.unpack_from("<I", self.data)[0] != 30:
            raise ValueError(f"Unsupported BSP version: {name}")
        self.lumps = [struct.unpack_from("<II", self.data, 4 + index * 8) for index in range(15)]
        if any(offset + size > len(self.data) for offset, size in self.lumps):
            raise ValueError(f"Invalid BSP lump: {name}")
        offset, size = self.lumps[0]
        text = self.data[offset:offset + size].decode("latin1").rstrip("\0")
        self.entities = []
        self.targets = defaultdict(list)
        for index, record in enumerate(re.findall(r"\{[^}]*\}", text)):
            entity = dict(PAIR.findall(record))
            entity["index"] = index
            self.entities.append(entity)
            if entity.get("targetname"):
                self.targets[entity["targetname"]].append(entity)

    def model_bounds(self, entity):
        model = entity.get("model", "")
        if not model.startswith("*"):
            return None
        index = int(model[1:])
        offset, size = self.lumps[14]
        if index < 0 or index * 64 + 64 > size:
            raise ValueError(f"Invalid brush model: {self.name}/{model}")
        values = struct.unpack_from("<6f", self.data, offset + index * 64)
        origin = vector(entity.get("origin"))
        return [[values[axis] + origin[axis] for axis in range(3)],
                [values[axis + 3] + origin[axis] for axis in range(3)]]

    def describe(self, entity):
        result = dict(entity)
        bounds = self.model_bounds(entity)
        if bounds:
            result["bounds"] = bounds
        return result
