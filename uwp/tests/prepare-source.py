#!/usr/bin/env python3
"""Fetch exactly vanilla v2.4 and apply this repository's current engine patch.

The destination must not already exist, so a developer's checkout is never reset
or silently tested with an older applied patch. No game assets/vcpkg are needed.
"""

import argparse
import subprocess
from pathlib import Path

BASE = "7f895c9a7f5ed77a1ccb3e94b326660e744b523e"
UPSTREAM = "https://github.com/KartKrewDev/RingRacers.git"


def prepare(destination):
    destination.mkdir(parents=True, exist_ok=False)
    patch = Path(__file__).resolve().parents[1] / "patches/ringracers-uwp.patch"

    def git(*args):
        subprocess.run(["git", "-C", str(destination), *args], check=True)

    git("init", "-q")
    git("config", "core.autocrlf", "false")
    git("config", "core.eol", "lf")
    git("fetch", "--depth", "1", UPSTREAM, BASE)
    git("checkout", "--detach", "FETCH_HEAD")
    git("apply", "--check", str(patch))
    git("apply", str(patch))
    print(f"Patched v2.4 source ready at {destination}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    prepare(args.destination.resolve())
