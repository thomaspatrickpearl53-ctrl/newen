#!/usr/bin/env python3
"""Writes a tiny Half-Life map (BSP30): one closed, lit box room with a player start.

For testing the engine where no Half-Life data is around (a 64-bit or Android bring-up, CI): its one
texture is inside the map, so no WAD is needed. hl.so still asks for its own models and sounds, which
are simply missing.

    tools/make_test_room.py out.bsp [size] [height]     default 512 x 512, 256 high
"""
import math
import struct
import sys

HULLS = [None, (16, 16, 36), (32, 32, 32), (16, 16, 18)]   # half sizes of hulls 1-3 (stand, large, duck)
CONTENTS_EMPTY, CONTENTS_SOLID = -1, -2


def build(half, height):
    lo = (-half, -half, 0)
    hi = (half, half, height)
    # the six walls: (axis, value, inward normal sign)
    walls = [(0, lo[0], 1), (0, hi[0], -1), (1, lo[1], 1), (1, hi[1], -1), (2, lo[2], 1), (2, hi[2], -1)]

    planes = []   # (normal, dist, type), axial planes always have a positive normal
    for axis, value, _ in walls:
        n = [0.0, 0.0, 0.0]
        n[axis] = 1.0
        planes.append((n, float(value), axis))

    # render BSP: a chain of nodes, one per wall; the inside goes on, the outside is the solid leaf 0
    nodes = []
    for i, (axis, value, inward) in enumerate(walls):
        nxt = i + 1 if i + 1 < len(walls) else -2     # -2 = leaf 1, the room
        front, back = (nxt, -1) if inward > 0 else (-1, nxt)
        nodes.append((i, front, back))

    # collision hulls: the same walls moved in by the hull's size; clipnode children are contents
    clipnodes, headnodes = [], [0]
    for hull in HULLS[1:]:
        headnodes.append(len(clipnodes))
        for i, (axis, value, inward) in enumerate(walls):
            dist = value + inward * hull[axis]
            planes.append(([1.0 if a == axis else 0.0 for a in range(3)], float(dist), axis))
            pnum = len(planes) - 1
            last = i + 1 == len(walls)
            nxt = CONTENTS_EMPTY if last else len(clipnodes) + 1
            front, back = (nxt, CONTENTS_SOLID) if inward > 0 else (CONTENTS_SOLID, nxt)
            clipnodes.append((pnum, front, back))

    # faces: each wall's quad, wound clockwise as seen from inside the room
    verts, edges, surfedges, faces, lighting = [], [(0, 0)], [], [], bytearray()
    corners = [(x, y, z) for x in (lo[0], hi[0]) for y in (lo[1], hi[1]) for z in (lo[2], hi[2])]
    texinfos = []
    for i, (axis, value, inward) in enumerate(walls):
        quad = [c for c in corners if c[axis] == value]
        u, v = [a for a in range(3) if a != axis]
        cx = sum(c[u] for c in quad) / 4
        cy = sum(c[v] for c in quad) / 4
        quad.sort(key=lambda c: math.atan2(c[v] - cy, c[u] - cx))   # counter-clockwise in (u, v)
        normal = [0, 0, 0]
        normal[axis] = inward
        # (u, v, normal) handedness decides whether that order looks clockwise from the inside
        eu = [1 if a == u else 0 for a in range(3)]
        ev = [1 if a == v else 0 for a in range(3)]
        cross = [eu[1] * ev[2] - eu[2] * ev[1], eu[2] * ev[0] - eu[0] * ev[2], eu[0] * ev[1] - eu[1] * ev[0]]
        if sum(cross[a] * normal[a] for a in range(3)) > 0:
            quad.reverse()
        first = len(surfedges)
        base = len(verts)
        verts.extend(quad)
        for k in range(4):
            edges.append((base + k, base + (k + 1) % 4))
            surfedges.append(len(edges) - 1)
        # texture axes along the wall, one texel per unit
        s = [0.0] * 4
        t = [0.0] * 4
        s[u] = 1.0
        t[v] = -1.0 if v == 2 else 1.0
        texinfos.append((s, t))
        # lightmap: one sample per 16 units, as the engine works it out from the texture extents
        smin = min(c[u] for c in quad); smax = max(c[u] for c in quad)
        tvals = [c[v] * t[v] for c in quad]
        w = int(smax // 16 - smin // 16) + 1
        h = int(max(tvals) // 16 - min(tvals) // 16) + 1
        level = 200 if axis == 2 and inward > 0 else 160   # a slightly brighter floor
        lightofs = len(lighting)
        lighting += bytes([level, level, level - 20]) * (w * h)
        side = 0 if inward > 0 else 1
        faces.append((i, side, first, 4, i, lightofs))

    return planes, nodes, clipnodes, headnodes, verts, edges, surfedges, faces, texinfos, lighting, lo, hi


def miptex(name, size):
    """A two-tone checker with Half-Life's per-texture palette."""
    pixels = []
    for m in range(4):
        n = size >> m
        cell = max(1, (size // 4) >> m)
        pixels.append(bytes((((x // cell) + (y // cell)) & 1) for y in range(n) for x in range(n)))
    header = 16 + 8 + 16
    offs, pos = [], header
    for p in pixels:
        offs.append(pos)
        pos += len(p)
    palette = bytearray(768)
    palette[0:3] = bytes((150, 140, 120))
    palette[3:6] = bytes((90, 85, 75))
    data = struct.pack('<16sII4I', name.encode(), size, size, *offs) + b''.join(pixels)
    data += struct.pack('<H', 256) + bytes(palette) + b'\0\0'
    return data


def write(path, half, height):
    planes, nodes, clipnodes, headnodes, verts, edges, surfedges, faces, texinfos, lighting, lo, hi = build(half, height)
    ents = ('{\n"classname" "worldspawn"\n"message" "newen test room"\n"sounds" "1"\n"MaxRange" "4096"\n}\n'
            '{\n"classname" "info_player_start"\n"origin" "0 0 37"\n"angle" "90"\n}\n'
            '{\n"classname" "light"\n"origin" "0 0 %d"\n}\n' % (height - 32)).encode() + b'\0'

    tex = miptex('newen_check', 64)
    textures = struct.pack('<ii', 1, 8) + tex

    lumps = [b''] * 15
    lumps[0] = ents
    lumps[1] = b''.join(struct.pack('<4fi', *n, d, t) for n, d, t in planes)
    lumps[2] = textures
    lumps[3] = b''.join(struct.pack('<3f', *v) for v in verts)
    lumps[4] = b''
    # each node owns the wall on its own plane
    lumps[5] = b''.join(struct.pack('<ihh3h3hHH', p, f, b, *lo, *hi, p, 1) for p, f, b in nodes)
    lumps[6] = b''.join(struct.pack('<8fii', *s, *t, 0, 0) for s, t in texinfos)
    lumps[7] = b''.join(struct.pack('<hhihh4Bi', p, side, fe, ne, ti, 0, 255, 255, 255, lo_)
                        for p, side, fe, ne, ti, lo_ in faces)
    lumps[8] = bytes(lighting)
    lumps[9] = b''.join(struct.pack('<ihh', p, f, b) for p, f, b in clipnodes)
    lumps[10] = (struct.pack('<ii3h3hHH4B', CONTENTS_SOLID, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
                 + struct.pack('<ii3h3hHH4B', CONTENTS_EMPTY, -1, *lo, *hi, 0, len(faces), 0, 0, 0, 0))
    lumps[11] = b''.join(struct.pack('<H', i) for i in range(len(faces)))
    lumps[12] = b''.join(struct.pack('<HH', *e) for e in edges)
    lumps[13] = b''.join(struct.pack('<i', e) for e in surfedges)
    lumps[14] = struct.pack('<3f3f3f4iiii', *[float(x) for x in lo], *[float(x) for x in hi], 0.0, 0.0, 0.0,
                            *headnodes, 1, 0, len(faces))

    out = bytearray(struct.pack('<i', 30) + b'\0' * (15 * 8))
    for i, data in enumerate(lumps):
        while len(out) % 4:
            out.append(0)
        struct.pack_into('<ii', out, 4 + i * 8, len(out), len(data))
        out += data
    with open(path, 'wb') as f:
        f.write(out)


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    size = int(sys.argv[2]) if len(sys.argv) > 2 else 512
    height = int(sys.argv[3]) if len(sys.argv) > 3 else 256
    write(sys.argv[1], size // 2, height)
