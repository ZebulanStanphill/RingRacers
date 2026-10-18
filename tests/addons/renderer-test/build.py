#!/usr/bin/env python3
"""Generate the renderer test map add-on (D_RenderTest_v1.pk3), or check the committed copy.

The UDMF map, its textures, flats and sprites, the SOC level header, BRIGHT and
SPRTINFO lumps, and the model files are generated deterministically here. The
map-local bot removal script is packaged from quiet.lua. Only the map's BSP
(the ZNODES lump) comes from an
external tool, ZDBSP, which is why --check compares that one lump structurally.
"""

import argparse
import io
import math
import re
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
PACKAGE = HERE / "D_RenderTest_v1.pk3"
MODELS = HERE / "models"

MAP_NAME = "RR_RENDERTEST"
MAP_WAD = f"maps/{MAP_NAME}.wad"
MARKER = "MAP01"  # the map's name comes from the file name; a WAD lump name holds 8 characters
EXTERNAL_TEXTURES = {"F_SKY1"}  # shipped with the game itself

# Editor numbers (MapThingNum) for the add-on's own scenery objects.
THING_TRANS = 3900
THING_GLOW = 3901

# Polyobject thing editor numbers: the engine's, and the ones ZDBSP knows.
PO_ANCHOR, PO_SPAWN = 760, 761
ZDBSP_PO_ANCHOR, ZDBSP_PO_SPAWN = 9300, 9301
PO_START_LINE = 20  # the engine's Polyobj_StartLine
ZDBSP_PO_START_LINE = 1  # ZDBSP's PO_LINE_START


# --------------------------------------------------------------------------
# PNG output. Deflate output differs between zlib builds, so the PNGs use
# stored blocks, which keeps --check byte-exact on every machine.
# --------------------------------------------------------------------------

def _png_chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def _zlib_stored(raw):
    out = bytearray(b"\x78\x01")
    pos = 0
    while True:
        block = raw[pos:pos + 65535]
        pos += len(block)
        final = pos >= len(raw)
        out.append(1 if final else 0)
        out += struct.pack("<HH", len(block), len(block) ^ 0xFFFF)
        out += block
        if final:
            break
    out += struct.pack(">I", zlib.adler32(bytes(raw)) & 0xFFFFFFFF)
    return bytes(out)


def png_indexed(width, height, palette, pixels, offsets=None):
    """An 8-bit palette PNG. palette: [(r, g, b, a)]. offsets: sprite (left, top) grAb offsets."""
    assert len(pixels) == width * height and max(pixels) < len(palette) <= 256
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw += pixels[y * width:(y + 1) * width]
    alphas = [c[3] for c in palette]
    while alphas and alphas[-1] == 255:
        alphas.pop()
    out = b"\x89PNG\r\n\x1a\n"
    out += _png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 3, 0, 0, 0))
    out += _png_chunk(b"PLTE", bytes(v for c in palette for v in c[:3]))
    if alphas:
        out += _png_chunk(b"tRNS", bytes(alphas))
    if offsets is not None:
        out += _png_chunk(b"grAb", struct.pack(">ii", *offsets))
    out += _png_chunk(b"IDAT", _zlib_stored(raw))
    out += _png_chunk(b"IEND", b"")
    return out


def render(width, height, palette, pixel, offsets=None):
    pixels = bytes(pixel(x, y) for y in range(height) for x in range(width))
    return png_indexed(width, height, palette, pixels, offsets)


