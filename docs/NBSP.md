# NBSP: newen's extended map format (version 1)

Half-Life's BSP30 with the small fields made 32-bit, so maps can have far more of everything and go past
±32768. The lumps and what they mean are BSP30's; only the field sizes change. newen loads both: BSP30 files are
widened as they load. All values little-endian, structs packed.

## Header

| Field | Type | |
|---|---|---|
| ident | 4 bytes | `NBSP` |
| version | int32 | 1 |
| lumps | 15 × { int32 offset, int32 length } | |

The lumps are BSP30's, in the same order: entities, planes, textures, vertexes, visibility, nodes, texinfo,
faces, lighting, clipnodes, leafs, marksurfaces, edges, surfedges, models. Lumps start on 4-byte boundaries.

Extra data (later features, e.g. a lightmap scale per face) goes in a **BSPX block**, the convention Quake
tools already use: 4-byte aligned right after the last lump, `"BSPX"`, int32 count, then count ×
{ char name[24], int32 offset, int32 length }. Readers skip names they don't know; newen ignores the block
for now.

## What changed from BSP30

| Lump | BSP30 | NBSP |
|---|---|---|
| nodes | int plane; short children[2]; short mins[3], maxs[3]; ushort firstface, numfaces | int plane; int children[2]; float mins[3], maxs[3]; int firstface, numfaces |
| faces | ushort plane; short side; int firstedge; short numedges; short texinfo; byte styles[4]; int lightofs | int plane; int side; int firstedge; int numedges; int texinfo; byte styles[4]; int lightofs |
| leafs | int contents, visofs; short mins[3], maxs[3]; ushort firstmarksurface, nummarksurfaces; byte ambient[4] | int contents, visofs; float mins[3], maxs[3]; int firstmarksurface, nummarksurfaces; byte ambient[4] |
| clipnodes | int plane; short children[2] | int plane; int children[2] |
| marksurfaces | ushort | int |
| edges | ushort v[2] | uint v[2] |

Unchanged: entities, planes, textures, vertexes (already float), visibility, texinfo, lighting (8-bit RGB, one
sample per 16 texture units), surfedges, models. Children keep BSP30's meaning: a node child >= 0 is a node,
negative is leaf `-1 - child`; a clipnode child >= 0 is a clipnode, negative is a contents value.

## Limits

The format has no size limit of its own. In practice float precision sets one: at ±131072 positions are exact
to about 1/64 unit, which the movement code still handles; further out needs origin rebasing (later).
Game DLLs can have their own limits: stock `hl.dll` deletes grenades, satchels, snarks and gibs outside ±4096.

The field layout is the same as Quake's BSP2, so tools that write BSP2 need only the header and Half-Life's
lumps (textures with palettes, RGB lighting).

## Making NBSP maps

- The compiler, `newen-compiler` (ericw-tools with NBSP added): `qbsp -nbsp map.map`, then `vis` and `light`
  as usual. `qbsp -hlbsp` writes BSP30 and switches to NBSP by itself when the map is too big for BSP30.
- `tools/bsp2nbsp.py in.bsp out.bsp [x y z]` converts a BSP30 map, optionally moving the whole map by an
  offset (for testing big coordinates).
