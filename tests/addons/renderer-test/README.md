# Renderer test map

[D_RenderTest_v1.pk3](D_RenderTest_v1.pk3) is an add-on for Ring Racers
2.4 with one hidden race map, **Render Test**, that puts the fork's renderer
features side by side: fog, overlapping water, heat haze, polyobject ripples,
wall light slices, translucent sorting, brightmaps, a skybox with rain and a
sprite-textured 3D model. It ships every texture, flat and sprite it uses, so it
needs no other assets. The map has been loaded on Xbox, but its individual
rooms and effects still need gameplay validation. Offline generation checks
(see the end) do not establish that every test looks or behaves correctly.

## Install and open

1. Copy **the PK3 itself**, without extracting it, to
   `E:\ringracers\addons\D_RenderTest_v1.pk3`.
2. Optional, for the model test (the *models* folder is in this download beside
   the PK3): the game reads 3D models only from its home folder, never from a
   PK3. That is the folder with `ringconfig.cfg`, `E:\ringracers\ringracers\`.
   Copy `models\models.dat` there (if you already have a `models.dat`, add its
   two lines to yours) and `models\rendertest.md3` to
   `E:\ringracers\ringracers\models\`. The game reads `models.dat` at launch and
   again when an add-on loads, so adding the file before loading the PK3 is
   enough. Models need a hardware renderer and **Models** On.
3. Launch the game, select **VibeRant D3D** (or **Software**, to compare), and
   load the file through the **Addons** menu. With a keyboard, the alternative
   console command is `addfile "E:\ringracers\addons\D_RenderTest_v1.pk3"`.
4. Open the console and run `cheats on`, then `map RR_RENDERTEST`, or
   `map RR_RENDERTEST -e` for Encore. For the split-screen
   variants, start the game with two local players first. The map is a one-lap
   race with no music.

The add-on removes AI racers when this map loads and during play, so you can
test without bots driving around or using items. Human players, including all
local split-screen players, stay in the map. Your bot settings and other maps
are unaffected; restarting this map also keeps it free of bots.

The hall runs west to east. You start at its west end, facing east, in light
rain. There are three doorways on the left (north) side, west to east: **Fog**,
**Water**, **Polyobjects**, and three on the right (south): **Light slices**,
**Transparency**, **Brightmaps**. Item boxes sit near the start and far down
the hall; they stay hidden until you cross the checkpoint just past the start
line (see Hall).

## What to look at

**Hall: skybox visibility and models** *(CHANGES: Skybox visibility;
Sprite-textured models, 3D models and mipmaps).* The hall's sky ceiling lets
**RAIN** fall around you. Above the walls, the sky shows the skybox, a
separate enclosed room far west of the hall that nothing connects to. It is lit
at full brightness, with magenta tiled walls, a teal checkered floor and four
orange glowing orbs, and has a sky ceiling of its own, so it rains inside it
too. The skybox camera hangs in the middle of that room at about kart-camera
height. It has no centre point, so it stays there and only turns as you turn.
Pass: the sky above the hall shows that bright room with no rain in it, while
rain still falls around your kart (Legacy GL and VibeRant D3D exclude
precipitation from skybox views, and Software does too). Fail: rain streaks in
front of the room's walls in the sky picture. If the sky instead shows the
hall, or nothing but sky, the skybox is not being drawn, and the check
tells you nothing.

The six item boxes stay hidden until you cross the checkpoint 100 units past
the start line, a line across the whole hall with a post at each end. The engine
hides them until a racer has crossed a first checkpoint, and the finish line
only counts as one from the second lap, which this one-lap map never reaches.
Two of the boxes are just west of the checkpoint and four are east of it, so most
appear ahead of you. Crossing the checkpoint again on the way back changes
nothing, and finishing the lap does not need it. The boxes turn into a plain
textured box when `models.dat` is installed: a 3D model that borrows its sprite
for a texture, so it also checks the sprite-patch preparation and the mipmaps
at near and far range. Without the files they stay the usual sprites. Toggle
**Models**.

**Fog** *(Fog).* Two fog blocks, red then blue, each cover two thirds of the
room and overlap in the middle third, 0 to 128 units high, with pillars to see
through them. Look at it from the doorway, then walk in and through all three
parts, with **Shaders** On and Off. Software is the reference; the Shaders Off
path is the older approximation.

**Water** *(Water (overlapping volumes); Underwater and heat distortion).* The
chamber is 768 units wide and 4,096 units long, with a 1,536-unit-long flat pool
between two ramps. Drive north through the doorway and down the entrance ramp:
the floor descends from 0 to -512 below the main water surface at 96, leaving
608 units of water above the flat bottom so the chase camera can submerge.
Continue along the bottom, then up the far ramp to the dry ledge at 128, which
has **heat haze** when your camera is on it. The ramps connect continuously to
the hall and ledge; you can drive back along the same route.

The main translucent rippled water spans both ramps and the pool. A smaller
volume of a different tint overlaps it in the east corner, with its surface at
48 over the same flat floor at -512. Three raised blocks rise out of the pool, and a
translucent wall crosses its deep section. Check that the refraction shows
submerged geometry, that no extra boundary shows where one volume lies inside
the other, and the underwater distortion once your camera is in the water.
**Reduce Effects** On turns both distortions and the refraction off.

**Polyobjects** *(Polyobject ripples).* Two 128 by 128 blocks. The translucent
one (70% opaque) has ripple flags on its top and bottom, and the opaque one has
none. Driving across the doorway starts the translucent one spinning, so look at
it from above: its top face should ripple, and the opaque block's should not.
**Reduce Effects** On stops the ripple. The ripple flags are on the sector behind
the polyobject's first line, which is where the engine reads them.

**Light slices** *(Wall light slices).* A 512-unit room, taller than the others,
with three invisible light blocks (white, orange and blue, at 64 to 96, 192 to
224 and 320 to 352) that cut the tall walls into lit and dim bands, and a
translucent platform at 128 to 160 with its own light level. Look for clean
bands with no inverted or missing slices, and for sprites and the platform
taking the bands. Split-screen is the case that remains untested.

**Transparency** *(Better transparency ordering; **not** Fill transparency).*
Three stacked translucent solids on a platform (translucent, additive,
subtractive), two translucent walls across the room, and six translucent
sprites in front of, behind and between them, plus two item boxes (model
included). Compare `gr_bettertransparency` Off and On from several angles: On
should order sprites, models and planes correctly where Off shows them popping
in front of or behind the wrong plane. Fill transparency (HUD fills) is not
covered here.

**Brightmaps** *(Brightmaps).* A dim blue room (light level 32) whose walls are
all one sign texture with a bright glyph band, and four dark orbs with a glowing
core and ring. The glyphs, core and ring should stay full-bright, untinted by
the room's blue, and the rest of each surface should be dark. Compare **Palette
Rendering** On and Off, and Encore, where the texel remap should persist on the
brightmapped pixels.

**Encore** *(Encore distortion; Encore inversion flag).* With `-e`, repeat the
Water room's underwater and heat views: the mirroring should combine with the
distortion instead of replacing it. Wherever Encore inverts the screen, such as
the start of the map, the scene should invert instead of a white quad covering
it.

## Settings to compare

| Setting | Where |
| --- | --- |
| Renderer | Options, Video, Advanced: **Software** or **VibeRant D3D** |
| Shaders | Options, Video, Advanced (`gr_shaders`): On, Off, Ignore custom shaders |
| Reduce Effects | Profile Setup, your profile, Accessibility (`reducevfx`; the Guest profile can't be edited, so use the console there) |
| Better transparency ordering | `gr_bettertransparency` |
| Palette Rendering | `gr_paletterendering` |
| Models | `gr_models` |

## Build and validation

`build.py` generates the UDMF map, every
texture, flat and sprite as PNG, the SOC level header, the `BRIGHT` and
`SPRTINFO` lumps, and the model files, and packages the map-local bot removal
script from [`quiet.lua`](quiet.lua). Only the map's BSP, the `ZNODES` lump,
comes from a tool, [ZDBSP](https://github.com/ZDoom/zdbsp) 1.19 (commit
`5d4fb9c`; it builds with `cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5`):

```sh
python3 tests/addons/renderer-test/build.py --zdbsp path/to/zdbsp
python3 tests/addons/renderer-test/build.py --check --zdbsp path/to/zdbsp
python3 tests/addons/renderer-test/build.py --validate
```

`--check` rebuilds, compares every PK3 entry and model file byte for byte, and
validates the committed files. The one exception is the map's `ZNODES`, which
node builds on different machines may order differently, so it is validated
instead. `--validate` (also run by the others, and needing no ZDBSP) re-parses
the TEXTMAP and checks what the engine requires: the namespace and version
first, valid vertex, line and sidedef references, closed sector boundaries with
no lines crossing or touching mid-line, 3D-floor specials aimed at sectors that
exist, polyobject loops with their anchor and spawn things, waypoints that link,
every texture shipped, each checkpoint's two posts (same ID and angle, aimed at a
line tag that exists), the skybox viewpoint inside a sector with a sky ceiling,
and in the nodes the original vertex count, seg line numbers, one seg per
polyobject line, and each polyobject's spawn spot inside one subsector. The CI's
`regressions` job runs `--check` and uploads the result as `renderer-test-map`.

How the map is built for the engine:

- The nodes are `XGLN` (ZDBSP's `-X` without `-z`), the format the engine reads
  for UDMF maps; `ZGLN` would be rejected.
- ZDBSP only avoids splitting polyobjects in ZDoom namespaces, with its own
  thing numbers (9300 and 9301, numbered by the thing's angle), start line
  special and one-sided lines. So it is given a copy of the map with those
  changes, numbered identically (`build.py` checks that ZDBSP kept the numbering),
  and the shipped TEXTMAP has the engine's own polyobject things (760 and 761),
  start line (special 20) and two-sided lines, which it needs to find the
  polyobject's planes.
- The light and fog colors are decimal numbers in the TEXTMAP, as the engine
  reads them with `atol`.
- The brightmap for the glowing orb is in `Graphics/`, not `Sprites/`, because
  every lump in `Sprites/` is read as a frame of the sprite named by its first
  four letters.
- The PNGs are stored uncompressed (the zlib output of different builds
  differs), which is why the textures, small as they are, take up most of the
  file.

The model is a box, `rendertest.md3`, with both triangle windings so that it is
solid however the renderer culls, and `models.dat` assigns it to the item box's
two sprites (`RBOX`, the ring box, and `RNDM`, the random item) at scale 2.0.
Raise or lower that scale if the box looks too small or large.

The offline checks do not verify the effects in gameplay. In particular the
SOC, `BRIGHT` and `SPRTINFO` text, the polyobject rotation trigger (crossing the
doorway line), the finish line, the checkpoint and the waypoint loop still need
gameplay validation; their definitions were checked against the engine source.
