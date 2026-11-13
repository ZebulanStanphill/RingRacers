#!/usr/bin/env python3
"""Generate the renderer test map add-on (D_RenderTest_v1.pk3), or check the committed copy.

The UDMF map, its textures, flats and sprites, the SOC level header, BRIGHT and
SPRTINFO lumps, and the model files are generated deterministically here. The
map-local scripts are packaged from quiet.lua (bot removal) and scenery.lua
(scenery objects whose editor numbers a game object shares). Only the map's BSP
(the ZNODES lump) comes from an
external tool, ZDBSP, which is why --check compares that one lump structurally.
The map also carries an ENCORE lump, a red/blue swap remap embedded below.
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

# The map's ENCORE lump: for each palette index, the index Encore mode shows
# instead. It swaps red and blue: entry i is the palette color nearest (squared
# RGB distance, lowest index on ties) to color i with its red and blue swapped,
# computed from the v2.4 PLAYPAL (the first 768 bytes of bios.pk3's PLAYPAL).
# The palette is game data we don't ship, so only the result is embedded;
# --encore-from PATH recomputes it from a PLAYPAL file and fails on a mismatch.
ENCORE_REMAP = bytes.fromhex(
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
    "9495969898999a9a9b9c9c9d9e9efdfd919284858687878788888888899d9d9d"
    "838d8d8889898a9f83ffff7b8e8e8f8a80818182828c8c0f788c8c0d10131619"
    "606162636465666768696a6b6c6d6e6f70717273747576775840bcbdbebfbf4f"
    "50515340403334354345ecef55424d4ed0d1d22020212223232527282a2b2d47"
    "c8c9cacbcccccdcfbabbd3d4d6e7faecb0b1b2b3b4b5b6b7b8b9babb7a7b7c7d"
    "c0c1c2c3c4c5c6c7a0a1a2a3acacacad909192aaababacac919292939394abab"
    "abacacacacadadad89aeae8a8aaf8b8baa0c0e0f1112131415aeaeaefc2e2f49"
)

# Editor numbers (MapThingNum) for the add-on's own scenery objects.
THING_TRANS = 3900
THING_GLOW = 3901
THING_PARITY = 3902
DARK_PAD_TAG = 85

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


def doom_patch(width, height, pixels):
    """A Doom-format patch (the engine's softwarepatch_t) of fully opaque palette indices.

    Unlike a PNG, which the engine converts for every reader but Software's texture brightmap path, this is read
    directly by all of them."""
    assert len(pixels) == width * height and 0 < height <= 255
    columns = []
    offset = 8 + 4 * width
    table = bytearray()
    for x in range(width):
        table += struct.pack("<I", offset)
        # One post covering the column: topdelta, length, padding, the pixels, padding; then the end marker.
        column = bytes([0, height, 0]) + bytes(pixels[y * width + x] for y in range(height)) + b"\x00\xff"
        columns.append(column)
        offset += len(column)
    return struct.pack("<hhhh", width, height, 0, 0) + bytes(table) + b"".join(columns)


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


def flat_skyfloor():
    """RTSKYF: the skybox room's floor, teal checks."""
    palette = [(24, 190, 170, 255), (10, 110, 104, 255), (6, 40, 44, 255)]

    def pixel(x, y):
        if x % 32 == 0 or y % 32 == 0:
            return 2
        return (x // 32 + y // 32) % 2

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


def texture_skywall():
    """RTSKYW: the skybox room's walls, saturated magenta tiles that look like nothing in the hall."""
    palette = [(18, 6, 24, 255), (214, 36, 176, 255), (246, 120, 220, 255), (150, 20, 130, 255)]

    def pixel(x, y):
        if x % 32 < 2 or y % 32 < 2:
            return 0
        if x % 32 == 2 or y % 32 == 2:
            return 2
        return 1 if (x // 32 + y // 32) % 2 else 3

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


# A texture brightmap is shipped as a Doom patch, not a PNG: Software reads it raw, without the PNG conversion
# every other image gets. It marks the bright pixels with palette index 0 (BRIGHTPIXEL, white in the palette,
# which is also what Legacy GL and VibeRant D3D test for: a fully red palette color) and the rest with 31
# (black); 255 would be transparent.
BRIGHT_INDEX, DARK_INDEX = 0, 31


def texture_sign_brightmap():
    """RTSIGNBM: marks the glyphs and band edges of RTSIGN; it must be the same size."""
    pixels = bytes(BRIGHT_INDEX if _sign_index(x, y) >= 3 else DARK_INDEX for y in range(128) for x in range(128))
    return doom_patch(128, 128, pixels)


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


def _orb_opaque(x, y):
    """Where RTGLA0 is not transparent. RTGLBRA0 must match it exactly (see sprite_glow_brightmap)."""
    return _orb_distance(x, y) <= 22 * 22


def sprite_glow():
    """RTGLA0: a dark orb with a glowing ring and core, 48x48."""
    palette = [(0, 0, 0, 0), (16, 20, 40, 255), (48, 60, 110, 255), (80, 96, 150, 255),
               (255, 150, 30, 255), (255, 250, 190, 255)]

    def pixel(x, y):
        d = _orb_distance(x, y)
        if not _orb_opaque(x, y):
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
    """RTGLBRA0: marks the ring and core of RTGLA0; it must be the same size and just as transparent.

    Software draws a sprite's columns together with its brightmap's, post by post, so the two must have the same
    posts: transparent exactly where RTGLA0 is."""
    palette = [(0, 0, 0, 0), (0, 0, 0, 255), (255, 255, 255, 255)]

    def pixel(x, y):
        if not _orb_opaque(x, y):
            return 0
        d = _orb_distance(x, y)
        return 2 if d <= 8 * 8 or 13 * 13 <= d <= 15 * 15 else 1

    return render(48, 48, palette, pixel, offsets=(24, 48))


# Parity wing assets: deliberately saturated, with fine detail behind blends.
def texture_pattern():
    return render(128, 128, [(255, 240, 160, 255), (40, 80, 200, 255),
                             (255, 60, 100, 255), (40, 240, 160, 255)],
                  lambda x, y: ((x // 8) + (y // 8)) % 4)


def texture_label(number):
    """One large room/band number, repeated vertically on a coloured panel."""
    digits = ("111101101101111", "010110010010111", "111001111100111",
              "111001111001111", "101101111001001", "111100111001111",
              "111100111101111", "111001001001001", "111101111101111",
              "111101111001111")
    colours = [(220, 70, 60, 255), (60, 200, 100, 255), (70, 130, 240, 255),
               (230, 180, 50, 255), (180, 80, 220, 255), (40, 210, 210, 255),
               (240, 130, 60, 255)]
    # Rooms 8 and 9 get colours no other room panel has (the seven above repeat from room 7 on).
    colours = {8: (240, 110, 170, 255), 9: (150, 110, 60, 255)}.get(number, colours[number % 7])
    text = str(number)

    def pixel(x, y):
        if x < 3 or x >= 125:
            return 0
        xx, yy = x - (128 - len(text) * 32) // 2, y % 128 - 40
        if 0 <= xx < len(text) * 32 and 0 <= yy < 40:
            n, gx = divmod(xx, 32)
            if gx < 24 and digits[int(text[n])][(yy // 8) * 3 + gx // 8] == "1":
                return 2
        return 1

    return render(128, 128, [(12, 16, 24, 255), colours, (255, 255, 255, 255)], pixel)


def texture_fogpanel():
    return render(128, 256, [(40, 180, 220, 255), (255, 255, 255, 255),
                             (20, 60, 100, 255)],
                  lambda x, y: 2 if x % 64 < 4 or y % 64 < 4 else (1 if (x-y) % 64 < 4 else 0))


def texture_treeline():
    def pixel(x, y):
        # 512-high patch, with its opaque tips at z=352, below the front sky
        # ceiling (382). The zero-height back must not clip any of it.
        top = 160 + abs(x % 128 - 64) * 3
        return 0 if y < top else (1 if (x // 8 + y // 16) % 2 else 2)
    return render(256, 512, [(0, 0, 0, 0), (20, 90, 40, 255),
                             (70, 180, 70, 255)], pixel)


def sprite_parity():
    return render(64, 192, [(0, 0, 0, 0), (255, 240, 80, 255),
                            (240, 60, 120, 255), (60, 230, 250, 255)],
                  lambda x, y: 0 if x < 4 or x >= 60 or y < 4 else 1 + (y // 24) % 3,
                  offsets=(32, 192))


# Ordered so that every folder's entries are contiguous: the engine finds a
# PK3 folder as a run of consecutive entries.
IMAGES = [
    ("Textures/RTWALL.png", texture_wall),
    ("Textures/RTPOLY.png", texture_poly),
    ("Textures/RTSIGN.png", texture_sign),
    ("Textures/RTSIGNBM.lmp", texture_sign_brightmap),
    ("Textures/RTSKYW.png", texture_skywall),
    ("Textures/RTWATER.png", texture_water),
    ("Textures/RTFOG.png", texture_fog),
    ("Textures/RTGLASS.png", texture_glass),
    ("Textures/RTPATT.png", texture_pattern),
    ("Textures/RTTREE.png", texture_treeline),
    ("Textures/RTPANE.png", texture_fogpanel),
    *[(f"Textures/RTN{n:02}.png", lambda n=n: texture_label(n)) for n in range(40)],
    ("Flats/RTFLOOR.png", flat_floor),
    ("Flats/RTCEIL.png", flat_ceiling),
    ("Flats/RTSKYF.png", flat_skyfloor),
    ("Sprites/RTTRA0.png", sprite_crystal),
    ("Sprites/RTGLA0.png", sprite_glow),
    ("Sprites/RTPRA0.png", sprite_parity),
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
MT_RTPARITY
S_RTPARITY
SPR_RTPR
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

Object MT_RTPARITY
MapThingNum = {THING_PARITY}
SpawnState = S_RTPARITY
SpawnHealth = 1000
Radius = 48*FRACUNIT
Height = 192*FRACUNIT
Flags = MF_SCENERY|MF_NOBLOCKMAP|MF_NOGRAVITY

State S_RTPARITY
SpriteName = RTPR
SpriteFrame = FF_FULLBRIGHT|A
Duration = -1
Next = S_NULL

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
ROOM_X = (-832, 0, 832)  # room centres; 768 wide behind a 512-wide doorway; water extends farther north
NORTH, SOUTH = (704, 1472), (-1472, -704)
# The skybox room, far west of the hall: enclosed, unreachable, with a sky ceiling so it rains in it.
SKY_ROOM = (-3600, -512, -2576, 512)
SKY_CENTER = ((SKY_ROOM[0] + SKY_ROOM[2]) // 2, (SKY_ROOM[1] + SKY_ROOM[3]) // 2)
SKY_CAMERA_HEIGHT = 64
# The sky pit, off the hall's east end: a covered doorway and viewing area (ordinary ceiling, so no rain
# there) looking onto a sector whose floor is sky and whose ceiling is not, so it gets no rain either.
PIT_DOOR = (1280, -256, 1408, 256)
PIT_WALK = (1408, -448, 1792, 448)
SKY_PIT = (1792, -448, 3200, 448)
PIT_CEILING, PIT_FLOOR = 320, -32


# Room 8: the west x of each bay (768 wide, 128 apart), its sector light, and the posts' distances north of the
# corridor. Room 9: the x range of the one walkable sector, and its sky ceiling height.
SEMI_X = (3456, 4352)
SEMI_LIGHT = (128, 96)
SEMI_ROWS = (256, 640, 1024)
SKYRISE_X = (5248, 6784)
SKYRISE_CEILING = 224


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

    # 3. Water: a deep pool with entrance/exit ramps, overlapping volumes, pillars and a dry heat-haze ledge.
    x0, x1 = room_x(ROOM_X[1])
    entrance_slope = {"floorplane_a": 0, "floorplane_b": 0.5, "floorplane_c": 1, "floorplane_d": -352}
    exit_slope = {"floorplane_a": 0, "floorplane_b": -0.5, "floorplane_c": 1, "floorplane_d": 2144}
    pillars = [rect(-288, 1920, -192, 2016), rect(-48, 2112, 48, 2208), rect(192, 3072, 288, 3168)]
    corner = rect(128, 2240, 384, 3008)
    g.sector("water_entrance", rect(x0, 704, x1, 1728), id=30, lightlevel=200, **entrance_slope)
    g.sector("water_main", [(x0, 1728), (x1, 1728), (x1, 2240), (128, 2240),
                            (128, 3008), (x1, 3008), (x1, 3264), (x0, 3264)], holes=pillars,
             heightfloor=-512, id=30, lightlevel=200)
    g.sector("water_corner", corner, heightfloor=-512, id=30, moreids="31", lightlevel=200)
    for n, p in enumerate(pillars):
        g.sector(f"water_pillar{n}", p, heightfloor=192, lightlevel=200)
    g.sector("water_exit", rect(x0, 3264, x1, 4544), heightfloor=-512, id=30, lightlevel=200, **exit_slope)
    g.sector("water_ledge", rect(x0, 4544, x1, 4800), heightfloor=128, lightlevel=200, heatwave=True)
    g.free_line((x0, 2048), (x1, 2048), "water_main", midtexture="RTGLASS", alpha=0.5,
                wrapmidtex=True, midpeg=True, sides=({}, {}))
    control("water_big", -768, 96, 120, [30, 128, 0, 0, 0], wall="RTWATER", texturefloor="RTWATER",
            textureceiling="RTWATER", lightlevel=200, lightcolor=color(32, 96, 255), lightalpha=24)
    control("water_small", -768, 48, 120, [31, 96, 0, 0, 0], wall="RTWATER", texturefloor="RTWATER",
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

    # 8. The skybox room: bright, saturated and unlike the hall, so the sky above the hall is easy to
    # recognise; its sky ceiling makes it rain around the skybox camera.
    g.sector("sky_room", rect(*SKY_ROOM), wall="RTSKYW", heightceiling=384, texturefloor="RTSKYF",
             textureceiling="F_SKY1", lightlevel=255)

    # 9. The sky pit: the only place where sky is visible but no precipitation can exist, so any rain in the
    # skybox picture shown there is a bug. Rain only spawns in sectors with a sky ceiling, and none of
    # these has one. The line along the pit's edge blocks karts.
    g.sector("pit_door", rect(*PIT_DOOR), heightceiling=PIT_CEILING, lightlevel=180)
    g.sector("pit_walk", rect(*PIT_WALK), heightceiling=PIT_CEILING, lightlevel=200)
    g.sector("sky_pit", rect(*SKY_PIT), heightfloor=PIT_FLOOR, heightceiling=PIT_CEILING, texturefloor="F_SKY1",
             lightlevel=200)
    g.edge((SKY_PIT[0], SKY_PIT[1]), (SKY_PIT[0], SKY_PIT[3]), blocking=True)

    # 10. Software parity wing. Enter through Brightmaps, then turn east.
    # All adjoining floors meet at zero.
    g.sector("parity_entry", rect(576, -1792, 1088, -1472), wall="RTN00", lightlevel=200)
    g.sector("parity_hall", rect(448, -2048, 9472, -1792), wall="RTN00", lightlevel=200)

    # 1: two equal windows, fog on the west, ordinary translucency on the east.
    g.sector("fogline_front", rect(1408, -2560, 2432, -2048), wall="RTN01",
             lightlevel=160, lightcolor=color(255, 60, 20), lightalpha=12)
    for n, (a, b) in enumerate(((1408, 1920), (1920, 2432))):
        g.sector(f"fogline_back{n}", rect(a, -3072, b, -2560), wall="RTPATT", lightlevel=255)
        g.edge((a, -2560), (b, -2560), midtexture="RTPANE", midpeg=True,
               renderstyle="fog" if n == 0 else "translucent", alpha=0.5, sides=({}, {}))

    # 2: identical colormaps above AND below a sprite-cutting light boundary.
    for n, (a, b) in enumerate(((2560, 3200), (3328, 3968))):
        cm = dict(fadecolor=color(20, 40, 120), fadestart=0, fadeend=20,
                  lightcolor=color(255, 255, 255), lightalpha=0)
        if n == 0:
            cm["colormapfadesprites"] = True
        g.sector(f"fade_room{n}", rect(a, -5632, b, -2048), wall="RTN02", id=81+n,
                 heightceiling=384, lightlevel=160, **cm)
        control(f"fade_slice{n}", 80, 96, 200, [81+n, 0], lightlevel=128, **cm)
        for y in range(-2432, -5505, -512):
            g.thing(THING_PARITY, (a+b)//2, y)

    # 3: flat pair, then a pair on a gentle ramp. P_TryMove assigns standingslope.
    g.sector("shadow_flat", rect(4096, -2560, 5120, -2048), wall="RTN03", lightlevel=200)
    g.sector("shadow_slope", rect(4096, -3072, 5120, -2560), wall="RTN03", lightlevel=200,
             heightceiling=384, floorplane_a=0, floorplane_b=0.25, floorplane_c=1, floorplane_d=640)
    for y in (-2368, -2880):
        for x, variant in ((4352, 1), (4864, 0)):
            g.thing(THING_PARITY, x, y, thingarg0=variant)

    # 4: frame overlay/trans50, renderflags overlay/trans50, plain trans50, opaque overlay.
    g.sector("overlay_room", rect(5248, -3072, 6272, -2048), wall="RTPATT", lightlevel=255)
    g.edge((5248, -2048), (6272, -2048), sides=({"texturetop": "RTN04"}, {"texturetop": "RTN04"}))
    g.edge((5248, -3072), (5248, -2048), sides=({"texturemiddle": "RTN04"},))
    for x, variant in zip((5376, 5632, 5888, 6144), (2, 3, 4, 5)):
        g.thing(THING_PARITY, x, -2880, thingarg0=variant)

    # 5: marked pad, looking south at three identical bays with different light.
    pad = rect(6848, -2304, 7104, -2112)
    g.sector("dark_front", rect(6400, -2560, 7552, -2048), holes=[pad], wall="RTN05", lightlevel=200)
    g.sector("dark_pad", pad, id=DARK_PAD_TAG, texturefloor="RTN05", lightlevel=200)
    for n, light in enumerate((255, 160, 96)):
        a = 6400 + n * 384
        g.sector(f"dark_bay{n}", rect(a, -3072, a+384, -2560), wall="RTPATT", lightlevel=light)
        g.thing(THING_PARITY, a+192, -2880)

    # 6: Municipal Meadow's two-sky edge, with a masked midtexture taller than the back.
    g.sector("skyedge_walk", rect(7680, -3072, 8704, -2048), wall="RTN06",
             heightceiling=382, textureceiling="F_SKY1", lightlevel=255)
    g.sector("skyedge_back", rect(7680, -3136, 8704, -3072), heightfloor=-2, heightceiling=-2,
             texturefloor="F_SKY1", textureceiling="F_SKY1", lightlevel=255)
    g.edge((7680, -3072), (8704, -3072), midtexture="RTTREE", midpeg=True,
           blocking=True, sides=({}, {}))

    # 7: forty numbered 128-unit bands facing a spot 1024 units from the wall.
    g.sector("ruler_passage", rect(9216, -5888, 9472, -2048), wall="RTN07", lightlevel=200)
    spot = rect(6848, -7488, 6976, -7360)
    outline = [(4352, -8448)] + [(4352+n*128, -8448) for n in range(1, 41)] + [(9472, -5888), (4352, -5888)]
    g.sector("ruler_room", outline, holes=[spot], wall="RTN07", heightceiling=512, lightlevel=255)
    g.sector("ruler_spot", spot, texturefloor="RTN07", heightceiling=512, lightlevel=255)
    for n in range(40):
        a = 4352+n*128
        g.edge((a, -8448), (a+128, -8448), sides=({"texturemiddle": f"RTN{n:02}"},))

    # 8: two bays north of the corridor, 768 wide and 1280 deep: sector light 128 and 96. Each holds three
    # columns of posts (west: ordinary light, middle: semibright, east: fullbright) in three rows.
    for n, (x0, light) in enumerate(zip(SEMI_X, SEMI_LIGHT)):
        g.sector(f"semi_bay{n}", rect(x0, -1792, x0 + 768, -512), wall="RTN08", lightlevel=light)
        for dy in SEMI_ROWS:
            for dx, variant in zip((128, 384, 640), (6, 7, 0)):
                extra = {"thingarg0": variant} if variant else {}
                g.thing(THING_PARITY, x0 + dx, -1792 + dy, **extra)

    # 9: a masked tree-line texture whose tips (z=352) rise above the sky ceilings (224) of both sides. One
    # walkable sector, with two different north edges: the west bay is room 6's arrangement (a zero-height
    # sky sector behind the line); the east bay has an ordinary open strip behind it. A 64-unit solid wall
    # between them keeps the two back sectors from touching.
    x0, x1 = SKYRISE_X
    gap0, gap1 = (x0 + x1) // 2 - 32, (x0 + x1) // 2 + 32
    g.sector("skyrise_walk", rect(x0, -1792, x1, -768), wall="RTN09", heightceiling=SKYRISE_CEILING,
             textureceiling="F_SKY1", lightlevel=255)
    g.sector("skyrise_back", rect(x0, -768, gap0, -704), wall="RTN09", heightfloor=-2, heightceiling=-2,
             texturefloor="F_SKY1", textureceiling="F_SKY1", lightlevel=255)
    g.sector("skyrise_strip", rect(gap1, -768, x1, -512), wall="RTN09", heightceiling=SKYRISE_CEILING,
             textureceiling="F_SKY1", lightlevel=255)
    g.edge((x0, -768), (gap0, -768), midtexture="RTTREE", midpeg=True, blocking=True, sides=({}, {}))
    g.edge((gap1, -768), (x1, -768), midtexture="RTTREE", midpeg=True, blocking=True, sides=({}, {}))

    # The control sectors, one 64x64 square each.
    for n, (name, bottom, top, special, args, wall, props) in enumerate(controls):
        x = 3328 + 128 * n
        square = rect(x, 0, x + 64, 64)
        props = {"heightfloor": bottom, "heightceiling": top, "texturefloor": "RTGLASS",
                 "textureceiling": "RTGLASS", **props}
        g.sector(f"ctl_{name}", square, wall=wall, **props)
        g.edge((x, 0), (x + 64, 0), special=special, args=args)

    # Things: the hall's starts, item boxes and skybox, then each room's contents. The map is for driving
    # around freely, so it has no finish line, checkpoint or waypoints: without waypoints the engine shows
    # the item boxes from the start, never calls a direction wrong (no pointing hand) and respawns at a start.
    for n, (x, y) in enumerate([(-1150, -150), (-1150, 150), (-1050, -50), (-1050, 50)]):
        g.thing(n + 1, x, y)
    for x, y in [(-1000, -250), (-1000, 250), (-700, 0), (1100, -300), (1100, 300), (300, 0)]:
        g.thing(2000, x, y, 24)
    # The skybox viewpoint, at camera height in the middle of the skybox room. There is no centre point,
    # so the sky camera stays here and only turns with the player.
    g.thing(780, *SKY_CENTER, SKY_CAMERA_HEIGHT, id=0, thingarg0=0)
    for dx, dy in [(-320, -320), (-320, 320), (320, -320), (320, 320)]:
        g.thing(THING_GLOW, SKY_CENTER[0] + dx, SKY_CENTER[1] + dy, 24)
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


def _in_sector(sector, x, y, lines, sides, vertices):
    """Whether a point is inside a sector (even-odd: the holes count out): casts a ray towards +x."""
    inside = False
    for line in lines:
        if not any(k in line and sides[line[k]]["sector"] == sector for k in ("sidefront", "sideback")):
            continue
        a, b = vertices[line["v1"]], vertices[line["v2"]]
        if (a["y"] > y) != (b["y"] > y):
            if a["x"] + (y - a["y"]) * (b["x"] - a["x"]) / (b["y"] - a["y"]) > x:
                inside = not inside
    return inside


def validate_map(tm, image_names, entries_png):
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
    # Free driving: four starts, and no waypoints (2001), checkpoint posts (2030) or finish line (special
    # 2001). Any waypoint would bring back the wrong-way hand and hide the item boxes until a checkpoint.
    assert {1, 2, 3, 4} <= {t["type"] for t in things}
    assert not any(t["type"] in (2001, 2030) for t in things), "no waypoints or checkpoints"
    assert not any(l.get("special") == 2001 for l in lines), "no finish line"
    # The skybox viewpoint (no centre point, so the sky camera stays put) lies in a sector with a sky ceiling.
    views = [t for t in things if t["type"] == 780]
    assert len(views) == 1 and views[0]["thingarg0"] == 0 and views[0]["id"] == 0, "one skybox viewpoint, ID 0"
    holder = [i for i in range(len(sectors)) if _in_sector(i, views[0]["x"], views[0]["y"], lines, sides, vertices)]
    assert len(holder) == 1, "the skybox viewpoint is not inside exactly one sector"
    sky = sectors[holder[0]]
    assert sky["textureceiling"] == "F_SKY1", "the skybox viewpoint needs a sky ceiling, so that it rains there"
    assert sky["heightfloor"] < views[0]["height"] < sky["heightceiling"] - 64
    # The sky pit: sky floors under ceilings that are not sky (rain only spawns under a sky ceiling, so none
    # reaches a pit and the skybox picture in it is rain-free unless the renderer draws precipitation there),
    # and the hall and the skybox room still have their sky ceilings and their rain. A line between a pit and
    # ordinary floor blocks karts.
    sky_floors = [i for i, s in enumerate(sectors) if s["texturefloor"] == "F_SKY1" and s["heightfloor"] != s["heightceiling"]]
    assert sky_floors, "the map needs a sky pit"
    assert all(sectors[i]["textureceiling"] != "F_SKY1" for i in sky_floors), "a sky floor under a sky ceiling gets rain"
    assert all(sectors[i]["heightceiling"] - sectors[i]["heightfloor"] >= 256 for i in sky_floors), "a pit is too shallow to see into"
    assert sum(1 for s in sectors if s["textureceiling"] == "F_SKY1") >= 2, "the hall and the skybox room need sky ceilings"
    for n, line in enumerate(lines):
        pair = [sides[line[k]]["sector"] for k in ("sidefront", "sideback") if k in line]
        if len(pair) == 2 and (pair[0] in sky_floors) != (pair[1] in sky_floors):
            assert line.get("blocking") is True, f"line {n} lets karts drive into a sky pit"
    # The parity rooms depend on these exact UDMF and Lua-facing fields.
    geometry = build_geometry()
    def named(name):
        return sectors[geometry.index(name)]

    foglines = [l for l in lines if l.get("renderstyle") == "fog"]
    assert len(foglines) == 1
    fogline = foglines[0]
    assert fogline.get("twosided") and fogline.get("midpeg")
    assert sides[fogline["sidefront"]]["texturemiddle"] == "RTPANE"
    front = sectors[sides[fogline["sidefront"]]["sector"]]
    assert front["lightlevel"] == 160 and front["lightalpha"] == 12
    assert front["lightcolor"] == color(255, 60, 20)
    fades = [named(f"fade_room{n}") for n in range(2)]
    assert fades[0].get("colormapfadesprites") is True
    assert "colormapfadesprites" not in fades[1]
    assert {k: v for k, v in fades[0].items() if k not in ("id", "colormapfadesprites")} == \
           {k: v for k, v in fades[1].items() if k != "id"}
    for n, room in enumerate(fades):
        ctl = named(f"ctl_fade_slice{n}")
        assert (ctl["heightfloor"], ctl["heightceiling"]) == (80, 96)
        masters = [l for l in lines if sides[l["sidefront"]]["sector"] == geometry.index(f"ctl_fade_slice{n}")
                   and l.get("special") == 200]
        assert len(masters) == 1 and masters[0]["arg0"] == room["id"]
        assert room["fadestart"] == 0 and room["fadeend"] == 20 and room["fadecolor"] != 0
        for key in ("fadecolor", "fadestart", "fadeend", "colormapfadesprites"):
            assert ctl.get(key) == room.get(key)
        assert sum(t["type"] == THING_PARITY and 2560+n*768 < t["x"] < 3200+n*768
                   and -5632 < t["y"] < -2048 for t in things) == 7
    assert named("dark_pad")["id"] == DARK_PAD_TAG
    assert named("dark_pad")["texturefloor"] == "RTN05"
    assert _in_sector(geometry.index("dark_pad"), 6976, -2240, lines, sides, vertices)
    assert _in_sector(geometry.index("ruler_spot"), 6912, -7424, lines, sides, vertices)
    assert [named(f"dark_bay{n}")["lightlevel"] for n in range(3)] == [255, 160, 96]
    back = named("skyedge_back")
    assert back["heightfloor"] == back["heightceiling"] == -2
    assert back["texturefloor"] == back["textureceiling"] == "F_SKY1"
    # Room 9's west bay has a second zero-height sky sector, behind its own copy of room 6's edge.
    assert [i for i, sector in enumerate(sectors) if sector["texturefloor"] == "F_SKY1"
            and sector["heightfloor"] == sector["heightceiling"]] == \
           [geometry.index("skyedge_back"), geometry.index("skyrise_back")]
    skyedges = [l for l in lines if sides[l["sidefront"]].get("texturemiddle") == "RTTREE"]
    assert len(skyedges) == 3
    for edge in skyedges:
        assert all(edge.get(k) is True for k in ("twosided", "midpeg", "blocking"))
        assert sides[edge["sidefront"]]["texturemiddle"] == sides[edge["sideback"]].get("texturemiddle", "RTTREE")
    edge = next(l for l in skyedges if vertices[l["v1"]]["y"] == -3072)
    assert sectors[sides[edge["sideback"]]["sector"]] == back

    # Room 8: two bays of nine posts, light 128 and 96, three columns of ordinary, semibright and fullbright.
    variants = {t.get("thingarg0", 0) for t in things if t["type"] == THING_PARITY}
    assert variants == set(range(8)), f"parity post variants are {sorted(variants)}, not 0 to 7"
    assert [named(f"semi_bay{n}")["lightlevel"] for n in range(2)] == [128, 96]
    for n in range(2):
        bay = named(f"semi_bay{n}")
        assert "lightcolor" not in bay and "fadecolor" not in bay, "the dark bays have no colormap"
        assert bay["textureceiling"] != "F_SKY1" and bay["heightceiling"] == 256
        x0 = SEMI_X[n]
        posts = [t for t in things if t["type"] == THING_PARITY and x0 < t["x"] < x0 + 768 and -1792 < t["y"] < -512]
        assert len(posts) == 9, f"semi_bay{n} has {len(posts)} posts, not 9"
        for dy in SEMI_ROWS:
            row = sorted((t["x"] - x0, t.get("thingarg0", 0)) for t in posts if t["y"] == -1792 + dy)
            assert row == [(128, 6), (384, 7), (640, 0)], f"semi_bay{n}, row {dy} north: {row}"
        assert all(_in_sector(geometry.index(f"semi_bay{n}"), t["x"], t["y"], lines, sides, vertices) for t in posts)

    # Room 9: the tree line's opaque tips (z=352, texture bottom at the floor) rise above the sky ceilings
    # (224) on both sides of both its edges: the west bay's zero-height sky sector, and the east bay's strip.
    rise = geometry.index("skyrise_walk")
    walk, rise_back, strip = named("skyrise_walk"), named("skyrise_back"), named("skyrise_strip")
    assert walk["textureceiling"] == strip["textureceiling"] == "F_SKY1"
    assert walk["heightceiling"] == strip["heightceiling"] == SKYRISE_CEILING
    assert walk["heightfloor"] == strip["heightfloor"] == 0
    assert walk["lightlevel"] == strip["lightlevel"] == rise_back["lightlevel"] == 255
    assert rise_back["heightfloor"] == rise_back["heightceiling"] == -2
    assert rise_back["texturefloor"] == rise_back["textureceiling"] == "F_SKY1"
    assert strip["texturefloor"] != "F_SKY1" and not {"colormapfog"} & set(strip)
    tree_top = tree_tip_height(entries_png["Textures/RTTREE.png"])
    assert tree_top > SKYRISE_CEILING, f"RTTREE's tips reach {tree_top}, not above the {SKYRISE_CEILING} sky ceilings"
    rise_edges = [l for l in skyedges if sides[l["sidefront"]]["sector"] == rise]
    assert len(rise_edges) == 2
    assert sorted(sides[l["sideback"]]["sector"] for l in rise_edges) == \
           sorted([geometry.index("skyrise_back"), geometry.index("skyrise_strip")])
    for edge in rise_edges:
        back_sector = sectors[sides[edge["sideback"]]["sector"]]
        assert back_sector["textureceiling"] == "F_SKY1"
        assert back_sector["heightceiling"] in (-2, SKYRISE_CEILING)
    strip_lines = [l for l in lines if strip is sectors[sides[l["sidefront"]]["sector"]]
                   or ("sideback" in l and strip is sectors[sides[l["sideback"]]["sector"]])]
    assert sum("sideback" in l for l in strip_lines) == 1, "the strip is open only behind the tree line"
    bands = [l for l in lines if vertices[l["v1"]]["y"] == vertices[l["v2"]]["y"] == -8448]
    assert len(bands) == 40
    assert all(abs(vertices[l["v1"]]["x"] - vertices[l["v2"]]["x"]) == 128 for l in bands)
    assert {sides[l["sidefront"]]["texturemiddle"] for l in bands} == {f"RTN{n:02}" for n in range(40)}
    # Every parity room remains accessible from the hall through unblocked
    # shared edges; the sky back and control sectors deliberately do not.
    reached = {geometry.index("hall")}
    while True:
        before = len(reached)
        for line in lines:
            if "sideback" not in line or line.get("blocking"):
                continue
            pair = {sides[line[k]]["sector"] for k in ("sidefront", "sideback")}
            if pair & reached:
                reached |= pair
        if len(reached) == before:
            break
    assert all(geometry.index(name) in reached for name in
               ("fogline_front", "fade_room0", "fade_room1", "shadow_flat", "shadow_slope",
                "overlay_room", "dark_pad", "skyedge_walk", "ruler_spot", "semi_bay0", "semi_bay1",
                "skyrise_walk"))
    # Everything the map names ships in the PK3 (or comes with the game).
    shipped = {Path(n).stem.upper() for n in image_names}
    used = {s[k] for s in sectors for k in ("texturefloor", "textureceiling")}
    used |= {v for side in sides for k, v in side.items() if k.startswith("texture")}
    assert not used - shipped - EXTERNAL_TEXTURES, f"missing textures: {used - shipped - EXTERNAL_TEXTURES}"
    return polyobject_lines


def png_opaque_mask(data):
    """Reads back a PNG that png_indexed wrote: (width, height, one boolean per pixel, True where not transparent)."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    chunks, pos = {}, 8
    while pos < len(data):
        (length,) = struct.unpack_from(">I", data, pos)
        chunks[data[pos + 4:pos + 8]] = data[pos + 8:pos + 8 + length]
        pos += 12 + length
    width, height, depth, kind = struct.unpack(">IIBB", chunks[b"IHDR"][:10])
    assert (depth, kind) == (8, 3), "not an 8-bit palette PNG"
    alphas = chunks.get(b"tRNS", b"")
    raw = zlib.decompress(chunks[b"IDAT"])
    assert len(raw) == height * (width + 1) and all(raw[y * (width + 1)] == 0 for y in range(height))
    return width, height, [raw[y * (width + 1) + 1 + x] >= len(alphas) or alphas[raw[y * (width + 1) + 1 + x]] != 0
                           for y in range(height) for x in range(width)]


def tree_tip_height(data):
    """The height above a texture's bottom of the topmost opaque pixel of an indexed PNG (with a pegged bottom)."""
    width, height, opaque = png_opaque_mask(data)
    top = min(i // width for i, solid in enumerate(opaque) if solid)
    return height - top


def doom_patch_pixels(data):
    """Reads back a Doom patch that doom_patch wrote: (width, height, palette indices, None where transparent)."""
    width, height = struct.unpack_from("<hh", data)
    pixels = [None] * (width * height)
    for x in range(width):
        (pos,) = struct.unpack_from("<I", data, 8 + 4 * x)
        while data[pos] != 0xFF:
            top, length = data[pos], data[pos + 1]
            for i in range(length):
                pixels[(top + i) * width + x] = data[pos + 3 + i]
            pos += length + 4
    return width, height, pixels


def validate_brightmaps(entries):
    """What Software needs of brightmaps, which Legacy GL and VibeRant D3D are more forgiving about."""
    # Software reads a texture brightmap as a raw Doom patch (R_GenerateTextureBrightmap), with no PNG conversion.
    lump = entries["Textures/RTSIGNBM.lmp"]
    assert lump[:8] != b"\x89PNG\r\n\x1a\n", "RTSIGNBM must be a Doom patch, not a PNG: Software cannot read a PNG brightmap"
    assert "Textures/RTSIGNBM.png" not in entries
    width, height, pixels = doom_patch_pixels(lump)
    base_width, base_height, base_opaque = png_opaque_mask(entries["Textures/RTSIGN.png"])
    assert (width, height) == (base_width, base_height), "RTSIGNBM must be as large as RTSIGN"
    assert all(p in (BRIGHT_INDEX, DARK_INDEX) for p in pixels), "RTSIGNBM may use only palette indices 0 and 31"
    assert all(base_opaque), "RTSIGN is expected to be opaque"
    assert [p == BRIGHT_INDEX for p in pixels] == [_sign_index(x, y) >= 3 for y in range(height) for x in range(width)]
    # Software draws a sprite's posts and its brightmap's together (R_DrawMaskedColumn), one for one.
    sprite = png_opaque_mask(entries["Sprites/RTGLA0.png"])
    brightmap = png_opaque_mask(entries["Graphics/RTGLBRA0.png"])
    assert sprite[:2] == brightmap[:2], "RTGLBRA0 must be as large as RTGLA0"
    assert sprite[2] == brightmap[2], "RTGLBRA0 must be transparent exactly where RTGLA0 is"
    assert not all(sprite[2]) and any(sprite[2]), "RTGLA0 is expected to be partly transparent"


def validate_scenery_script(source):
    """scenery.lua must use the editor numbers the SOC and the map use."""
    for name, number in (("THING_TRANS", THING_TRANS), ("THING_GLOW", THING_GLOW),
                         ("THING_PARITY", THING_PARITY), ("DARK_PAD_TAG", DARK_PAD_TAG)):
        found = re.findall(rf"^local {name} = (\d+)$", source, re.M)
        assert found == [str(number)], f"scenery.lua has {name} = {found}, but build.py uses {number}"
    for object_type in ("MT_RTTRANS", "MT_RTGLOW", "MT_RTPARITY"):
        assert object_type in source and f"Object {object_type}" in SOC, f"{object_type} is not used by both"

    for required in ("mt.args[0]", "RF_SHADOWDRAW", "P_TryMove", "FF_FULLBRIGHT",
                     "FF_OVERLAY|FF_TRANS50", "RF_OVERLAY|RF_TRANS50",
                     "variant == 6", "variant == 7", "mo.renderflags = $|RF_SEMIBRIGHT",
                     'COM_AddCommand("rendertest_darkness"', "player.darkness_start",
                     "player.darkness_end", "mo.subsector.sector.tag == DARK_PAD_TAG"):
        assert required in source, f"scenery.lua lacks {required}"
    assert "SpriteFrame = FF_FULLBRIGHT|A" in SOC
    # Variant 6 must be ordinary-lit: the base frame is fullbright, so it has to drop that flag (the renderflags'
    # brightness bits, which take precedence, stay clear). Variant 7 sets them to semibright.
    ordinary = re.search(r"variant == 6 then\n(.*?)\n\s*elseif", source, re.S)
    assert ordinary and "mo.frame = A\n" in ordinary.group(1) + "\n" and "RF_" not in ordinary.group(1), \
        "scenery.lua variant 6 must set mo.frame = A (no FF_FULLBRIGHT) and no render flags"
    semibright = re.search(r"variant == 7 then\n(.*?)\n\s*(?:elseif|end)", source, re.S)
    assert semibright and "mo.renderflags = $|RF_SEMIBRIGHT" in semibright.group(1), \
        "scenery.lua variant 7 must add RF_SEMIBRIGHT"


def encore_remap(playpal):
    """The red/blue swap remap for a PLAYPAL (only its first palette, 768 bytes, is used)."""
    assert len(playpal) >= 768, "PLAYPAL is shorter than one palette"
    palette = [tuple(playpal[i * 3:i * 3 + 3]) for i in range(256)]

    def nearest(color):
        return min(range(256), key=lambda j: sum((a - b) ** 2 for a, b in zip(color, palette[j])))

    return bytes(nearest((b, g, r)) for r, g, b in palette)


def validate_package(entries, models, rewritten=None):
    """The checks behind --validate. Returns a one-line summary."""
    validate_brightmaps(entries)
    for asset, size in (("Sprites/RTPRA0.png", (64, 192)), ("Textures/RTTREE.png", (256, 512)),
                        ("Textures/RTPANE.png", (128, 256))):
        width, height, opaque = png_opaque_mask(entries[asset])
        assert (width, height) == size
        if asset != "Textures/RTPANE.png":
            assert any(opaque) and not all(opaque), f"{asset} needs a transparent silhouette"
    validate_scenery_script(entries["Lua/scenery.lua"].decode("utf-8"))
    wad = read_wad(entries[MAP_WAD])
    assert [name for name, _ in wad] == [MARKER, "TEXTMAP", "ZNODES", "ENCORE", "ENDMAP"]
    lumps = dict(wad)
    assert len(lumps["ENCORE"]) == 256, "the engine ignores an ENCORE lump that is not 256 bytes"
    assert lumps["ENCORE"] == ENCORE_REMAP, "the ENCORE lump is not the red/blue swap remap"
    geometry = build_geometry()
    lines = geometry.build()
    text = lumps["TEXTMAP"].decode("ascii")
    assert text == emit_textmap(geometry, lines), "the TEXTMAP is not what this script generates"
    tm = parse_textmap(text)
    one_sided = validate_map(tm, [n for n in entries if n.endswith((".png", ".lmp"))], entries)
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
    return wad_bytes([(MARKER, b""), ("TEXTMAP", textmap.encode("ascii")), ("ZNODES", znodes),
                      ("ENCORE", ENCORE_REMAP), ("ENDMAP", b"")])


def build(zdbsp):
    """Returns ({PK3 entry: bytes}, {model file: bytes}, ZDBSP's rewrite of the map it was given)."""
    geometry = build_geometry()
    lines = geometry.build()
    znodes, rewritten = run_zdbsp(zdbsp, emit_textmap(geometry, lines, zdbsp=True))
    entries = {
        "Lua/quiet.lua": (HERE / "quiet.lua").read_bytes(),
        "Lua/scenery.lua": (HERE / "scenery.lua").read_bytes(),
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
    parser.add_argument("--encore-from", metavar="PLAYPAL", type=Path,
                        help="recompute the Encore remap from a PLAYPAL file and fail if it differs from the embedded one")
    args = parser.parse_args()

    if args.encore_from:
        if encore_remap(args.encore_from.read_bytes()) != ENCORE_REMAP:
            parser.error(f"the red/blue swap of {args.encore_from} differs from ENCORE_REMAP")
        print(f"ENCORE_REMAP matches the red/blue swap of {args.encore_from}")
        return

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
