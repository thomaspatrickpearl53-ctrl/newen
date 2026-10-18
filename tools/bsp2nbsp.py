# Converts a Half-Life BSP30 map to newen's NBSP format (docs/NBSP.md), optionally moving the whole map.
#   python tools/bsp2nbsp.py in.bsp out.bsp [x y z]
# Moving keeps texture alignment (so lightmaps stay valid) and entity origins. Maps with origin brushes
# (rotating doors...) can't be moved, only converted.
import struct, sys, re

LUMPS = 15
ENTITIES, PLANES, TEXTURES, VERTEXES, VISIBILITY, NODES, TEXINFO, FACES, LIGHTING, CLIPNODES, LEAFS, \
    MARKSURFACES, EDGES, SURFEDGES, MODELS = range(LUMPS)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    off = tuple(float(v) for v in sys.argv[3:6]) if len(sys.argv) >= 6 else (0.0, 0.0, 0.0)
    moving = off != (0.0, 0.0, 0.0)
    d = open(src, 'rb').read()
    if struct.unpack_from('<i', d, 0)[0] != 30:
        sys.exit('%s is not a BSP30 map' % src)
    L = [struct.unpack_from('<ii', d, 4 + i * 8) for i in range(LUMPS)]
    raw = [d[o:o + n] for o, n in L]
    if not raw[ENTITIES].lstrip().startswith(b'{'):
        sys.exit('lumps in Blue Shift order are not supported')

    def dot(a, b):
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]

    def recs(lump, fmt):
        size = struct.calcsize(fmt)
        return [struct.unpack_from(fmt, raw[lump], i * size) for i in range(len(raw[lump]) // size)]

    out = [None] * LUMPS

    ents = raw[ENTITIES].rstrip(b'\0').decode('latin-1')
    if moving:
        for b in re.findall(r'\{([^}]*)\}', ents):
            kv = dict(re.findall(r'"([^"]*)"\s*"([^"]*)"', b))
            if kv.get('model', '').startswith('*') and 'origin' in kv:
                sys.exit('maps with origin brushes can only be converted, not moved')

        def shift(m):
            v = [float(x) for x in m.group(1).split()]
            return '"origin" "%g %g %g"' % (v[0] + off[0], v[1] + off[1], v[2] + off[2])
        ents = re.sub(r'"origin"\s*"([^"]*)"', shift, ents)
    out[ENTITIES] = ents.encode('latin-1') + b'\0'

    out[PLANES] = b''.join(struct.pack('<4fi', nx, ny, nz, dist + dot((nx, ny, nz), off), typ)
                           for nx, ny, nz, dist, typ in recs(PLANES, '<4fi'))
    out[VERTEXES] = b''.join(struct.pack('<3f', x + off[0], y + off[1], z + off[2])
                             for x, y, z in recs(VERTEXES, '<3f'))
    ti = []
    for v in recs(TEXINFO, '<8f2i'):
        v = list(v)
        for a in range(2):
            v[a * 4 + 3] -= dot(v[a * 4:a * 4 + 3], off)
        ti.append(struct.pack('<8f2i', *v))
    out[TEXINFO] = b''.join(ti)
    for lump in (TEXTURES, VISIBILITY, LIGHTING, SURFEDGES):
        out[lump] = raw[lump]

    # widened lumps
    out[NODES] = b''.join(
        struct.pack('<3i6f2i', plane, c0, c1, *[b[k] + off[k % 3] for k in range(6)], ff, nf)
        for plane, c0, c1, *b, ff, nf in [(r[0], r[1], r[2], *r[3:9], r[9], r[10]) for r in recs(NODES, '<i2h6h2H')])
    out[FACES] = b''.join(struct.pack('<5i4Bi', pl, side, fe, ne, tex, s0, s1, s2, s3, lo)
                          for pl, side, fe, ne, tex, s0, s1, s2, s3, lo in recs(FACES, '<Hhihh4Bi'))
    nclip = len(raw[CLIPNODES]) // 8
    clips = []
    for plane, c0, c1 in recs(CLIPNODES, '<i2h'):
        ch = []
        for c in (c0, c1):
            if c < -15 and nclip > 32767:   # 16-bit children past 32767 come out negative
                c &= 0xffff
            ch.append(c)
        clips.append(struct.pack('<3i', plane, *ch))
    out[CLIPNODES] = b''.join(clips)
    out[LEAFS] = b''.join(
        struct.pack('<2i6f2i4B', contents, visofs, *[b[k] + off[k % 3] for k in range(6)], fm, nm, a0, a1, a2, a3)
        for contents, visofs, *b, fm, nm, a0, a1, a2, a3
        in [(r[0], r[1], *r[2:8], r[8], r[9], *r[10:14]) for r in recs(LEAFS, '<2i6h2H4B')])
    out[MARKSURFACES] = b''.join(struct.pack('<i', m) for (m,) in recs(MARKSURFACES, '<H'))
    out[EDGES] = b''.join(struct.pack('<2I', a, b) for a, b in recs(EDGES, '<2H'))
    models = []
    for r in recs(MODELS, '<9f7i'):
        r = list(r)
        for k in range(6):
            r[k] += off[k % 3]
        models.append(struct.pack('<9f7i', *r))
    out[MODELS] = b''.join(models)

    # "NBSP", version 1, lump count, lumps; each lump 4-byte aligned
    header = 12 + LUMPS * 8
    body = b''
    table = []
    for data in out:
        table.append((header + len(body), len(data)))
        body += data + b'\0' * (-len(data) % 4)
    f = struct.pack('<4sii', b'NBSP', 1, LUMPS) + b''.join(struct.pack('<ii', o, n) for o, n in table) + body
    open(dst, 'wb').write(f)
    print('wrote %s (%d bytes)%s' % (dst, len(f), ' moved by %g %g %g' % off if moving else ''))


main()
