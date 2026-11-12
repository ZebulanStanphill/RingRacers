# Renderer test map

[D_RenderTest_v1.pk3](D_RenderTest_v1.pk3) is an add-on for Ring Racers
2.4 with one hidden race map, **Render Test**, that puts the fork's renderer
features side by side: fog, overlapping water, heat haze, polyobject ripples,
wall light slices, translucent sorting, brightmaps, a skybox with rain and a
sprite-textured 3D model. It ships every texture, flat and sprite it uses, so it
needs no other assets. The map has been loaded on Xbox, but its
original rooms and effects still need gameplay validation. The parity wing
was also checked with the local macOS offscreen harness (see below). Offline
generation checks (see the end) do not establish that every test looks or
behaves correctly.

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
   enough. Models need a hardware renderer and **Models** On. While
   `models.dat` is installed it applies to every map and item box, not only
   this map: with Models On, all item boxes become the plain textured box, even
   without the add-on loaded. Delete `models.dat` (or its two lines) after
   testing. If the boxes stay sprites, the log says why: "Error while loading
   models.dat: ..." when the game can't open the file, or "Unknown
   sprite/player skin ... detected in models.dat" for a name it doesn't know.
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
**Transparency**, **Brightmaps**. A doorway in the middle of the east wall, at
the far end, leads to the **Sky pit** (see Hall). Item boxes sit near the start
and far down the hall; they stay hidden until you cross the checkpoint just past
the start line (see Hall).

A new **parity wing** leaves the south wall of Brightmaps (between x=576 and
1088). Follow its corridor east, y=-1920. Numbered coloured wall panels identify
rooms 1 through 7; 0 marks the entrance/corridor. Every room returns to this
corridor, and the hall's finish, checkpoint and waypoint loop are unchanged.

## What to look at

**Hall: skybox visibility and models** *(CHANGES: Skybox visibility;
Sprite-textured models, 3D models and mipmaps).* The hall's sky ceiling lets
**RAIN** fall around you; that rain is expected, and says nothing about the
skybox. The sky shows the skybox, a separate enclosed room far west of the hall
that nothing connects to. It is lit at full brightness, with magenta tiled
walls, a teal checkered floor and four orange glowing orbs, and has a sky
ceiling of its own, so it rains inside it. The skybox camera hangs in the
middle of that room at about kart-camera height. It has no centre point, so it
stays there and only turns as you turn.

The hall's rain falls between you and the sky, so the sky above the hall cannot
show whether the skybox picture itself contains rain. The **sky pit** can. Drive
to the **east end** of the hall (straight ahead from the start, about 2400
units) and through the doorway in the middle of the east wall. It leads into a
covered area with an ordinary ceiling, so no rain falls there. At its far side,
a blocked edge (karts cannot cross it, and it is a 32-unit drop) borders the
pit, a wide shallow sector (1408 units long, 896 wide) whose **floor is sky**
and whose ceiling is not, so no rain falls in it either. Where its floor would
be, you see the skybox. Face east from the edge and look down the pit; from the
hall's east end you can also see it through the doorway. Because nothing in the
pit or the covered area can rain, every raindrop in the sky picture is a
skybox-rendering failure.

Pass: the pit shows the skybox room (the teal checkered floor, with the magenta
walls and orange orbs toward the far end of the pit) with no rain streaks in it,
while rain still falls around your kart in the hall (Legacy GL and VibeRant D3D
exclude precipitation from skybox views, and Software does too). Fail: rain
streaks in front of the room's floor or walls in the pit. If the pit instead
shows the hall, or nothing but a plain sky texture, the skybox is not being
drawn, and the check tells you nothing.

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
brightmapped pixels: in Encore the sign's yellow glyphs turn cyan and its cyan
band edges yellow, and the orbs' glow changes color too, all still full-bright.

**1: Fog-style line** *(CHANGES: Fog-style lines).* First south opening in the new
wing, x=1408..2432, y=-2048..-3072. Face south from (1920, -2180), camera z=80.
The west/right opening is `renderstyle = "fog"`; the east/left is an ordinary
50% translucent copy of the same 256-high diagonal window texture. Both look
onto bright fine checks 512 units behind the line. The front sector is light
160 with a mild red/orange colormap. VibeRant should tint everything behind the
fog rectangle red without drawing its diagonal texture; the comparison window
should show that texture over the checks. Software is **not a reference for
this line style**: at this pose its fog opening shows untreated checks, while
the ordinary window shows the red-tinted diagonal texture. `R_CheckBlendMode`
in `r_segs.cpp` assigns world floor/ceiling heights to screen clipping bounds;
its inconsistent fog result changes with the view. This was rendered, not
inferred from the desired hardware result.

