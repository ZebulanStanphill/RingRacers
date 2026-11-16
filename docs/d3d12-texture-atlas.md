# Small-texture atlas

VibeRant packs small, clamped RGBA8 uploads into shared 2048×2048 pages instead
of creating a resource for every translated sprite frame. On Xbox One X at
31af14d, resource creation cost roughly 1.3–1.8 ms per texture; cycling rainbow
translations created 300–1000 textures per five seconds. Sharing pages removes
those resource creations after pages have enough reusable space.

`gr_textureatlas` defaults to On, is saved, and flushes cached textures when
changed. Set it Off for comparison. OpenGL is unaffected.

Uploads qualify only without mipmaps, wrapping, dimensions above 256, or linear
anisotropic filtering. Any loaded custom shader source disables atlas admission,
and loading sources flushes existing textures because custom shaders sample raw
UVs. Screens, palette tables, R8 textures, and other driver-owned resources retain
the standalone path. Brightmaps qualify independently. If a polygon forces wrap
on an atlas view, the driver first copies its pixels to standalone storage.
Allocation failure also falls back to standalone storage.

Each page has 16-pixel shelf height classes and four-pixel width granularity.
Freed intervals coalesce after the pending GPU fence completes. Once all its
rectangles retire, a page discards its shelves, allowing different height classes.
A page whose rectangles have all retired is released, except one kept for the
next upload: a long level can fill dozens of pages with sprite translations,
which the level change then empties. Views name their page by index, so a
released page's slot is filled again before the list grows.
A same-size eligible reupload keeps its rectangle. Uploads use the staging ring,
region copies, and page-wide transitions; page resource state is tracked centrally.

Built-in shaders transform each texture unit's UV independently, then clamp to
the rectangle's texel centers. This preserves point and bilinear CLAMP behavior
without gutters. A texture of its own gets zero constants, which the shaders
read as no transform; that is also what fullscreen passes see, since they bind a
zeroed b0 while sampling screen textures. Screen-only integer Load operations stay unchanged. The appended C++ constants
and HLSL float4 arrays match, retaining the old constants' offsets and fitting the
existing 512-byte draw allocation. Vulkan already honors region copy offsets.

The `D3D12: atlas:` perf line reports live views, retained pages, occupied shelf
area (including fence-pending frees), period placements, frees, and released
pages, and standalone admission reasons. `textures: ... created` still counts actual GPU resources,
including pages, rather than atlas views. Page memory is counted once. Only the
UWP build logs these lines.

`d3d12_texture_atlas` tests allocation, rounding, full pages, deferred reuse,
coalescing, and empty-page reset under the native harness's sanitizers. GPU frame
comparisons and Vulkan validation exercise the shared driver, and the Tests
workflow runs the driver test on native Direct3D 12 (WARP).

## Local validation (2026-11-15)

Linux RelWithDebInfo, VibeRant Vulkan on AMD Radeon Vega 10 (RADV), built with
Unix Makefiles and seven jobs. The native Clang ASan/UBSan suite passed all 93
tests, including the atlas allocator and the adjusted custom-shader/wipe harnesses.

Paired staff-ghost captures used atlas On and Off, `+fpscap 300`, and deterministic
frame capture. The 20-map Nearest sweep used tics 70, 140, 350, 525, and 700:

| Check | Paired frames | Identical frames | Differing pixels | Worst frame |
| --- | ---: | ---: | ---: | ---: |
| Nearest, 20 maps, 640×400 | 100 | 86 | 19 / 25,600,000 | 5 pixels (0.001953%) |
| Nearest, Aerial Highlands, 1280×720 | 5 | 3 | 2 / 4,608,000 | 1 pixel (0.000109%) |
| Bilinear, three maps, 640×400 | 9 | 0 | 58 / 2,304,000 | 16 pixels (0.00625%) |

The six validated runs (Aerial Highlands, Dragonspire Sewer 1, and Final Fall,
On and Off) had zero Vulkan validation messages. The additional 1280×720 pair
also had zero messages. All 40 runs of the main sweep exited normally.

Aerial Highlands logged 604 atlas textures in three pages at 89.0% occupancy.
One warm five-second period placed 234 views with 34 resource creations; another
placed 42 with eight creations. The Off run instead logged periods with 337 and
64 resource creations. These are wall-clock windows, not synchronized benchmark
intervals, and are evidence of resource sharing rather than an Xbox FPS claim.

**Precision limitation:** the normalized-page transform has rare rounding
changes. Nearest differences are isolated texel selections. Bilinear also has
small color-rounding differences (mostly one channel level, up to 25 levels
in these captures), so it does not strictly meet a point-sampling-only exception
to pixel identity. This implementation follows the normalized transform design;
bit-identical bilinear results would require further sampling work.

## Xbox result (2026-11-15)

About two hours of online races on an Xbox One X at b04dbef (33 maps, 1,126
five-second windows, 3D models on for the first half) against an earlier online
session at 31af14d (7 maps, 232 windows):

| Per five-second race window | 31af14d | b04dbef |
| --- | --- | --- |
| Resources created (mean) | 151 | 6 |
| Median frames per second | 46.8 | 55.1 |
| Windows below 50 frames per second | 56% | 37% |
| Windows below 40 frames per second | 40% | 20% |

The sessions differ in maps and player counts, so the frame rates are not a
controlled comparison; the resource creations are the direct measure. A window
placed 97 textures in pages at the median and 2,347 at most. The textures left
standalone were wrapped ones (8,593 over the session) and ones over the size
limit (1,561). The player reported no visual problems.

A level kept every translation it had drawn until the level change, up to
9,826 textures in 35 pages at 97% occupancy, and b04dbef never released a
page: the session ended with 35 pages (560 MB) holding about 130 textures.
Empty pages are released since the following commit, which this session did
not test.
