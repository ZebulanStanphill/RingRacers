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

## Invalidation coverage

ACS line-special setters, savegame/netgame restoration and runtime line-special
activation notify the cache through `HWR_LineSpecialChanged`. Map construction
and conversion precede `HWR_LoadLevel` and use its level invalidation instead.
One-shot triggers and slope setup that only clear known non-horizon specials do
not invalidate the cache. Lua's line-special property is read-only; sector and
object special setters do not change linedefs.

## Validation

`horizon_cache` extracts the production cache, invalidation and both call-site
gates. ASan/UBSan cases cover ordinary, horizon, GL and null-linedef segs,
cache reuse, changing a special in both directions, owner clearing on level
teardown, and equal/different-sized replacement levels. `horizon_visibility`
continues to check the per-seg horizon visibility decision against Software.
GPU output and the performance improvement require Windows/Xbox validation.