**2: Fullbright sprite fading** *(CHANGES: `colormapfadesprites`).* Two
long blue rooms, west x=2560..3200 and east x=3328..3968, y=-2048..-5632. The
west room sets `colormapfadesprites`; the east does not. Both have seven tall
fullbright striped posts and a light block between z=80 and 96. Look south
along a row from (2640, -2160, 120), or (3408, -2160, 120) in the east room,
keeping off the centre so the nearest post does not hide the others.
Software darkens the **lower pieces** of the west posts with distance below
the light-list boundary; all pieces in the east stay bright. The upper west
pieces also stay bright: upstream Software only tests this flag in
`R_SplitSprite`, and `R_DrawVisSprite` excludes fullbright sprites from the
extra-colormap tint. Thus the lower pieces darken rather than becoming blue
like the walls; expecting the entire sprite to fade blue would be wrong.
VibeRant should reproduce this split-piece reference. On the pre-sprite-fix
hardware build both rows stayed fully bright, even below the boundary.

**3: Shadow-drawn sprites** *(CHANGES: `RF_SHADOWDRAW` sprites).* Yellow panels, x=4096..5120,
y=-2048..-3072. The near pair is on flat ground; the far pair is on a ramp
rising from 0 to 128. In each pair the west/right striped post has
`RF_SHADOWDRAW`, the east/left post does not. Look south from
(4608, -1940, 220), pitch -10, or drive in and look down. Software draws the
flagged posts as flat little ground shapes, as wide as the upright posts and
as tall as the object's 48-unit radius and the camera's height make them,
beside the tall 192-unit reference posts. The ramp version
also takes the slope. Lua calls `P_TryMove` at the same position after settling
the object on its floor: `p_map.c` assigns `standingslope` on that successful
grounded move, even for static scenery. VibeRant should match; the supplied
hardware build leaves all four upright.

**4: Translucent overlay sprites** *(CHANGES: Translucent overlay sprites).*
Purple 4 panels, x=5248..6272, y=-2048..-3072. Face south from
(5760, -2320, 112). From west to east (right to left on screen): frame flags
`FF_OVERLAY|FF_TRANS50`, render flags `RF_OVERLAY|RF_TRANS50`, plain
`FF_TRANS50`, and opaque `FF_OVERLAY`. Software shows the fine checked wall
through the first three equally, while the last hides it. VibeRant should do
the same. On the supplied pre-sprite-fix hardware build the two translucent
overlay posts almost disappear into the checks; the plain translucent and
opaque posts remain clearly visible.

**5: Level darkening** *(CHANGES: Level darkening).* Teal 5 panels, x=6400..7552,
y=-2048..-3072. Drive onto the numbered floor pad, x=6848..7104,
y=-2304..-2112, and face south toward the three checked bays: west/right light
255, middle 160, east/left 96, each with a fullbright striped post. The pad
keeps extending darkness while occupied. A fresh trigger has an eight-second
plateau between eight-tic fades; after leaving, it clears within 296 tics
(about 8.46 seconds). It affects only the player driving onto it, including in
split-screen. The map-local console command **`rendertest_darkness`** triggers
the same effect for its issuing player anywhere in Render Test. Software dims
walls and floors, with brighter bays changing more; the darkest bay may already
be black at this distance. The fullbright posts stay unchanged. VibeRant should
match this lighting response, and did on the supplied walls-fix build. The
natural race effect is subtle (maximum 1/7, not total black); `P_GetDarkness`
and the eight-tic `DARKNESS_FADE_TIME` define this in `p_tick.c`/`doomstat.h`.
Useful camera: (6976, -2240, 100), facing south.

**6: Midtexture between two skies** *(CHANGES: Wall textures between two skies).* Orange 6 panels,
x=7680..8704, y=-2048..-3072. Face south from (8192, -2400, 100).
The tall green tree-line is a masked 512-high texture on a two-sided `midpeg`
line. Behind it the sky sector's floor equals its ceiling at -2; in front the
floor is 0 and the sky ceiling 382, like Municipal Meadow's MMSBTR edge. The
opaque tips reach z=352, below the front sky ceiling; the edge blocks karts.
Software and fixed VibeRant should show the complete jagged
silhouette against the sky above the zero-height back sector; pre-fix VibeRant
showed nothing. This map uses its existing skybox, so magenta tiles behind the
green silhouette are expected instead of Municipal Meadow's sky artwork.

