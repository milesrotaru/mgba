# Minimal Python PSF/GSF loader for analysis scripts (mirrors psf.c).
import os, struct, zlib

def _tags(data):
    tags = {}
    if not data.startswith(b'[TAG]'):
        return tags
    for line in data[5:].decode('utf-8', 'replace').split('\n'):
        if '=' in line:
            k, v = line.split('=', 1)
            k = k.strip().lower()
            tags[k] = tags[k] + '\n' + v.strip() if k in tags else v.strip()
    return tags

def _open(dir, name):
    p = os.path.join(dir, name)
    if os.path.exists(p):
        return p
    for f in os.listdir(dir):
        if f.lower() == name.lower():
            return os.path.join(dir, f)
    raise FileNotFoundError(p)

def load(path, image=None, top=True):
    if image is None:
        image = {'data': bytearray(), 'entry': None}
    d = open(path, 'rb').read()
    assert d[:4] == b'PSF\x22', path
    rsv, csz, crc = struct.unpack('<III', d[4:16])
    tags = _tags(d[16 + rsv + csz:])
    dir = os.path.dirname(path) or '.'
    if '_lib' in tags:
        load(_open(dir, tags['_lib']), image, False)
    if csz:
        prog = zlib.decompress(d[16 + rsv:16 + rsv + csz])
        entry, offset, size = struct.unpack('<III', prog[:12])
        if image['entry'] is None:
            image['entry'] = entry
        off = offset & 0x1FFFFFF
        if len(image['data']) < off + size:
            image['data'].extend(b'\0' * (off + size - len(image['data'])))
        image['data'][off:off + size] = prog[12:12 + size]
    n = 2
    while f'_lib{n}' in tags:
        load(_open(dir, tags[f'_lib{n}']), image, False)
        n += 1
    return (image, tags) if top else image
