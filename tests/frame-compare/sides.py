"""How to start the game for one side of a comparison.

A side is "NAME=KIND:TARGET [engine arguments...]" on the command line, for example

    sw-linux=native:/path/to/ringracers -software
    d3d-wine=wine:rr-d3d12.exe -d3d12
    vk-linux=native:/other/build/bin/ringracers -vulkan

KIND says how to start TARGET, and the rest are the renderer's own arguments. Adding another way to start a game is
one more entry in KINDS: a class with a `command(self, home, extra)` method (and `prepare`, if it needs more).
Adding another native build or renderer needs nothing here: it is another `native:` side.
"""

import os
import shlex
import shutil
import subprocess
from pathlib import Path

GAME_DIR = Path(os.environ.get("RR_GAME_DIR", Path.home() / ".cache/rr-wine/game"))
WINE_PREFIX = Path(os.environ.get("RR_WINE_PREFIX", Path.home() / ".cache/rr-wine/prefix"))

# Arguments every side gets: a fixed window, no sound, and no pausing or focus notices when the window is not focused
# (the windows are on a hidden workspace and never are). A fresh home directory per run keeps saved settings out.
COMMON_ARGS = ["-win", "-noaudio", "+showfocuslost", "off", "+pauseifunfocused", "off"]


class Side:
    """One thing to compare: a name, how to start the game, and the renderer's arguments."""

    def __init__(self, name, kind, target, args):
        if kind not in KINDS:
            raise SystemExit("side %s: unknown kind %r (have: %s)" % (name, kind, ", ".join(sorted(KINDS))))
        self.name, self.kind, self.target, self.args = name, kind, target, list(args)
        self.launcher = KINDS[kind](target)

    @classmethod
    def parse(cls, text):
        """'NAME=KIND:TARGET args...' -> Side"""
        name, sep, rest = text.partition("=")
        if not sep or not name or not all(c.isalnum() or c in "-_." for c in name):
            raise SystemExit("side %r: expected NAME=KIND:TARGET [arguments], NAME of letters, digits, - _ ." % text)
        words = shlex.split(rest)
        if not words or ":" not in words[0]:
            raise SystemExit("side %r: expected KIND:TARGET after the =" % text)
        kind, _, target = words[0].partition(":")
        return cls(name, kind, target, words[1:])

    def describe(self):
        return {"name": self.name, "kind": self.kind, "target": self.target, "args": self.args}


class Native:
    """A game binary of this machine's own system, run in the game's data directory."""

    def __init__(self, target):
        self.binary = Path(target).expanduser().resolve()

    def check(self):
        if not self.binary.is_file():
            raise SystemExit("native binary not found: %s" % self.binary)

    def env(self):
        return dict(os.environ)

    def command(self, home, extra, size):
        return [str(self.binary), "-home", str(home), "-width", str(size[0]), "-height", str(size[1])] + COMMON_ARGS + extra


class Wine:
    """A Windows build (an .exe in the game's data directory) run with Wine."""

    def __init__(self, target):
        self.exe = target

    def check(self):
        path = Path(self.exe)
        if not path.is_absolute():
            path = GAME_DIR / self.exe
        if not path.is_file():
            raise SystemExit("Windows build not found: %s (copy it into %s)" % (path, GAME_DIR))
        if not shutil.which("wine") or not shutil.which("winepath"):
            raise SystemExit("wine and winepath are needed for wine: sides")
        self.exe = str(path)

    def env(self):
        env = dict(os.environ)
        env["WINEPREFIX"] = str(WINE_PREFIX)
        env.setdefault("WINEDEBUG", "-all")
        env.setdefault("WINEDLLOVERRIDES", "mscoree,mshtml=")
        return env

    def command(self, home, extra, size):
        winhome = subprocess.run(["winepath", "-w", str(home)], env=self.env(), capture_output=True, text=True,
                                 check=True).stdout.strip()
        return ["wine", self.exe, "-home", winhome, "-width", str(size[0]), "-height", str(size[1])] + COMMON_ARGS + extra


KINDS = {"native": Native, "wine": Wine}
