#!/usr/bin/env python3
"""Shows the crashes logged in latest-log.txt (the "UWP: crash:" lines) with the game's functions named.

usage: crash-report.py latest-log.txt ringracers-uwp.map

The map must come from the same build as the log (the ringracers-uwp-symbols artifact).
"""
import bisect, re, sys

def read_map(path):
    base, syms, insyms = None, [], False
    for line in open(path, errors="replace"):
        m = re.search(r"Preferred load address is ([0-9a-fA-F]+)", line)
        if m:
            base = int(m[1], 16)
        if "Publics by Value" in line or "Static symbols" in line:
            insyms = True
            continue
        m = re.match(r"\s*([0-9a-fA-F]{4}):([0-9a-fA-F]{8})\s+(\S+)\s+([0-9a-fA-F]{16})\s+(.*)$", line) if insyms else None
        if m and int(m[4], 16):
            syms.append((int(m[4], 16) - base, m[3], m[5].split()[-1] if m[5].split() else ""))
    syms.sort()
    return syms

def main():
    log, mapfile = sys.argv[1], sys.argv[2]
    syms = read_map(mapfile)
    starts = [s[0] for s in syms]
    for line in open(log, encoding="utf-8", errors="replace"):
        if not line.startswith(("UWP: crash", "UWP:   called from")):
            continue
        line = line.rstrip("\n")
        m = re.search(r"ringracers-uwp\.exe\+([0-9a-f]+)", line)
        if m:
            off = int(m[1], 16)
            i = bisect.bisect_right(starts, off) - 1
            if i >= 0:
                line += f"   = {syms[i][1]}+{off - syms[i][0]:x}  [{syms[i][2]}]"
        print(line)

main()
