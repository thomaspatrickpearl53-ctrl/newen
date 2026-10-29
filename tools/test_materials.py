# Makes rough test materials for the modern renderer (r_renderer modern) from a map's or a model's own textures:
# a normal map guessed from each texture's brightness (<name>_n.png) and a specular map (<name>_s.png).
# They are only for trying the renderer out; real materials are drawn by hand or baked from real height data.
#   python tools/test_materials.py <halflife folder> <map name | models/x.mdl> <userdata game folder> [strength] [filter]
# Maps write <out>/textures/, models <out>/models/textures/<model>/ (where newen looks for them). e.g.
#   python tools/test_materials.py "C:/Program Files (x86)/Steam/steamapps/common/Half-Life" c1a0d bin/userdata/valve
#   python tools/test_materials.py "C:/Program Files (x86)/Steam/steamapps/common/Half-Life" models/scientist.mdl bin/userdata/valve
import fnmatch, os, struct, sys
import numpy as np
from PIL import Image

hl, source, out = sys.argv[1], sys.argv[2], sys.argv[3]
strength = float(sys.argv[4]) if len(sys.argv) > 4 else 3.0
pattern = sys.argv[5] if len(sys.argv) > 5 else '*'

def miptex_rgb(data, ofs):
    name = data[ofs:ofs + 16].split(b'\0')[0].decode('latin-1')
    w, h, o0, o1, o2, o3 = struct.unpack_from('<6I', data, ofs + 16)
    if not o0:
        return name, None
    pix = np.frombuffer(data, np.uint8, w * h, ofs + o0).reshape(h, w)
    pal_ofs = ofs + o3 + (w // 8) * (h // 8) + 2
    pal = np.frombuffer(data, np.uint8, 768, pal_ofs).reshape(256, 3)
    return name, pal[pix]

def map_textures(mapname):
    bsp = open(os.path.join(hl, 'valve', 'maps', mapname + '.bsp'), 'rb').read()
    assert struct.unpack_from('<i', bsp, 0)[0] == 30, 'BSP30 maps only'
    lumps = [struct.unpack_from('<ii', bsp, 4 + i * 8) for i in range(15)]
    ents = bsp[lumps[0][0]:lumps[0][0] + lumps[0][1]].decode('latin-1')
    tex_ofs = lumps[2][0]
    count = struct.unpack_from('<i', bsp, tex_ofs)[0]
    images = {}
    for i in range(count):
        o = struct.unpack_from('<i', bsp, tex_ofs + 4 + i * 4)[0]
        if o < 0:
            continue
        name, rgb = miptex_rgb(bsp, tex_ofs + o)
        images[name.lower()] = rgb
    # textures that aren't embedded come from the worldspawn's WADs
    wadkey = ents.split('"wad"', 1)[1].split('"')[1] if '"wad"' in ents else ''
    for w in wadkey.split(';'):
        base = os.path.basename(w.replace('\\', '/'))
        path = os.path.join(hl, 'valve', base)
        if not base or not os.path.exists(path):
            continue
        wad = open(path, 'rb').read()
        n, info = struct.unpack_from('<ii', wad, 4)
        for i in range(n):
            filepos, _, _, typ, comp = struct.unpack_from('<iiibb', wad, info + i * 32)
            name = wad[info + i * 32 + 16:info + i * 32 + 32].split(b'\0')[0].decode('latin-1').lower()
            if typ == 0x43 and not comp and name in images and images[name] is None:
                images[name] = miptex_rgb(wad, filepos)[1]
    skip = lambda n: n[0] in '!*+-{' or n.startswith(('sky', 'water', 'aaa', 'origin', 'clip'))
    return {n: v for n, v in images.items() if v is not None and not skip(n)}, os.path.join(out, 'textures')

def model_textures(path):
    full = os.path.join(hl, 'valve', path)
    data = open(full, 'rb').read()
    numtex, texofs = struct.unpack_from('<ii', data, 180)
    if numtex == 0:   # textures in the separate <name>T.mdl
        data = open(full[:-4] + 'T.mdl', 'rb').read()
        numtex, texofs = struct.unpack_from('<ii', data, 180)
    images = {}
    for i in range(numtex):
        o = texofs + i * 80
        name = data[o:o + 64].split(b'\0')[0].decode('latin-1')
        flags, w, h, index = struct.unpack_from('<4i', data, o + 64)
        pix = np.frombuffer(data, np.uint8, w * h, index).reshape(h, w)
        pal = np.frombuffer(data, np.uint8, 768, index + w * h).reshape(256, 3)
        images[os.path.splitext(name)[0]] = pal[pix]   # newen drops the extension, like the HD textures
    model = os.path.splitext(os.path.basename(path))[0]
    return images, os.path.join(out, 'models', 'textures', model)

images, folder = model_textures(source) if source.lower().endswith('.mdl') else map_textures(source)
os.makedirs(folder, exist_ok=True)
made = 0
for name, rgb in sorted(images.items()):
    if not fnmatch.fnmatch(name.lower(), pattern):
        continue
    height = rgb.astype(np.float32).mean(axis=2) / 255.0
    # wrapping differences, since world textures tile
    dx = (np.roll(height, -1, 1) - np.roll(height, 1, 1)) * 0.5 * strength
    dy = (np.roll(height, -1, 0) - np.roll(height, 1, 0)) * 0.5 * strength
    # +Y points up the image, so a height rising downwards (dy > 0) tilts the normal up
    nrm = np.stack([-dx, dy, np.ones_like(height)], axis=2)
    nrm /= np.linalg.norm(nrm, axis=2, keepdims=True)
    Image.fromarray(((nrm * 0.5 + 0.5) * 255).astype(np.uint8)).save(os.path.join(folder, name + '_n.png'))
    spec = (np.clip(height * 1.2 - 0.2, 0, 1) * 160).astype(np.uint8)
    Image.fromarray(spec).convert('RGB').save(os.path.join(folder, name + '_s.png'))
    made += 1
print(f'{made} test materials written to {folder}')
