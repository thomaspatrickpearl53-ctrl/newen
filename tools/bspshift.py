# Moves a whole BSP30 map by an offset (test tool): geometry, bounds, texture alignment, entities.
# Lightmaps stay valid because texture-space coordinates are kept the same.
import struct, sys, re

src, dst, ox, oy, oz = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5])
off = (ox, oy, oz)
d = bytearray(open(src, 'rb').read())
assert struct.unpack_from('<i', d, 0)[0] == 30
L = [struct.unpack_from('<ii', d, 4 + i * 8) for i in range(15)]

def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]

def short(v):
    v = int(round(v))
    assert -32768 <= v <= 32767, v
    return v

# entities: which brush models are stored relative to an origin (rotating ones) - those keep their geometry
ents = bytes(d[L[0][0]:L[0][0] + L[0][1]]).rstrip(b'\0').decode('latin-1')
relative = set()
for b in re.findall(r'\{([^}]*)\}', ents):
    kv = dict(re.findall(r'"([^"]*)"\s*"([^"]*)"', b))
    if kv.get('model', '').startswith('*') and 'origin' in kv:
        relative.add(int(kv['model'][1:]))
assert not relative, 'maps with origin brushes are not supported: %s' % sorted(relative)

def shift_origin(m):
    v = [float(x) for x in m.group(2).split()]
    return '"%s" "%g %g %g"' % (m.group(1), v[0] + ox, v[1] + oy, v[2] + oz)
ents = re.sub(r'"(origin)"\s*"([^"]*)"', shift_origin, ents)
newents = ents.encode('latin-1') + b'\0'

# planes
o, n = L[1]
for i in range(n // 20):
    p = o + i * 20
    nx, ny, nz, dist = struct.unpack_from('<4f', d, p)
    struct.pack_into('<f', d, p + 12, dist + dot((nx, ny, nz), off))
# vertices
o, n = L[3]
for i in range(n // 12):
    p = o + i * 12
    x, y, z = struct.unpack_from('<3f', d, p)
    struct.pack_into('<3f', d, p, x + ox, y + oy, z + oz)
# nodes: int plane, short children[2], short mins[3], maxs[3], ushort firstface, numfaces
o, n = L[5]
for i in range(n // 24):
    p = o + i * 24 + 8
    b = struct.unpack_from('<6h', d, p)
    struct.pack_into('<6h', d, p, *[short(b[k] + off[k % 3]) for k in range(6)])
# texinfo: keep s/t the same at the moved positions
o, n = L[6]
for i in range(n // 40):
    p = o + i * 40
    v = struct.unpack_from('<8f', d, p)
    for a in range(2):
        s = v[a * 4:a * 4 + 3]
        struct.pack_into('<f', d, p + a * 16 + 12, v[a * 4 + 3] - dot(s, off))
# leafs: int contents, visofs, short mins[3], maxs[3], ...
o, n = L[10]
for i in range(n // 28):
    p = o + i * 28 + 8
    b = struct.unpack_from('<6h', d, p)
    struct.pack_into('<6h', d, p, *[short(b[k] + off[k % 3]) for k in range(6)])
# models: float mins[3], maxs[3], origin[3]
o, n = L[14]
for i in range(n // 64):
    p = o + i * 64
    b = struct.unpack_from('<6f', d, p)
    struct.pack_into('<6f', d, p, *[b[k] + off[k % 3] for k in range(6)])

# put the new entity text at the end of the file
eo = len(d)
d += newents
while len(d) % 4:
    d += b'\0'
struct.pack_into('<ii', d, 4, eo, len(newents))
open(dst, 'wb').write(d)
print('wrote', dst)