**7: View-width ruler** *(CHANGES: Two-player view width).* At the east
end of the corridor, take the passage south (x=9216..9472) to the large ruler
room, x=4352..9472, y=-5888..-8448. Centre the camera above the numbered 7 floor
spot at **(6912, -7424)** and face exactly south (yaw 270, pitch 0), with
`fov 100`, stationary and without a speed FOV effect. The wall is **1024 units**
away; its **40 bands are 128 units wide**, numbered 0..39 from west to east.
The spot faces the boundary between bands 19 and 20. Compare the edge bands
in one-player and two-player split-screen, using the same camera position.

`R_ExecuteSetViewSize` in `r_main.cpp` sets `fovtan = tan(FOV/2)` (with no view
morph zoom), then multiplies it by 17/10 for two players. Half-width at the wall
is therefore `1024 * tan(50 degrees) = 1220.36` units for one player and
`1220.36 * 1.7 = 2074.61` for two. One-player edge x coordinates are 5691.64
and 8132.36: bands **10 (right edge)** and **29 (left edge)**. Two-player edge
coordinates are 4837.39 and 8986.61: bands **3 (right)** and **36 (left)**.
Software's fixed-point angle table introduces a small rounding difference,
well within those bands. VibeRant should show the same edge bands in each mode;
the supplied build predates the split-screen view fix and shows bands 35/4
in two-player mode (29/10 in one-player), against Software's 36/3. Camera
z=100 works.

**Encore** *(Encore distortion; Encore inversion flag).* With `-e`, repeat the
Water room's underwater and heat views: the mirroring should combine with the
distortion instead of replacing it. Wherever Encore inverts the screen, such as
the start of the map, the scene should invert instead of a white quad covering
it.

The map ships an `ENCORE` lump, a palette remap that swaps red and blue, because
without one Encore would only mirror the screen. In Encore, reds turn blue and
blues red across the map, while white and grays stay put. Sector color tints
don't change, by design and in both renderers: the game remaps a UDMF sector's
textures but not its `lightcolor` and `fadecolor`. So a pool looks recolored from
outside but unchanged from inside, and the fog rooms keep their red and blue.

## Settings to compare

| Setting | Where |
| --- | --- |
| Renderer | Options, Video, Advanced: **Software** or **VibeRant D3D** |
| Shaders | Options, Video, Advanced (`gr_shaders`): On, Off, Ignore custom shaders |
| Reduce Effects | Profile Setup, your profile, Accessibility (`reducevfx`; the Guest profile can't be edited, so use the console there) |
| Better transparency ordering | `gr_bettertransparency` |
| Palette Rendering | `gr_paletterendering` |
| Models | `gr_models` |

To switch renderer quickly, bind a key in the console:
`bind f5 "add renderer 1"` toggles between Software and VibeRant D3D (confirmed
on Xbox). Binds aren't saved in the config, so to keep it, put that line in
`ringexec.cfg` in the home folder (`E:\ringracers\ringracers\ringexec.cfg`),
which the game runs at every launch. Binds don't fire while a menu or the
console is open.

## Build and validation

`build.py` generates the UDMF map, every
texture, flat and sprite as PNG (except the sign's brightmap, a Doom patch; see
below), the SOC level header, the `BRIGHT` and
`SPRTINFO` lumps, and the model files, and packages the map-local scripts
[`quiet.lua`](quiet.lua) (bot removal) and [`scenery.lua`](scenery.lua) (see
below). Only the map's BSP, the `ZNODES` lump,
comes from a tool, [ZDBSP](https://github.com/ZDoom/zdbsp) 1.19 (commit
`5d4fb9c`; it builds with `cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5`):

```sh
python3 tests/addons/renderer-test/build.py --zdbsp path/to/zdbsp
python3 tests/addons/renderer-test/build.py --check --zdbsp path/to/zdbsp
python3 tests/addons/renderer-test/build.py --validate
python3 tests/addons/renderer-test/build.py --encore-from path/to/PLAYPAL
```

`--check` rebuilds, compares every PK3 entry and model file byte for byte, and
validates the committed files. The one exception is the map's `ZNODES`, which
node builds on different machines may order differently, so it is validated
instead. `--validate` (also run by the others, and needing no ZDBSP) re-parses
the TEXTMAP and checks what the engine requires: the namespace and version
first, valid vertex, line and sidedef references, closed sector boundaries with
no lines crossing or touching mid-line, 3D-floor specials aimed at sectors that
exist, polyobject loops with their anchor and spawn things, waypoints that link,
every texture shipped, each checkpoint's two posts (same ID and angle, aimed at
a line tag that exists), the skybox viewpoint inside a sector with a sky
ceiling, the sky pit's sky floors under non-sky ceilings with every line into
them blocking karts, and in the nodes the original vertex count, seg line
numbers, one seg per polyobject line, and each polyobject's spawn spot inside
one subsector. The CI's `regressions` job runs `--check` and uploads the result
as `renderer-test-map`.