def tri(n, period):
    """Integer triangle wave, 0..period/2; keeps the textures free of floating point."""
    return abs(n % period - period // 2)


# --------------------------------------------------------------------------
# Textures, flats and sprites
# --------------------------------------------------------------------------

def texture_wall():
    palette = [(84, 84, 92, 255), (176, 84, 64, 255), (156, 70, 52, 255),
               (194, 104, 78, 255), (222, 146, 112, 255), (112, 48, 38, 255)]

    def pixel(x, y):
        row, yy = y // 16, y % 16
        sx = x + (16 if row % 2 else 0)
        xx = sx % 32
        if yy < 2 or xx < 2:
            return 0
        if yy == 2 or xx == 2:
            return 4
        if yy == 15 or xx == 31:
            return 5
        return 1 + (sx // 32 + row * 3) % 3

    return render(128, 128, palette, pixel)


def flat_floor():
    palette = [(206, 184, 120, 255), (84, 112, 72, 255), (60, 60, 48, 255), (240, 224, 160, 255)]

    def pixel(x, y):
        if x % 16 == 0 or y % 16 == 0:
            return 2
        if x % 16 == 8 and y % 16 == 8:
            return 3
        return (x // 16 + y // 16) % 2

    return render(64, 64, palette, pixel)


def flat_ceiling():
    palette = [(70, 76, 110, 255), (96, 104, 150, 255), (40, 44, 70, 255)]

    def pixel(x, y):
        d = (x + y) % 16
        if d < 2:
            return 2
        return 0 if d < 9 else 1

    return render(64, 64, palette, pixel)


def texture_water():
    palette = [(24, 70, 160, 255), (36, 96, 190, 255), (52, 124, 214, 255), (88, 160, 232, 255), (150, 206, 250, 255)]
    return render(64, 64, palette, lambda x, y: min(4, tri(x * 3 + tri(y * 2, 16) * 2, 24) // 3))


def texture_fog():
    palette = [(120, 120, 128, 255), (140, 140, 148, 255), (100, 100, 108, 255)]
    return render(32, 32, palette, lambda x, y: (x * 7 + y * 13 + (x * y) % 5) % 3)


def texture_glass():
    palette = [(150, 225, 240, 255), (190, 240, 250, 255), (70, 150, 180, 255), (255, 255, 255, 255)]

    def pixel(x, y):
        if min(x, y, 63 - x, 63 - y) < 3:
            return 2
        if (x - y) % 32 < 3:
            return 3
        return 1 if ((x + y) // 16) % 2 else 0

    return render(64, 64, palette, pixel)


def texture_poly():
    palette = [(230, 140, 30, 255), (255, 220, 60, 255), (40, 30, 20, 255), (200, 90, 20, 255)]

    def pixel(x, y):
        d = abs(x % 32 - 16) + abs(y % 32 - 16)
        if d >= 24:
            return 2
        if d >= 20:
            return 3
        return 1 if d < 8 else 0

    return render(128, 128, palette, pixel)


GLYPHS = [
    ("..XXXX..", ".XX..XX.", "XX....XX", "XXXXXXXX", "XX....XX", "XX....XX", "XX....XX", "........"),  # A
    ("XXXXXX..", "XX...XX.", "XX...XX.", "XXXXXX..", "XX..XX..", "XX...XX.", "XX...XX.", "........"),  # R
    ("...XX...", "...XX...", "...XX...", "XXXXXXXX", "XXXXXXXX", "...XX...", "...XX...", "...XX..."),  # +
    ("XX......", "XXXX....", "XXXXXX..", "XXXXXXXX", "XXXXXX..", "XXXX....", "XX......", "........"),  # >
    (".XXXXXX.", "XX....XX", "XX....XX", "XX....XX", "XX....XX", "XX....XX", ".XXXXXX.", "........"),  # O
    ("XXXXXXXX", "XXXXXXXX", "XX....XX", "XX....XX", "XX....XX", "XX....XX", "XXXXXXXX", "XXXXXXXX"),  # box
]
SIGN_BAND = (48, 80)  # rows of the bright glyph band


def _sign_index(x, y):
    """Palette index of RTSIGN: 0 dark base, 1 panel seam, 2 band, 3 glyph, 4 band edge."""
    top, bottom = SIGN_BAND
    if top <= y < bottom:
        if y < top + 2 or y >= bottom - 2:
            return 4
        gy = y - (top + 8)
        if 0 <= gy < 16:
            glyph = GLYPHS[(x // 16 + y // 64) % len(GLYPHS)]
            if glyph[gy // 2][(x % 16) // 2] == "X":
                return 3
        return 2
    return 1 if x % 32 == 0 or y % 32 == 0 else 0


def texture_sign():
    palette = [(20, 24, 48, 255), (34, 40, 78, 255), (16, 70, 80, 255), (255, 230, 60, 255), (60, 230, 255, 255)]
    return render(128, 128, palette, _sign_index)


def texture_sign_brightmap():
    palette = [(0, 0, 0, 255), (255, 255, 255, 255)]
    return render(128, 128, palette, lambda x, y: 1 if _sign_index(x, y) >= 3 else 0)


def sprite_crystal():
    """RTTRA0: a faceted crystal, 32x48, with transparent surroundings."""
    palette = [(0, 0, 0, 0), (20, 30, 60, 255), (140, 220, 255, 255), (80, 170, 230, 255),
               (40, 100, 180, 255), (255, 255, 255, 255)]

    def inside(x, y):
        if y < 2 or y > 45:
            return False
        halfwidth = (y - 2) * 14 // 18 if y <= 20 else 14 - (y - 20) * 14 // 25
        return abs(x - 16) <= halfwidth

    def pixel(x, y):
        if not inside(x, y):
            return 0
        if not (inside(x - 1, y) and inside(x + 1, y) and inside(x, y - 1) and inside(x, y + 1)):
            return 1
        if x < 14:
            return 2
        if x < 18:
            return 5 if y < 26 else 3
        return 4 if y >= 24 else 3

    return render(32, 48, palette, pixel, offsets=(16, 48))


def _orb_distance(x, y):
    return (x - 24) ** 2 + (y - 24) ** 2


def sprite_glow():
    """RTGLA0: a dark orb with a glowing ring and core, 48x48."""
    palette = [(0, 0, 0, 0), (16, 20, 40, 255), (48, 60, 110, 255), (80, 96, 150, 255),
               (255, 150, 30, 255), (255, 250, 190, 255)]

    def pixel(x, y):
        d = _orb_distance(x, y)
        if d > 22 * 22:
            return 0
        if d > 20 * 20:
            return 1
        if d <= 8 * 8:
            return 5
        if 13 * 13 <= d <= 15 * 15:
            return 4
        return 3 if x + y < 40 else 2

    return render(48, 48, palette, pixel, offsets=(24, 48))


def sprite_glow_brightmap():
    """RTGLBRA0: marks the ring and core of RTGLA0; it must be the same size."""
    palette = [(0, 0, 0, 255), (255, 255, 255, 255)]

    def pixel(x, y):
        d = _orb_distance(x, y)
        return 1 if d <= 8 * 8 or 13 * 13 <= d <= 15 * 15 else 0

    return render(48, 48, palette, pixel, offsets=(24, 48))


# Ordered so that every folder's entries are contiguous: the engine finds a
# PK3 folder as a run of consecutive entries.
IMAGES = [
    ("Textures/RTWALL.png", texture_wall),
    ("Textures/RTPOLY.png", texture_poly),
    ("Textures/RTSIGN.png", texture_sign),
    ("Textures/RTSIGNBM.png", texture_sign_brightmap),
    ("Textures/RTWATER.png", texture_water),
    ("Textures/RTFOG.png", texture_fog),
    ("Textures/RTGLASS.png", texture_glass),
    ("Flats/RTFLOOR.png", flat_floor),
    ("Flats/RTCEIL.png", flat_ceiling),
    ("Sprites/RTTRA0.png", sprite_crystal),
    ("Sprites/RTGLA0.png", sprite_glow),
    # Not in Sprites/: that folder's lumps are all read as frames of whichever
    # sprite their first four letters name, and this one starts with RTGL.
    ("Graphics/RTGLBRA0.png", sprite_glow_brightmap),
]


# --------------------------------------------------------------------------
# Text lumps
# --------------------------------------------------------------------------

SOC = f"""Freeslot
MT_RTTRANS
MT_RTGLOW
S_RTTRANS
S_RTGLOW
SPR_RTTR
SPR_RTGL

Object MT_RTTRANS
MapThingNum = {THING_TRANS}
SpawnState = S_RTTRANS
SpawnHealth = 1000
Radius = 16*FRACUNIT
Height = 48*FRACUNIT
Flags = MF_SCENERY|MF_NOBLOCKMAP|MF_NOGRAVITY

Object MT_RTGLOW
MapThingNum = {THING_GLOW}
SpawnState = S_RTGLOW
SpawnHealth = 1000
Radius = 24*FRACUNIT
Height = 48*FRACUNIT
Flags = MF_SCENERY|MF_NOBLOCKMAP|MF_NOGRAVITY

State S_RTTRANS
SpriteName = RTTR
SpriteFrame = FF_TRANS50|A
Duration = -1
Next = S_NULL

State S_RTGLOW
SpriteName = RTGL
SpriteFrame = A
Duration = -1
Next = S_NULL

Level {MAP_NAME}
LevelName = Render Test
TypeOfLevel = RACE
NumLaps = 1
Hidden = True
Music = NONE
Weather = RAIN

"""

BRIGHT = "texture RTSIGN RTSIGNBM\n"

SPRTINFO = """SPRITE RTGL
{
	FRAME A
	{
		BRIGHTMAP RTGLBRA0
	}
}
"""

# models.dat sits in the game's home folder, next to models/. The file name is
# relative to models/. The item box's sprite is RBOX until it turns into RNDM.
MODELS_DAT = "RNDM rendertest.md3 2.0 0.0\nRBOX rendertest.md3 2.0 0.0\n"


# --------------------------------------------------------------------------
# MD3 model: a box about as big as an item box, textured by the sprite
# --------------------------------------------------------------------------

def build_md3():
    half, height = 1024, 2048  # raw units; the engine scales them by 1/64 and models.dat's scale
    # name, normal (lat, lng), the four corners in counter-clockwise order seen from outside
    faces = [
        ("+x", (0, 64), [(half, -half, 0), (half, half, 0), (half, half, height), (half, -half, height)]),
        ("-x", (128, 64), [(-half, half, 0), (-half, -half, 0), (-half, -half, height), (-half, half, height)]),
        ("+y", (64, 64), [(half, half, 0), (-half, half, 0), (-half, half, height), (half, half, height)]),
        ("-y", (192, 64), [(-half, -half, 0), (half, -half, 0), (half, -half, height), (-half, -half, height)]),
        ("+z", (0, 0), [(-half, -half, height), (half, -half, height), (half, half, height), (-half, half, height)]),
        ("-z", (0, 128), [(-half, half, 0), (half, half, 0), (half, -half, 0), (-half, -half, 0)]),
    ]
    uvs = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]
    verts, st, tris = [], [], []
    for _, (lat, lng), corners in faces:
        base = len(verts)
        for corner, uv in zip(corners, uvs):
            verts.append(struct.pack("<hhhH", *corner, (lat << 8) | lng))
            st.append(struct.pack("<ff", *uv))
        # Both windings, so the box is solid whichever way the renderer culls.
        tris += [(base, base + 1, base + 2), (base, base + 2, base + 3),
                 (base, base + 2, base + 1), (base, base + 3, base + 2)]
    tri_bytes = b"".join(struct.pack("<iii", *t) for t in tris)
    ofs_tris = 108
    ofs_shaders = ofs_tris + len(tri_bytes)
    ofs_st = ofs_shaders
    ofs_xyz = ofs_st + 8 * len(verts)
    surface_end = ofs_xyz + 8 * len(verts)
    surface = struct.pack("<i64siiiiiiiiii", 0x33504449, b"box", 0, 1, 0, len(verts), len(tris),
                          ofs_tris, ofs_shaders, ofs_st, ofs_xyz, surface_end)
    surface += tri_bytes + b"".join(st) + b"".join(verts)
    frame = struct.pack("<3f3f3ff16s", -16.0, -16.0, 0.0, 16.0, 16.0, 32.0, 0.0, 0.0, 0.0, 28.0, b"frame")
    ofs_frames = 108
    ofs_tags = ofs_frames + len(frame)
    ofs_surfaces = ofs_tags
    header = struct.pack("<ii64siiiiiiiii", 0x33504449, 15, b"rendertest", 0, 1, 0, 1, 0,
                         ofs_frames, ofs_tags, ofs_surfaces, ofs_surfaces + len(surface))
    return header + frame + surface


# --------------------------------------------------------------------------
# Map geometry
# --------------------------------------------------------------------------

def _area(loop):
    return sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(loop, loop[1:] + loop[:1])) / 2


def _on_open_segment(p, a, b):
    cross = (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])
    if cross:
        return False
    dot = (p[0] - a[0]) * (b[0] - a[0]) + (p[1] - a[1]) * (b[1] - a[1])
    return 0 < dot < (b[0] - a[0]) ** 2 + (b[1] - a[1]) ** 2


def rect(x0, y0, x1, y1):
    return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]


class Line:
    def __init__(self, v1, v2, front, back, props):
        self.v1, self.v2, self.front, self.back, self.props = v1, v2, front, back, props


class Geometry:
    """Sectors as polygons; shared edges become two-sided lines, the rest one-sided walls."""

    def __init__(self):
        self.sectors = []  # dicts: name, outer, holes, wall, props
        self.edge_props = {}  # frozenset({a, b}) -> line props
        self.free_lines = []  # (a, b, sector name, props): both sides in one sector
        self.things = []

    def sector(self, name, outer, holes=(), wall="RTWALL", **props):
        defaults = {"heightfloor": 0, "heightceiling": 256, "texturefloor": "RTFLOOR",
                    "textureceiling": "RTCEIL", "lightlevel": 160}
        defaults.update(props)
        self.sectors.append({"name": name, "outer": list(outer), "holes": [list(h) for h in holes],
                             "wall": wall, "props": defaults})

    def edge(self, a, b, **props):
        self.edge_props[frozenset((a, b))] = props

    def free_line(self, a, b, sector, **props):
        self.free_lines.append((a, b, sector, props))

    def thing(self, type, x, y, height=0, angle=0, **extra):
        self.things.append({"type": type, "x": x, "y": y, "height": height, "angle": angle, **extra})

    def index(self, name):
        return [s["name"] for s in self.sectors].index(name)

    def build(self):
        loops = []  # (sector index, points) oriented with the sector on the right of each edge
        for i, s in enumerate(self.sectors):
            outer = s["outer"] if _area(s["outer"]) < 0 else s["outer"][::-1]
            loops.append((i, outer))
            for hole in s["holes"]:
                loops.append((i, hole if _area(hole) > 0 else hole[::-1]))
        points = {p for _, loop in loops for p in loop}
        for a, b, _, _ in self.free_lines:
            points |= {a, b}
        split = []
        for i, loop in loops:
            out = []
            for a, b in zip(loop, loop[1:] + loop[:1]):
                inner = sorted((p for p in points if _on_open_segment(p, a, b)),
                               key=lambda p: (p[0] - a[0]) ** 2 + (p[1] - a[1]) ** 2)
                out += [a] + inner
            split.append((i, out))
        directed = {}
        for i, loop in split:
            for a, b in zip(loop, loop[1:] + loop[:1]):
                assert (a, b) not in directed, f"overlapping edge {a}->{b}"
                directed[(a, b)] = i
        lines, done = [], set()
        for i, loop in split:
            for a, b in zip(loop, loop[1:] + loop[:1]):
                if (a, b) in done:
                    continue
                back = directed.get((b, a))
                done |= {(a, b), (b, a)}
                lines.append(Line(a, b, i, back, self.edge_props.get(frozenset((a, b)), {})))
        used = {frozenset(k) for k in done}
        for key in self.edge_props:
            assert key in used, f"edge properties for a line that does not exist: {sorted(key)}"
        for a, b, name, props in self.free_lines:
            i = self.index(name)
            lines.append(Line(a, b, i, i, props))
        return lines


def textmap_value(value):
    if value is True:
        return "true"
    if isinstance(value, str):
        return f'"{value}"'
    return str(value)


def emit_textmap(geometry, lines, zdbsp=False):
    """The TEXTMAP text. With zdbsp=True, the variant given to ZDBSP: it sees the polyobjects as it expects them."""
    vertices = []
    for line in lines:
        for p in (line.v1, line.v2):
            if p not in vertices:
                vertices.append(p)
    index = {p: i for i, p in enumerate(vertices)}

    sidedefs, linedefs = [], []
    for line in lines:
        props = dict(line.props)
        override = props.pop("sides", None)
        midtexture = props.pop("midtexture", None)
        polyobject = props.pop("polyobject", None)
        args = props.pop("args", [])
        two_sided = line.back is not None
        entry = {"v1": index[line.v1], "v2": index[line.v2]}
        sides = []
        for which, sector in enumerate((line.front, line.back)):
            if sector is None:
                continue
            wall = geometry.sectors[sector]["wall"]
            if override is not None:
                textures = dict(override[which])
            elif two_sided:
                textures = {"texturetop": wall, "texturebottom": wall}
            else:
                textures = {"texturemiddle": wall}
            if midtexture is not None:
                textures["texturemiddle"] = midtexture
            sidedefs.append({"sector": sector, **textures})
            sides.append(len(sidedefs) - 1)
        entry["sidefront"] = sides[0]
        if zdbsp and polyobject is not None:
            # ZDBSP knows Hexen's one-sided polyobjects and its own start line special; it is the
            # same map otherwise (the back sidedef stays, unused, so that the numbering matches).
            two_sided = False
            if props.get("special") == PO_START_LINE:
                props["special"] = ZDBSP_PO_START_LINE
        if two_sided:
            entry["sideback"] = sides[1]
            entry["twosided"] = True
        if "special" in props:
            entry["special"] = props.pop("special")
        for n, value in enumerate(args):
            entry[f"arg{n}"] = value
        entry.update(props)
        linedefs.append(entry)

    out = ['namespace = "zdoom";' if zdbsp else 'namespace = "ringracers";']
    if not zdbsp:
        out.append("version = 2;")

    def block(name, fields):
        out.append(name)
        out.append("{")
        for key, value in fields.items():
            out.append(f"{key} = {textmap_value(value)};")
        out.append("}")

    for p in vertices:
        block("vertex", {"x": p[0], "y": p[1]})
    for entry in linedefs:
        block("linedef", entry)
    for side in sidedefs:
        block("sidedef", side)
    for s in geometry.sectors:
        block("sector", s["props"])
    for t in geometry.things:
        fields = dict(t)
        if zdbsp and fields["type"] in (PO_ANCHOR, PO_SPAWN):
            # ZDBSP numbers polyobjects by the thing's angle, and knows other thing numbers.
            fields["type"] = ZDBSP_PO_ANCHOR if fields["type"] == PO_ANCHOR else ZDBSP_PO_SPAWN
            fields["angle"] = fields["id"]
        if not fields["angle"]:
            del fields["angle"]
        ordered = {k: fields.pop(k) for k in ("x", "y", "height", "angle", "type") if k in fields}
        block("thing", {**ordered, **fields})
    return "\n".join(out) + "\n"


# --------------------------------------------------------------------------
# The map itself
# --------------------------------------------------------------------------

HALL = (-1280, -640, 1280, 640)
ROOM_X = (-832, 0, 832)  # room centres; each is 768 wide and 768 deep, behind a 512-wide doorway
NORTH, SOUTH = (704, 1472), (-1472, -704)


def room_x(cx):
    return cx - 384, cx + 384


def color(r, g, b):
    return (r << 16) | (g << 8) | b


def build_geometry():
    g = Geometry()
    floor, ceil = "RTFLOOR", "RTCEIL"

    # The hall, with a sky ceiling for the rain, and a doorway to each room.
    g.sector("hall", rect(*HALL), heightceiling=512, textureceiling="F_SKY1", lightlevel=200)
    for n, cx in enumerate(ROOM_X):
        g.sector(f"door_n{n}", rect(cx - 256, 640, cx + 256, 704), lightlevel=180)
        g.sector(f"door_s{n}", rect(cx - 256, -704, cx + 256, -640), lightlevel=180)

    # Control sectors for the 3D floors live in a row east of everything else.
    controls = []

    def control(name, bottom, top, special, args, wall="RTGLASS", **props):
        controls.append((name, bottom, top, special, args, wall, props))

    # 2. Fog: two overlapping fog blocks, seen from outside through the doorway and from inside.
    x0, x1 = room_x(ROOM_X[0])
    strips = [(x0, x0 + 256, 21, None), (x0 + 256, x0 + 512, 21, "22"), (x0 + 512, x1, 22, None)]
    for n, (a, b, tag, more) in enumerate(strips):
        extra = {"moreids": more} if more else {}
        pillar = rect((a + b) // 2 - 48, 1052, (a + b) // 2 + 48, 1148)
        g.sector(f"fog_{n}", rect(a, NORTH[0], b, NORTH[1]), holes=[pillar], id=tag, lightlevel=180, **extra)
    control("fog_red", 0, 128, 202, [21], wall="RTFOG", lightlevel=200, lightcolor=color(255, 0, 0),
            fadecolor=color(64, 0, 0), fadeend=24, colormapfog=True)
    control("fog_blue", 0, 128, 202, [22], wall="RTFOG", lightlevel=200, lightcolor=color(0, 0, 255),
            fadecolor=color(0, 0, 64), fadeend=24, colormapfog=True)

    # 3. Water: overlapping water volumes over a sloped floor, pillars, a heat-haze ledge, a translucent wall.
    x0, x1 = room_x(ROOM_X[1])
    slope = {"floorplane_a": 0, "floorplane_b": -0.2, "floorplane_c": 1, "floorplane_d": 140.8}
    pillars = [rect(-288, 772, -192, 868), rect(-48, 832, 48, 928), rect(192, 1092, 288, 1188)]
    corner = rect(128, 704, 384, 960)
    g.sector("water_main", [(x0, 704), (128, 704), (128, 960), (x1, 960), (x1, 1344), (x0, 1344)], holes=pillars,
             id=30, lightlevel=200, **slope)
    g.sector("water_corner", corner, id=30, moreids="31", lightlevel=200, **slope)
    for n, p in enumerate(pillars):
        g.sector(f"water_pillar{n}", p, heightfloor=192, lightlevel=200)
    g.sector("water_ledge", rect(x0, 1344, x1, 1472), heightfloor=128, lightlevel=200, heatwave=True)
    g.free_line((x0, 1016), (x1, 1016), "water_main", midtexture="RTGLASS", alpha=0.5,
                wrapmidtex=True, midpeg=True, sides=({}, {}))
    control("water_big", 0, 96, 120, [30, 128, 0, 0, 0], wall="RTWATER", texturefloor="RTWATER",
            textureceiling="RTWATER", lightlevel=200, lightcolor=color(32, 96, 255), lightalpha=24)
    control("water_small", 0, 48, 120, [31, 96, 0, 0, 0], wall="RTWATER", texturefloor="RTWATER",
            textureceiling="RTWATER", lightlevel=200, lightcolor=color(32, 255, 96), lightalpha=24)

    # 4. Polyobjects: the room is one convex sector, and the objects wait in a garage far to the east.
    x0, x1 = room_x(ROOM_X[2])
    g.sector("poly_room", rect(x0, NORTH[0], x1, NORTH[1]), lightlevel=200)
    po1, po2 = rect(2436, 936, 2564, 1064), rect(2736, 936, 2864, 1064)
    g.sector("garage", rect(2304, 704, 2944, 1344), holes=[po1, po2], wall="RTWALL", lightlevel=160)
    po_wall = ({"texturemiddle": "RTPOLY"}, {"texturemiddle": "RTPOLY"})
    # The sector behind a polyobject's first line is what the engine draws its planes from, and where the
    # ripple flags are read.
    g.sector("po1", po1, heightceiling=48, texturefloor="RTPOLY", textureceiling="RTPOLY", lightlevel=200,
             ripple_floor=True, ripple_ceiling=True)
    g.sector("po2", po2, heightceiling=96, texturefloor="RTPOLY", textureceiling="RTPOLY", lightlevel=200)
    for n, (a, b, c, d) in enumerate([(2436, 936, 2564, 1064), (2736, 936, 2864, 1064)], 1):
        for p, q in [((c, b), (c, d)), ((c, d), (a, d)), ((a, d), (a, b))]:
            g.edge(p, q, polyobject=n, sides=po_wall)
        g.edge((a, b), (c, b), polyobject=n, sides=po_wall, special=PO_START_LINE,
               args=[n, -1, 3 if n == 1 else 0, 0, 0])
    # Crossing the doorway line sets the translucent polyobject spinning.
    g.edge((x0 + 128, 704), (x1 - 128, 704), special=484, args=[1, 8, 1, 4, 0], playercross=True)
    g.thing(PO_ANCHOR, 2500, 1000, id=1)
    g.thing(PO_SPAWN, 704, 1088, id=1)
    g.thing(PO_ANCHOR, 2800, 1000, id=2)
    g.thing(PO_SPAWN, 964, 1088, id=2)

    # 5. Light slices: a tall room with three light blocks and a lit translucent platform.
    x0, x1 = room_x(ROOM_X[0])
    plat = rect(-1000, -1400, -664, -1200)
    g.sector("light_main", rect(x0, SOUTH[0], x1, SOUTH[1]), holes=[plat], heightceiling=512, id=50, lightlevel=160)
    g.sector("light_plat", plat, heightceiling=512, id=50, moreids="51", lightlevel=160)
    control("light_white", 64, 96, 200, [50, 0], wall="RTWALL", lightlevel=255,
            lightcolor=color(255, 255, 255), lightalpha=25)
    control("light_orange", 192, 224, 200, [50, 0], wall="RTWALL", lightlevel=128,
            lightcolor=color(255, 128, 0), lightalpha=40)
    control("light_blue", 320, 352, 200, [50, 0], wall="RTWALL", lightlevel=48,
            lightcolor=color(32, 64, 255), lightalpha=40)
    control("light_glass", 128, 160, 100, [51, 160, 0, 0, 0], lightlevel=100)

    # 6. Transparency ordering: stacked translucent solids and translucent walls.
    x0, x1 = room_x(ROOM_X[1])
    stack = rect(-192, -1216, 192, -960)
    g.sector("trans_main", rect(x0, SOUTH[0], x1, SOUTH[1]), holes=[stack], lightlevel=200)
    g.sector("trans_stack", stack, id=60, lightlevel=200)
    g.free_line((x0, -800), (x1, -800), "trans_main", midtexture="RTGLASS", alpha=0.4,
                wrapmidtex=True, sides=({}, {}))
    g.free_line((x0, -1360), (x1, -1360), "trans_main", midtexture="RTGLASS", alpha=0.7,
                wrapmidtex=True, sides=({}, {}))
    control("stack_low", 32, 64, 100, [60, 100, 0, 0, 0], lightlevel=200)
    control("stack_mid", 96, 128, 100, [60, 160, 1, 0, 0], lightlevel=200)
    control("stack_high", 160, 192, 100, [60, 220, 2, 0, 0], lightlevel=200)

    # 7. Brightmaps: a dim blue room with bright sign walls.
    x0, x1 = room_x(ROOM_X[2])
    g.sector("bright_room", rect(x0, SOUTH[0], x1, SOUTH[1]), wall="RTSIGN", lightlevel=32,
             lightcolor=color(0, 0, 255), lightalpha=20)

    # The control sectors, one 64x64 square each.
    for n, (name, bottom, top, special, args, wall, props) in enumerate(controls):
        x = 3328 + 128 * n
        square = rect(x, 0, x + 64, 64)
        props = {"heightfloor": bottom, "heightceiling": top, "texturefloor": "RTGLASS",
                 "textureceiling": "RTGLASS", **props}
        g.sector(f"ctl_{name}", square, wall=wall, **props)
        g.edge((x, 0), (x + 64, 0), special=special, args=args)

    # The finish line, just in front of the starts (the first waypoint is the finish line's).
    g.free_line((-950, -640), (-950, 640), "hall", special=2001, playercross=True, repeatspecial=True,
                sides=({}, {}))

    # Things: the hall's starts, waypoint loop, item boxes and skybox, then each room's contents.
    for n, (x, y) in enumerate([(-1150, -150), (-1150, 150), (-1050, -50), (-1050, 50)]):
        g.thing(n + 1, x, y)
    loop = [(-950, -400), (0, -400), (1000, -400), (1000, 0), (1000, 400), (0, 400), (-950, 400), (-950, 0)]
    for n, (x, y) in enumerate(loop, 1):
        g.thing(2001, x, y, id=n, thingarg0=n % len(loop) + 1, thingarg1=256, thingarg2=8 if n == 1 else 0)
    for x, y in [(-1000, -250), (-1000, 250), (-700, 0), (1100, -300), (1100, 300), (300, 0)]:
        g.thing(2000, x, y, 24)
    g.thing(780, 0, 0, 160, id=0, thingarg0=0)  # skybox viewpoint
    g.thing(780, 0, 0, 0, id=0, thingarg0=1)  # skybox centre point
    for x, y in [(-300, -1000), (300, -1000)]:
        g.thing(2000, x, y, 24)
    for x, y, z in [(-100, -900, 0), (-60, -1050, 66), (60, -1120, 130), (0, -1000, 198), (-120, -1290, 0),
                    (100, -1420, 0)]:
        g.thing(THING_TRANS, x, y, z)
    for x, y in [(640, -900), (1024, -900), (640, -1280), (1024, -1280)]:
        g.thing(THING_GLOW, x, y, 24)
    return g


# --------------------------------------------------------------------------
# WAD files and ZDBSP
# --------------------------------------------------------------------------

def wad_bytes(lumps):
    body, directory = b"", b""
    for name, data in lumps:
        directory += struct.pack("<ii8s", 12 + len(body), len(data), name.encode("ascii"))
        body += data
    return b"PWAD" + struct.pack("<ii", len(lumps), 12 + len(body)) + body + directory


def read_wad(data):
    assert data[:4] in (b"PWAD", b"IWAD"), "not a WAD"
    count, table = struct.unpack("<ii", data[4:12])
    lumps = []
    for i in range(count):
        offset, size, name = struct.unpack("<ii8s", data[table + 16 * i:table + 16 * i + 16])
        lumps.append((name.rstrip(b"\0").decode("ascii"), data[offset:offset + size]))
    return lumps


def run_zdbsp(zdbsp, textmap):
    """Returns (ZNODES lump, ZDBSP's rewritten TEXTMAP)."""
    with tempfile.TemporaryDirectory() as tmp:
        source, result = Path(tmp) / "in.wad", Path(tmp) / "out.wad"
        source.write_bytes(wad_bytes([(MARKER, b""), ("TEXTMAP", textmap.encode("ascii")), ("ENDMAP", b"")]))
        # -X: extended nodes, not compressed (the engine only reads XGLN and XGL3 ones);
        # -q: keep unused sidedefs and sectors, so the numbering stays the one in TEXTMAP.
        try:
            subprocess.run([str(zdbsp), "-X", "-q", "-t", "-o", str(result), str(source)], check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        except FileNotFoundError:
            sys.exit(f"ZDBSP not found: {zdbsp} (see --zdbsp)")
        except subprocess.CalledProcessError as error:
            sys.exit(f"ZDBSP failed:\n{error.stdout.decode(errors='replace')}")
        lumps = dict(read_wad(result.read_bytes()))
    return lumps["ZNODES"], lumps["TEXTMAP"].decode("ascii")


# --------------------------------------------------------------------------
# Parsing and validation
# --------------------------------------------------------------------------

def parse_value(text):
    if text.startswith('"'):
        assert text.endswith('"'), text
        return text[1:-1]
    if text == "true":
        return True
    try:
        return int(text)
    except ValueError:
        return float(text)


def parse_textmap(text):
    """Parses the TEXTMAP this script writes (and ZDBSP rewrites): one key per line."""
    result = {"globals": {}, "vertex": [], "linedef": [], "sidedef": [], "sector": [], "thing": []}
    block, kind = None, None
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        i += 1
        if not line:
            continue
        if block is None and line in result and line != "globals":
            assert i < len(lines) and lines[i].strip() == "{", f"{line} without a block"
            block, kind = {}, line
            i += 1
        elif line == "}":
            assert block is not None, "stray }"
            result[kind].append(block)
            block = None
        else:
            match = re.fullmatch(r"(\w+) = (.+);", line)
            assert match, f"unexpected line {line!r}"
            target = result["globals"] if block is None else block
            assert match.group(1) not in target, f"duplicate key {line!r}"
            target[match.group(1)] = parse_value(match.group(2))
    assert block is None, "unterminated block"
    return result


def tags_of(sector):
    tags = [sector["id"]] if "id" in sector else []
    return tags + [int(t) for t in str(sector.get("moreids", "")).split()]


def parse_znodes(data):
    """Parses an XGLN ZNODES lump (the engine reads those and XGL3): counts, segs, subsector sizes, nodes."""
    assert data[:4] == b"XGLN", f"ZNODES is {data[:4]!r}; the engine only reads XGLN (and XGL3) nodes"
    pos = 4

    def take(fmt):
        nonlocal pos
        values = struct.unpack_from("<" + fmt, data, pos)
        pos += struct.calcsize("<" + fmt)
        return values

    original, extra = take("II")
    pos += 8 * extra
    (count,) = take("I")
    sizes = [take("I")[0] for _ in range(count)]
    (numsegs,) = take("I")
    segs = [take("IIHB") for _ in range(numsegs)]  # vertex, partner, linedef (0xFFFF: miniseg), side
    (numnodes,) = take("I")
    nodes = [take("hhhh" + "hhhhhhhh" + "II") for _ in range(numnodes)]
    assert pos == len(data), f"{len(data) - pos} unexpected bytes after the nodes"
    return {"original": original, "extra": extra, "sizes": sizes, "segs": segs, "nodes": nodes}


def locate(bsp, x, y):
    """The subsector holding a point, walking the node tree as the engine does."""
    child = len(bsp["nodes"]) - 1
    while not child & 0x80000000:
        nx, ny, dx, dy, *_, right, left = bsp["nodes"][child]
        child = right if dx * (y - ny) < dy * (x - nx) else left  # R_PointOnSide: child 0 is the front
    return child & 0x7FFFFFFF


def check_znodes(data, tm, one_sided=(), keep_whole=()):
    """Structural checks on a ZNODES lump against its TEXTMAP.

    one_sided: linedefs the node builder was given as one-sided polyobject lines.
    keep_whole: (name, x, y, radius, sector) discs that must lie in one subsector of that sector.
    """
    bsp = parse_znodes(data)
    assert bsp["original"] == len(tm["vertex"]), "nodes and TEXTMAP disagree about the vertex count"
    assert sum(bsp["sizes"]) == len(bsp["segs"]), "subsector sizes do not add up to the seg count"
    for vertex, _, linedef, side in bsp["segs"]:
        assert vertex < bsp["original"] + bsp["extra"], "seg vertex out of range"
        assert linedef == 0xFFFF or linedef < len(tm["linedef"]), "seg linedef out of range"
        assert side in (0, 1)
    count = {}
    for _, _, linedef, side in bsp["segs"]:
        if linedef != 0xFFFF:
            count[(linedef, side)] = count.get((linedef, side), 0) + 1
    for n, line in enumerate(tm["linedef"]):
        for side in range(1 if n in one_sided or "sideback" not in line else 2):
            assert count.get((n, side), 0) >= 1, f"linedef {n} side {side} has no seg"
    for n in one_sided:
        assert count.get((n, 0)) == 1 and (n, 1) not in count, f"polyobject linedef {n} was split or doubled"

    subsectors, first = [], 0
    for size in bsp["sizes"]:
        subsectors.append(bsp["segs"][first:first + size])
        first += size
    assert all(any(s[2] != 0xFFFF for s in ss) for ss in subsectors), "subsector without a real seg"
    children = [c for node in bsp["nodes"] for c in node[-2:]]
    assert all(c < len(bsp["nodes"]) if not c & 0x80000000 else (c & 0x7FFFFFFF) < len(subsectors) for c in children)

    def sector_of(ss):
        linedef, side = next((s[2], s[3]) for s in ss if s[2] != 0xFFFF)
        return tm["sidedef"][tm["linedef"][linedef]["sidefront" if side == 0 else "sideback"]]["sector"]

    for name, x, y, radius, sector in keep_whole:
        ring = [(round(radius * math.cos(a * math.pi / 16)), round(radius * math.sin(a * math.pi / 16)))
                for a in range(32)]
        found = {locate(bsp, x + dx, y + dy) for dx, dy in [(0, 0)] + ring}
        assert len(found) == 1, f"{name} spans {len(found)} subsectors around its spawn spot"
        assert sector_of(subsectors[found.pop()]) == sector, f"{name} spawns in the wrong sector"
    return bsp


def validate_map(tm, image_names):
    """What the engine requires of a UDMF map, as far as it can be checked without the game.

    Raises AssertionError. Returns the polyobject linedefs.
    """
    assert list(tm["globals"])[:2] == ["namespace", "version"], "TEXTMAP must start with namespace, then version"
    assert tm["globals"]["namespace"] == "ringracers" and tm["globals"]["version"] == 2
    vertices, lines, sides, sectors, things = tm["vertex"], tm["linedef"], tm["sidedef"], tm["sector"], tm["thing"]
    assert len(sectors) >= 2, "the node builder needs at least two sectors"
    assert all({"x", "y"} <= set(v) for v in vertices)
    for n, line in enumerate(lines):
        assert {"v1", "v2", "sidefront"} <= set(line), f"linedef {n} lacks a required key"
        assert line["v1"] != line["v2"] and 0 <= min(line["v1"], line["v2"]) and max(line["v1"], line["v2"]) < len(vertices)
        assert 0 <= line["sidefront"] < len(sides)
        if "sideback" in line:
            assert 0 <= line["sideback"] < len(sides) and line.get("twosided") is True, f"linedef {n}"
        else:
            assert "twosided" not in line
    assert all(0 <= side["sector"] < len(sectors) for side in sides)
    assert {l[k] for l in lines for k in ("sidefront", "sideback") if k in l} == set(range(len(sides))), "unused sidedefs"
    assert {side["sector"] for side in sides} == set(range(len(sectors))), "a sector has no sidedef"

    # Every sector's boundary is closed: its edges enter and leave each vertex equally often.
    for s in range(len(sectors)):
        balance = {}
        for line in lines:
            for key, forward in (("sidefront", True), ("sideback", False)):
                if key in line and sides[line[key]]["sector"] == s:
                    a, b = (line["v1"], line["v2"]) if forward else (line["v2"], line["v1"])
                    balance[a] = balance.get(a, 0) + 1
                    balance[b] = balance.get(b, 0) - 1
        assert not any(balance.values()), f"sector {s} is not closed"

    # Lines meet only at shared end points: nothing crosses, overlaps or touches another's middle.
    ends = [((vertices[l["v1"]]["x"], vertices[l["v1"]]["y"]), (vertices[l["v2"]]["x"], vertices[l["v2"]]["y"]))
            for l in lines]

    def cross(p, q, r):
        return (r[0] - q[0]) * (p[1] - q[1]) - (r[1] - q[1]) * (p[0] - q[0])

    for i, (a, b) in enumerate(ends):
        for j in range(i + 1, len(ends)):
            c, d = ends[j]
            for p, (q, r) in ((c, (a, b)), (d, (a, b)), (a, (c, d)), (b, (c, d))):
                assert not _on_open_segment(p, q, r), f"lines {i} and {j} touch away from their ends"
            assert not (cross(c, a, b) * cross(d, a, b) < 0 and cross(a, c, d) * cross(b, c, d) < 0), \
                f"lines {i} and {j} cross"

    # 3D floors: each special sits on a control sector's one-sided line and aims at a tag that exists.
    tags = {t for s in sectors for t in tags_of(s)}
    for n, line in enumerate(lines):
        if line.get("special") in (100, 120, 200, 202):
            assert line["arg0"] in tags, f"FOF line {n} targets tag {line['arg0']}, which no sector has"
            assert "sideback" not in line
            control = sectors[sides[line["sidefront"]]["sector"]]
            assert control["heightceiling"] > control["heightfloor"]
            assert line["arg0"] not in tags_of(control)
    for s in sectors:  # colors are decimal RGB numbers (the engine reads them with atol)
        assert all(isinstance(s.get(k, 0), int) and 0 <= s.get(k, 0) <= 0xFFFFFF for k in ("lightcolor", "fadecolor"))

    # Polyobjects: closed two-sided loops (the sector behind the lines gives the planes), with their things.
    polyobject_lines = set()
    for number in sorted({t["id"] for t in things if t["type"] == PO_ANCHOR}):
        assert sum(1 for t in things if t["type"] == PO_ANCHOR and t["id"] == number) == 1
        assert sum(1 for t in things if t["type"] == PO_SPAWN and t["id"] == number) == 1
        starts = [n for n, l in enumerate(lines) if l.get("special") == PO_START_LINE and l["arg0"] == number]
        assert len(starts) == 1, f"polyobject {number} needs exactly one start line"
        start = lines[starts[0]]
        assert start["arg1"] == -1 and "sideback" in start
        inner = sides[start["sideback"]]["sector"]
        garage = sides[start["sidefront"]]["sector"]
        loop = {n for n, l in enumerate(lines) if "sideback" in l and sides[l["sideback"]]["sector"] == inner}
        assert loop == {n for n, l in enumerate(lines) if inner in (sides[l[k]]["sector"] for k in ("sidefront", "sideback") if k in l)}
        assert all(sides[lines[n]["sidefront"]]["sector"] == garage for n in loop)
        assert all(set(sides[lines[n]["sidefront"]]) == {"sector", "texturemiddle"} for n in loop), \
            "polyobject walls take a middle texture only"
        polyobject_lines |= loop
    # Waypoints: every next id exists, and exactly one is the finish line.
    waypoints = [t for t in things if t["type"] == 2001]
    ids = {t["id"] for t in waypoints}
    assert len(ids) == len(waypoints) >= 3 and all(t["thingarg0"] in ids for t in waypoints)
    assert sum(1 for t in waypoints if t.get("thingarg2", 0) & 8) == 1
    assert {1, 2, 3, 4} <= {t["type"] for t in things}
    # Everything the map names ships in the PK3 (or comes with the game).
    shipped = {Path(n).stem.upper() for n in image_names}
    used = {s[k] for s in sectors for k in ("texturefloor", "textureceiling")}
    used |= {v for side in sides for k, v in side.items() if k.startswith("texture")}
    assert not used - shipped - EXTERNAL_TEXTURES, f"missing textures: {used - shipped - EXTERNAL_TEXTURES}"
    return polyobject_lines


def validate_package(entries, models, rewritten=None):
    """The checks behind --validate. Returns a one-line summary."""
    wad = read_wad(entries[MAP_WAD])
    assert [name for name, _ in wad] == [MARKER, "TEXTMAP", "ZNODES", "ENDMAP"]
    lumps = dict(wad)
    geometry = build_geometry()
    lines = geometry.build()
    text = lumps["TEXTMAP"].decode("ascii")
    assert text == emit_textmap(geometry, lines), "the TEXTMAP is not what this script generates"
    tm = parse_textmap(text)
    one_sided = validate_map(tm, [n for n in entries if n.endswith(".png")])
    sent = parse_textmap(emit_textmap(geometry, lines, zdbsp=True))
    if rewritten is not None:
        # The node builder numbered everything the way we did, so its nodes fit the TEXTMAP we ship.
        received = parse_textmap(rewritten)
        for kind in ("vertex", "linedef", "sidedef", "sector"):
            assert len(received[kind]) == len(sent[kind]), f"ZDBSP changed the number of {kind}s"
        assert [(v["x"], v["y"]) for v in received["vertex"]] == [(v["x"], v["y"]) for v in sent["vertex"]]
        for a, b in zip(sent["linedef"], received["linedef"]):
            assert all(a.get(k) == b.get(k) for k in ("v1", "v2", "sidefront", "sideback", "special"))
        assert [s["sector"] for s in received["sidedef"]] == [s["sector"] for s in sent["sidedef"]]
    room = geometry.index("poly_room")
    spawns = [(f"polyobject {t['id']}", t["x"], t["y"], 92, room) for t in tm["thing"] if t["type"] == PO_SPAWN]
    bsp = check_znodes(lumps["ZNODES"], tm, one_sided, spawns)
    assert set(models) == {"models.dat", "rendertest.md3"}
    md3 = models["rendertest.md3"]
    assert md3[:4] == b"IDP3" and struct.unpack_from("<i", md3, 4)[0] == 15
    assert struct.unpack_from("<i", md3, 104)[0] == len(md3), "MD3 end offset"
    return (f"{len(tm['sector'])} sectors, {len(tm['linedef'])} lines, {len(tm['thing'])} things, "
            f"{len(bsp['sizes'])} subsectors")


# --------------------------------------------------------------------------
# Packaging
# --------------------------------------------------------------------------

def map_wad(textmap, znodes):
    return wad_bytes([(MARKER, b""), ("TEXTMAP", textmap.encode("ascii")), ("ZNODES", znodes), ("ENDMAP", b"")])


def build(zdbsp):
    """Returns ({PK3 entry: bytes}, {model file: bytes}, ZDBSP's rewrite of the map it was given)."""
    geometry = build_geometry()
    lines = geometry.build()
    znodes, rewritten = run_zdbsp(zdbsp, emit_textmap(geometry, lines, zdbsp=True))
    entries = {
        "Lua/quiet.lua": (HERE / "quiet.lua").read_bytes(),
        "SOC/rendertest.soc": SOC.encode("ascii"),
        "BRIGHT": BRIGHT.encode("ascii"),
        "SPRTINFO": SPRTINFO.encode("ascii"),
        MAP_WAD: map_wad(emit_textmap(geometry, lines), znodes),
        **{name: make() for name, make in IMAGES},
    }
    models = {"models.dat": MODELS_DAT.encode("ascii"), "rendertest.md3": build_md3()}
    return entries, models, rewritten


def pk3_bytes(entries):
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_STORED) as archive:
        for name, data in entries.items():
            entry = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            entry.create_system = 3
            entry.external_attr = 0o100644 << 16
            archive.writestr(entry, data)
    return output.getvalue()


def read_pk3(path):
    with zipfile.ZipFile(path) as archive:
        assert archive.testzip() is None, f"bad CRC in {path}"
        return {info.filename: archive.read(info) for info in archive.infolist()}


def read_models():
    return {name: (MODELS / name).read_bytes() for name in ("models.dat", "rendertest.md3") if (MODELS / name).exists()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zdbsp", default="zdbsp", help="ZDBSP 1.19 executable (default: zdbsp on PATH)")
    parser.add_argument("--check", action="store_true",
                        help="rebuild and compare with the committed PK3 and models, then validate them")
    parser.add_argument("--validate", action="store_true",
                        help="validate the committed PK3 and models without rebuilding (needs no ZDBSP)")
    args = parser.parse_args()

    if args.validate:
        if not PACKAGE.exists():
            parser.error(f"{PACKAGE.name} is missing; run build.py to create it")
        print(f"Validated {PACKAGE.name}: {validate_package(read_pk3(PACKAGE), read_models())}")
        return

    entries, models, rewritten = build(args.zdbsp)
    summary = validate_package(entries, models, rewritten)

    if args.check:
        if not PACKAGE.exists():
            parser.error(f"{PACKAGE.name} is missing; run build.py to create it")
        committed = read_pk3(PACKAGE)
        if list(committed) != list(entries):
            parser.error(f"PK3 entries differ: {list(committed)} != {list(entries)}")
        for name, data in entries.items():
            if name != MAP_WAD and committed[name] != data:
                parser.error(f"{name} differs from the generated one; run build.py to rebuild it")
        old, new = read_wad(committed[MAP_WAD]), read_wad(entries[MAP_WAD])
        if [n for n, _ in old] != [n for n, _ in new] or dict(old)["TEXTMAP"] != dict(new)["TEXTMAP"]:
            parser.error("the map's lumps or TEXTMAP differ from the generated ones; run build.py to rebuild it")
        for name, data in models.items():
            if read_models().get(name) != data:
                parser.error(f"models/{name} differs from the generated one; run build.py to rebuild it")
        # Node builds can differ a little between machines, so the committed nodes are validated, not compared.
        validate_package(committed, read_models())
        print(f"Verified {PACKAGE.name} and models/: {summary}")
        return

    PACKAGE.write_bytes(pk3_bytes(entries))
    MODELS.mkdir(exist_ok=True)
    for name, data in models.items():
        (MODELS / name).write_bytes(data)
    print(f"Built {PACKAGE.name}: {PACKAGE.stat().st_size} bytes, {len(entries)} entries ({summary})")


if __name__ == "__main__":
    main()
