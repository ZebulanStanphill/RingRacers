"""Reproduce ghost list and short-replay schedules from official v2.4 assets.

The duration parser mirrors G_GetStaffGhostBrief, G_SkipDemoExtraFiles, and
G_SkipDemoSkins in src/g_demo.cpp. It intentionally accepts only v2.4's versions 14 through 18.
"""
import argparse
import json
from pathlib import Path
import struct
import zipfile


def duration(b):
    version = struct.unpack_from('<H', b, 14)[0]
    if b[:12] != b'\xf0KartReplay\x0f' or not 14 <= version <= 18 or b[96:100] != b'PLAY':
        raise ValueError('Expected an official v2.4 staff ghost (demo versions 14–18)')
    p = b.index(b'\0', 100) + 1 + 16  # map name, checksum
    flags = struct.unpack_from('<H', b, p)[0]
    if not flags & 1:
        raise ValueError('Staff ghost has no attack time')
    p += 2
    p = b.index(b'\0', p) + 2  # gametype, numlaps
    files = b[p]; p += 1
    for _ in range(files):
        p = b.index(b'\0', p) + 1 + 16
    if version < 16:
        skins = b[p]; p += 1
        availability = 32
    else:
        skins = struct.unpack_from('<H', b, p)[0]; p += 2
        availability = 16
    p += skins * (16 + 2 + 4) + availability
    return struct.unpack_from('<I', b, p)[0]


def main():
    p = argparse.ArgumentParser()
    p.add_argument('archive', type=Path)
    p.add_argument('--schedules', action='store_true')
    a = p.parse_args()
    seen = {}
    schedules = {}
    with zipfile.ZipFile(a.archive) as archive:
        for name in archive.namelist():
            if not name.endswith('.lmp'):
                continue
            ghost = Path(name).stem
            mapname = ghost.split('-')[0]
            if mapname in seen:
                continue
            seen[mapname] = ghost
            ticks = duration(archive.read(name))
            if ticks < 2135:
                schedules[mapname] = dict(recorded_race_tics=ticks, tics=[70, 210, 350, 525, 700])
    print(json.dumps(schedules, indent=2) if a.schedules else '\n'.join(seen.values()))


if __name__ == '__main__':
    main()
