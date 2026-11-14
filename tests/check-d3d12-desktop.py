#!/usr/bin/env python3
"""Check that the hardware renderer's engine code compiles with HAVE_D3D12 but without _UWP.

Usage: check-d3d12-desktop.py PATH/TO/compile_commands.json

For each C file under src/hardware/ (the Direct3D 12 driver in src/hardware/r_d3d12/
is C++ and is not included) in a normal game build's compile database, re-runs its
recorded compile command with -DHAVE_D3D12 and -fsyntax-only added and the output
options removed. Exits 1 if any of them reports an error, or if the database has no
such files.
"""

import json
import os
import shlex
import subprocess
import sys


def command_words(entry):
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry["command"])


def syntax_check_words(words):
    """The compile command without its output options, plus -DHAVE_D3D12 -fsyntax-only."""
    out = []
    skip = False
    for word in words:
        if skip:
            skip = False
        elif word == "-o":
            skip = True
        elif word.startswith("-o") and len(word) > 2 and not word.startswith("-objc"):
            pass
        elif word in ("-c", "-MD", "-MMD", "-MP"):
            pass
        elif word in ("-MF", "-MT", "-MQ"):
            skip = True
        elif word.startswith(("-MF", "-MT", "-MQ")):
            pass
        else:
            out.append(word)
    # Insert before the source file, which is conventionally last.
    return out[:-1] + ["-DHAVE_D3D12", "-fsyntax-only"] + out[-1:] if out else out


def main(argv):
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    with open(argv[1], encoding="utf-8") as f:
        database = json.load(f)

    failures = 0
    checked = 0
    for entry in database:
        source = entry["file"]
        directory = entry.get("directory", ".")
        if not os.path.isabs(source):
            source = os.path.join(directory, source)
        parts = os.path.normpath(source).replace("\\", "/").split("/")
        if not source.endswith(".c") or "src" not in parts:
            continue
        index = len(parts) - 1 - parts[::-1].index("src")
        if parts[index + 1:index + 2] != ["hardware"] or len(parts) != index + 3:
            continue

        words = syntax_check_words(command_words(entry))
        result = subprocess.run(words, cwd=directory, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, universal_newlines=True)
        checked += 1
        if result.returncode != 0:
            failures += 1
            print("FAILED: %s with -DHAVE_D3D12" % "/".join(parts[index:]))
            print(result.stdout)
        else:
            print("ok: %s" % "/".join(parts[index:]))

    if checked == 0:
        print("No src/hardware/*.c entries in %s" % argv[1])
        return 1
    print("%d checked, %d failed" % (checked, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
