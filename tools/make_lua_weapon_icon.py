#!/usr/bin/env python3
"""Draws the weapon-menu icon newen gives a Lua weapon that ships none of its own.

Writes base/sprites/weapon_lua_generic.txt and one GoldSrc sprite per HUD size
(320, 640, 1280, 2560) into base/sprites/newen/. Each sprite holds the icon, the
icon as it looks when selected, and the small ammo icon, one under the other.

    python3 tools/make_lua_weapon_icon.py

No libraries needed. The drawing is newen's own: a plain pistol outline and "LUA".
"""
import os
import struct

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "base", "sprites")

# everything is drawn in the 640 HUD's units: the icon is 170x45, the ammo icon 24x24
ICON_W, ICON_H, AMMO = 170, 45, 24

# the pistol, as polygons
PISTOL = [
	[(22, 11), (86, 11), (86, 20), (22, 20)],                    # slide and barrel
	[(60, 20), (80, 20), (88, 39), (70, 39)],                    # grip
	[(46, 20), (60, 20), (60, 23), (49, 23), (49, 28), (62, 28), (63, 31), (46, 31)],   # trigger guard
	[(24, 8), (28, 8), (28, 11), (24, 11)],                      # front sight
	[(80, 8), (85, 8), (85, 11), (80, 11)],                      # rear sight
]

# "LUA" in blocks, each letter in a 14x22 box
def letters(x, y):
	l = [[(0, 0), (4, 0), (4, 18), (14, 18), (14, 22), (0, 22)]]
	u = [[(0, 0), (4, 0), (4, 18), (10, 18), (10, 0), (14, 0), (14, 22), (0, 22)]]
	a = [[(0, 22), (0, 0), (14, 0), (14, 22), (10, 22), (10, 14), (4, 14), (4, 22)],]
	hole = [(4, 4), (10, 4), (10, 10), (4, 10)]   # the A's counter, cut out below
	out, holes = [], []
	for i, polys in enumerate((l, u, a)):
		for p in polys:
			out.append([(px + x + i * 19, py + y) for px, py in p])
	holes.append([(px + x + 38, py + y) for px, py in hole])
	return out, holes

TEXT, TEXT_HOLES = letters(100, 12)

# the corner marks Half-Life's icons get when selected
def corners(w, h, n=7, t=1.5):
	out = []
	for cx, sx in ((1, 1), (w - 1, -1)):
		for cy, sy in ((1, 1), (h - 1, -1)):
			out.append(sorted_box(cx, cy, cx + sx * n, cy + sy * t))
			out.append(sorted_box(cx, cy, cx + sx * t, cy + sy * n))
	return out


def sorted_box(x1, y1, x2, y2):
	x1, x2 = min(x1, x2), max(x1, x2)
	y1, y2 = min(y1, y2), max(y1, y2)
	return [(x1, y1), (x2, y1), (x2, y2), (x1, y2)]


# a clip: the ammo icon
CLIP = [[(8, 2), (16, 2), (18, 22), (6, 22)]]
CLIP_HOLES = [[(10, 5), (14, 5), (14, 8), (10, 8)]]


def inside(poly, x, y):
	hit = False
	n = len(poly)
	for i in range(n):
		x1, y1 = poly[i]
		x2, y2 = poly[(i + 1) % n]
		if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
			hit = not hit
	return hit


def draw(w, h, scale, polys, holes, level, back=0):
	"""A w x h block of brightness values, 3x3 samples per pixel, on a plain background."""
	px = bytearray(w * h)
	for y in range(h):
		for x in range(w):
			got = 0
			for sy in range(3):
				for sx in range(3):
					ux = (x + (sx + 0.5) / 3) / scale
					uy = (y + (sy + 0.5) / 3) / scale
					if any(inside(p, ux, uy) for p in polys) and not any(inside(p, ux, uy) for p in holes):
						got += 1
			px[y * w + x] = back + (level - back) * got // 9
	return px


def pot(n):
	p = 1
	while p < n:
		p *= 2
	return p


def sprite(path, w, h, pixels):
	radius = (w * w + h * h) ** 0.5 / 2
	# IDSP v2, parallel to the view, additive, one frame, then a grey palette
	data = struct.pack("<4siiifiiifi", b"IDSP", 2, 2, 1, radius, w, h, 1, 0.0, 1)
	data += struct.pack("<h", 256) + bytes(c for i in range(256) for c in (i, i, i))
	data += struct.pack("<iiiii", 0, -(w // 2), h // 2, w, h) + bytes(pixels)
	with open(path, "wb") as f:
		f.write(data)


def main():
	os.makedirs(os.path.join(ROOT, "newen"), exist_ok=True)
	lines = []
	# HUD size -> pixels per 640-HUD unit. The 320 HUD's icons are 80x20 and 18x18.
	for res, scale in ((2560, 3.0), (1280, 2.0), (640, 1.0), (320, 80 / 170)):
		iw, ih = (80, 20) if res == 320 else (int(ICON_W * scale), int(ICON_H * scale))
		am = 18 if res == 320 else int(AMMO * scale)
		ascale = am / AMMO
		w, h = pot(iw), pot(ih * 2 + am)
		pixels = bytearray(w * h)

		def put(block, bw, bh, top):
			for y in range(bh):
				pixels[(top + y) * w:(top + y) * w + bw] = block[y * bw:(y + 1) * bw]

		# Half-Life's icons sit on a dim panel that is part of the picture
		put(draw(iw, ih, scale, PISTOL + TEXT, TEXT_HOLES, 170, 40), iw, ih, 0)
		put(draw(iw, ih, scale, PISTOL + TEXT + corners(ICON_W, ICON_H), TEXT_HOLES, 255, 40), iw, ih, ih)
		put(draw(am, am, ascale, CLIP, CLIP_HOLES, 255), am, am, ih * 2)
		name = "newen/lua_weapon_%d" % res
		sprite(os.path.join(ROOT, name + ".spr"), w, h, pixels)

		cross = "%dcrosshairs" % res if res > 640 else "crosshairs"
		c = {2560: 72, 1280: 48}.get(res, 24)
		lines += [
			"weapon\t\t%d %s\t0\t0\t%d\t%d" % (res, name, iw, ih),
			"weapon_s\t%d %s\t0\t%d\t%d\t%d" % (res, name, ih, iw, ih),
			"ammo\t\t%d %s\t0\t%d\t%d\t%d" % (res, name, ih * 2, am, am),
			# Half-Life's own pistol crosshair, by name
			"crosshair\t%d %s\t%d\t0\t%d\t%d" % (res, cross, c, c, c),
			"autoaim\t\t%d %s\t0\t%d\t%d\t%d" % (res, cross, c * 3, c, c),
		]
	with open(os.path.join(ROOT, "weapon_lua_generic.txt"), "w", newline="\n") as f:
		f.write("%d\n" % len(lines) + "\n".join(lines) + "\n")
	print("wrote weapon_lua_generic.txt and 4 sprites to", os.path.normpath(ROOT))


if __name__ == "__main__":
	main()
