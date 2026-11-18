# Title wall-replay fixture

The fixture uses data from `maps/RR_Title.wad` in the [official Ring Racers v2.4
release](https://github.com/KartKrewDev/RingRacers/releases/tag/v2.4), examined
during the title-first-frame investigation on 2026-10-11.

Original SHA-256 hashes:

- `maps.pk3`: `b39339acedc4b5ea16ce027de85d4fe86c73f1df92cf0f487c364cf3b5296920`
- `RR_Title.wad`: `c4313bc6749ef66272ce84815077cdcaae479fae48010563120e3c0ce81c2c52`
- `TEXTMAP`: `f74bc8004a798638a2372fac91bd2917d68e59c0acaae7df81490124e2aee94c`
- Uncompressed `ZNODES` (`title-znodes.bin`, unchanged):
  `49d82995f5d66141d65c8fad55a06ba47ff7f5fa5b849b8c491c76bbab978274`

`title-map.json` is a compact TEXTMAP extract. It keeps sector heights,
lights, flat identities, and tags, along with linedef endpoints, sides, flags,
specials, and arguments. It also keeps sidedef textures, vertex coordinates,
and slope mapthing coordinates, heights, and IDs. It omits unused decoration
and rendering properties. Fog control sector 61 sets `lightcolor=0`. The replay
reproduces its black colormap and special 202 fog flag. The title has no heightsec controls or portals.

The fixture keeps the full topology (240 expanded vertices, 159 subsectors,
744 segs, and 158 nodes) because removing apparently unseen sectors or lines
would change BSP bounding boxes, angular clipping, line indices, and
slope/control relationships. At configure time, the fixture header
expands from 59,892 bytes of committed JSON/binary input. The fixture runs all 72 slope constructors to
match the map. Runtime assertions confirm that neither facing sector of the
eight affected lines nor any FOF control has an authored slope.
