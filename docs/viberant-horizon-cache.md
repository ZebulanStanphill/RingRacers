# VibeRant horizon cache

VibeRant lazily caches one byte per subsector from its static BSP segs.
Only `!seg->glseg && seg->linedef && seg->linedef->special == HORIZONSPECIAL`
sets a byte. Plane and sky-claim paths skip the horizon scan when the byte
is clear. Marked subsectors retain the original per-seg visibility decision.
Xbox profiling attributed 4.6% of 3D-view samples to that decision.
The render-only cache changes no simulation fields.

Use a zone owner pointer with the `PU_LEVEL` allocation so teardown clears
the pointer.
`HWR_LoadLevel` invalidates it too, so equal-sized replacement maps rebuild.
Runtime writers call `HWR_LineSpecialChanged(before, after)`, which marks the
cache dirty only when either value is HORIZONSPECIAL. The next query rebuilds
the array. A rebuild scans every subsector, so one-shot triggers that clear
their own (never horizon) specials mid-race must not cause one.
The hooks are no-ops unless both HWRENDER and HAVE_VIBERANT are set.

## Invalidation coverage

ACS line-special setters, savegame/netgame restoration, and runtime line-special
activation notify the cache through `HWR_LineSpecialChanged`. Map construction
and conversion precede `HWR_LoadLevel` and use its level invalidation instead.
One-shot triggers and slope setup that only clear known non-horizon specials do
not invalidate the cache. Lua's line-special property is read-only. Sector and object
special setters do not change linedefs.

## Validation

`horizon_cache` extracts the production cache, invalidation, and both call-site
gates. ASan/UBSan cases cover ordinary, horizon, GL, and null-linedef segs,
cache reuse, changing a special in both directions, owner clearing on level
teardown, and equal/different-sized replacement levels. `horizon_visibility`
checks the per-seg horizon visibility decision against Software.
GPU output and the performance improvement require Windows/Xbox validation.
