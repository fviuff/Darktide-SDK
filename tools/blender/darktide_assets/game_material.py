"""Any of the game's materials as a game shader preset: its stream, the streams of the materials holding its
compiled shaders (its provider -> parent chain; the compiler ships copies of them), its texture slots and its
variables with the game's values. Read from the game files extract (the material resource names its stream) and the
game folder (bundle/data/..)."""
import os
import struct

from .particle_effect import _cooked_body, _name_table
from .reference_skeleton import _murmur64

_cache = {}


def material_hash(text):
    """a resource path (content/...) or #0123456789abcdef"""
    text = text.strip()
    if text.startswith("#"):
        return int(text[1:], 16)
    return _murmur64(text.lower())


def stream_path(extract, material):
    """the stream a material resource names, relative to the bundle folder (data/xx/<hash>)"""
    path = os.path.join(extract, "%016x.material" % material)
    if not os.path.isfile(path):
        raise ValueError("material %016x is not in the game files extract" % material)
    stub = _cooked_body(path)
    if not stub.startswith(b"data/"):
        raise ValueError("material %016x in the extract does not name a stream" % material)
    return stub.split(b"\0")[0].decode()


def layout(stream):
    """(provider, parent, [(texture slot id, texture)], [(variable id, floats, values)]) of a v61 material stream"""
    offset = struct.unpack_from("<I", stream, 4)[0]
    _, provider, parent = struct.unpack_from("<IQQ", stream, offset)
    offset += 20
    count = struct.unpack_from("<I", stream, offset)[0]
    offset += 4 + 4 * count
    count = struct.unpack_from("<I", stream, offset)[0]
    textures = [struct.unpack_from("<IQ", stream, offset + 4 + i * 12) for i in range(count)]
    offset += 4 + 12 * count
    count = struct.unpack_from("<I", stream, offset)[0]
    offset += 4 + 8 * count
    count = struct.unpack_from("<I", stream, offset)[0]
    records = [struct.unpack_from("<5I", stream, offset + 4 + i * 20) for i in range(count)]
    offset += 4 + 20 * count
    data = stream[offset + 4:offset + 4 + struct.unpack_from("<I", stream, offset)[0]]
    variables = [(name, klass + 1, list(struct.unpack_from("<%df" % (klass + 1), data, at)))
                 for klass, _, name, at, _ in records if klass <= 3]
    return provider, parent, textures, variables


def preset(text, extract, game_folder):
    """the game material as a preset dict like the entries of game_shaders.json"""
    key = (text.strip().lower(), os.path.abspath(extract), os.path.abspath(game_folder))
    if key in _cache:
        return _cache[key]
    bundle = os.path.join(game_folder, "bundle")
    read = lambda rel: open(os.path.join(bundle, *rel.split("/")), "rb").read()
    rel = stream_path(extract, material_hash(text))
    provider, parent, textures, variables = layout(read(rel))
    shader_streams, pending = {}, [provider, parent]
    while pending:
        material = pending.pop()
        if not material or "%016x" % material in shader_streams:
            continue
        shader_rel = stream_path(extract, material)
        shader_streams["%016x" % material] = shader_rel
        pending += layout(read(shader_rel))[:2]
    names = _name_table()
    label = lambda value: names.get(value, "#%08x" % value)
    result = {"id": "material:" + text.strip(), "label": text.strip(), "shader": text.strip(), "stream": rel,
              "shader_streams": shader_streams, "textures": [label(slot) for slot, _ in textures],
              "variables": [{"name": label(name), "floats": floats, "default": [round(v, 4) for v in values]}
                            for name, floats, values in variables]}
    _cache[key] = result
    return result