The parity checks additionally require the fog style/midtexture and front
colormap, matching fade-room/control colormaps with sprite-cutting light
blocks, all scenery variants, the darkness pad's tag matching Lua, the
zero-height two-sky sector and its blocked `midpeg` edge, all forty ruler bands
and their width, and every generated texture. Zero-height sky sectors are
checked separately from the original deep sky pit rules.

`--validate` also checks that the map's `ENCORE` lump is the 256-byte red/blue
swap remap, between `ZNODES` and `ENDMAP`. That remap is embedded in `build.py`
as a constant, because the PLAYPAL it derives from is game data this repository
doesn't ship. `--encore-from` recomputes it from a v2.4 PLAYPAL file (the first
768 bytes of `bios.pk3`'s `PLAYPAL`; each entry is the palette color nearest to
the color with red and blue swapped, lowest index on ties) and fails if the
result differs from the constant.

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
- The sign's texture brightmap, `Textures/RTSIGNBM.lmp`, is a Doom-format
  patch, not a PNG. Software reads a texture brightmap raw, without the PNG
  conversion textures get, so a PNG brightmap silently fails there (the
  console reports bad column offsets and nothing is bright). Its bright pixels
  are palette index 0, white, and the rest index 31, black; Software tests for
  index 0, and Legacy GL and VibeRant D3D for a fully red color, which index 0
  also is.
- The orb's sprite brightmap must be transparent exactly where the sprite is,
  as Software draws the two patches' posts together, so a brightmap that is
  opaque where the sprite is transparent shifts out of alignment with it.
  `--validate` checks both this and the sign brightmap's format.
- The scenery objects, the translucent sprite (`MT_RTTRANS`, editor number
  3900), the glowing orb (`MT_RTGLOW`, 3901), and the tall parity post
  (`MT_RTPARITY`, 3902), may share their numbers with
  objects in the game's own data. The engine gives a map thing the first object
  type with its number, so a colliding game object wins: on Xbox the Brightmaps
  room showed large dark trees where the orbs belong. The numbers can't be
  checked against the game's assets, so `scenery.lua` handles the collision.
  When Render Test loads, after the engine has spawned the map's things, it
  finds each thing numbered 3900, 3901 or 3902 whose object isn't the add-on's,
  removes that object, if any, and spawns the add-on's object at the thing's
  position (the floor under it plus its height), angle and scale. If it replaced
  anything, it prints one console line naming each number, the object that had
  taken it and how many things it replaced, for example `Render Test: the game
  uses the add-on's editor numbers; spawned the add-on's own objects instead:
  3901 was MT_NAME (mobj type 123), 24 things`. No line means no collision.
  `build.py --validate` checks that the script's numbers match the ones in the
  SOC and the map.
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

The seven parity rooms were rendered in Software and in the UWP hardware paths
using the local invisible-window macOS harness, with the `sw-parity-walls`
binary (walls fixes present, sprite/view fixes absent). The observations above
refer to that build. The pad/command were also exercised with a temporary
harness-only Lua probe; this add-on ships no probe or forced camera logic.
The harness loads the map with `-warp RR_RENDERTEST -file
/path/to/D_RenderTest_v1.pk3`; its first argument is `RR_RENDERTEST`, not MAP01.
For the ruler, append `-splitscreen 2` to start two local players. Harness
lighting comparisons can use `RR_DARK=0` and `RR_DARK=0.142857` at identical
poses; normal driving and `rendertest_darkness` use the engine's own timers.
These captures do not replace driving the new route/checkpoint on Xbox.
