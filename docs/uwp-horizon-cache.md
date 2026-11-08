# UWP horizon cache

D3D12 caches one byte per subsector, built lazily from its static BSP segs.
Only `!seg->glseg && seg->linedef && seg->linedef->special == HORIZONSPECIAL`
sets a byte. Both plane and sky-claim paths skip their whole horizon scan when
it is clear; marked subsectors retain the original per-seg visibility decision.
Xbox profiling attributed 4.6% of 3D-view samples to that decision.
The cache is render-only. No simulation fields are changed.

`PU_LEVEL` allocation with a zone owner pointer clears the pointer on teardown.
`HWR_LoadLevel` invalidates it too, so equal-sized replacement maps rebuild.
Runtime writers call `HWR_LineSpecialChanged(before, after)`, which marks the
cache dirty only when either value is HORIZONSPECIAL; the next query rebuilds
the array. A rebuild scans every subsector, so one-shot triggers that clear
their own (never horizon) specials mid-race must not cause one.
The hooks are no-ops unless both HWRENDER and HAVE_D3D12 are set.

## Special writer audit

Locations below refer to base d083abd3b, before insertion of hooks.
A whole-src search for `->special` / `.special` writes, increment/decrement,
and property macros found these line-special writers:

- `acs/call-funcs.cpp:847`: `CallFunc_ClearLineSpecial`; hooked.
- `acs/call-funcs.cpp:1211`: `CallFunc_SetLineSpecial`; hooked.
- `acs/call-funcs.cpp:2711`: `CallFunc_SetLineProperty`, LINE_PROP_ACTION;
  the macro invocation became an explicit, hooked case. The matching
  invocation at 2611 is GetLineProperty and only reads.
- `p_saveg.cpp:2848,7204`: world delta unarchive and original-line restoration
  (savegame and netgame paths); hooked.
- `p_spec.c:2289`: P_LineSpecialWasActivated; hooked.
- `p_spec.c:8003`: P_SpawnSpecialsThatRequireObjects disabled-line clear;
  hooked.
- `p_spec.c:1649,1655,1668` (one-shot triggers 313/321/399, 323-329,
  300-337), `p_spec.c:7739,7752` (executors 308/309) and `p_slopes.c:815`
  (slope linedefs): each clears a special known not to be HORIZONSPECIAL to 0,
  so the cache can't change; not hooked.
- `p_setup.cpp`: binary/UDMF initialization, default initialization and legacy
  linedef conversion all precede HWR_LoadLevel; covered by level invalidation
  and the zone owner reset. Export edits target a separate wlines copy and
  do not change live line specials. Exact write sites follow below.

Lua `META_LINE` installs only __index and __len, no __newindex: line.special
is not writable. Lua sector.special and mobj.special setters are unrelated.
Side, sector, mapthing and mobj special writes elsewhere do not change line_t.
Spawn-line snapshot copies only read live lines; save restoration writes are
listed above. There are no other line-special property setters in src/acs.

p_setup.cpp write sites (base line numbers):

```text
1180: ld->special = SHORT(mld->special);
1912: lines[i].special = atol(val);
2610: wlines[i].special = 0;
3271: ld->special = 0;
4840: lines[i].special = 0;
4865: lines[i].special = 0;
4898: lines[i].special = 0;
5018: lines[i].special = 53;
5029: lines[i].special = 56;
5038: lines[i].special = 60;
5054: lines[i].special = 61;
5075: lines[i].special = 66;
5087: lines[i].special = 0;
5129: lines[i].special = 100;
5171: lines[i].special = 120;
5219: lines[i].special = 100;
5236: lines[i].special = 150;
5287: lines[i].special = 170;
5336: lines[i].special = 190;
5343: lines[i].special = 200;
5376: lines[i].special = 220;
5444: lines[i].special = 254;
5498: lines[i].special = 300;
5511: lines[i].special = 303;
5523: lines[i].special = 305;
5539: lines[i].special = 309;
5554: lines[i].special = 314;
5560: lines[i].special = 317;
5566: lines[i].special = 319;
5582: lines[i].special = 321;
5614: lines[i].special--;
5667: lines[i].special = 331;
5679: lines[i].special = 334;
5692: lines[i].special = 337;
5710: lines[i].special = 340;
5718: lines[i].special = 400;
5733: lines[i].special = 403;
5742: lines[i].special = 405;
5750: lines[i].special = 0;
5862: lines[i].special--;
5927: lines[i].special = 429;
6000: lines[i].special = 0;
6158: lines[i].special = 482;
6179: lines[i].special = 484;
6202: lines[i].special = 489;
6244: lines[i].special = 500;
6255: lines[i].special = 0;
6273: lines[i].special = 502;
6285: lines[i].special = 0;
6296: lines[i].special = 500;
6323: lines[i].special = 510;
6368: lines[i].special = 541;
6374: lines[i].special = 600;
6403: lines[i].special = 700;
6445: lines[i].special = 704;
6455: lines[i].special = 720;
6464: lines[i].special = 720;
6473: lines[i].special = 720;
6482: lines[i].special = 720;
6509: lines[i].special = LT_SLOPE_ANCHORS;
```

## Validation

`horizon_cache` extracts the production cache, invalidation and both call-site
gates. ASan/UBSan cases cover ordinary, horizon, GL and null-linedef segs,
cache reuse, changing a special in both directions, owner clearing on level
teardown, and equal/different-sized replacement levels. `horizon_visibility`
continues to check the per-seg horizon visibility decision against Software.
GPU output and the performance improvement require Windows/Xbox validation.

All changed translation units (`hw_main.c`, `p_spec.c`,
`p_saveg.cpp`, `acs/call-funcs.cpp`) were syntax-checked with Apple Clang,
using `-fsyntax-only -DHAVE_CURL -DNONX86 -DNORUSEASM`, `-std=gnu11` for C
and `-std=c++17` for C++. Includes: `src`, `src/acs/vm`, `thirdparty/glm`
and every `thirdparty/*/include`. Variants: `_UWP + HAVE_D3D12 + HWRENDER`,
desktop `HWRENDER`, and software-only (none of those three defines).
All passed; p_spec.c reports its existing deprecated A_Dye prototype warning.
These are host syntax checks, not a Windows/UWP compiler or linker check.
