#!/usr/bin/env python3
"""Shows a function's disassembly with how many profile samples landed on each instruction, to see what in it is slow.

usage: annotate-function.py uwp-profile.txt ringracers-uwp.map ringracers-uwp.exe FUNCTION [min_samples]

ringracers-uwp.exe is in the .msix of the same build's ringracers-uwp artifact (a zip). FUNCTION is a symbol name
from the map, or a regular expression. Needs objdump (llvm-objdump).
"""
import bisect, collections, re, subprocess, sys

profile, mapfile, exe, func = sys.argv[1:5]
minsamples = int(sys.argv[5]) if len(sys.argv) > 5 else 1

base = None
syms = []
insyms = False
for line in open(mapfile, errors="replace"):
    m = re.search(r"Preferred load address is ([0-9a-fA-F]+)", line)
    if m:
        base = int(m[1], 16)
    if "Publics by Value" in line or "Static symbols" in line:
        insyms = True
        continue
    if not insyms:
        continue
    m = re.match(r"\s*([0-9a-fA-F]{4}):([0-9a-fA-F]{8})\s+(\S+)\s+([0-9a-fA-F]{16})\s+(.*)$", line)
    if m and int(m[4], 16):
        syms.append((int(m[4], 16) - base, m[3]))
syms.sort()
starts = [s[0] for s in syms]

def name_of(rva):
    i = bisect.bisect_right(starts, rva) - 1
    return syms[i][1] if i >= 0 else "?"

targets = [(s, n) for s, n in syms if n == func or re.fullmatch(func, n)]
if not targets:
    sys.exit(f"no symbol {func}")
start, name = targets[0]
end = starts[bisect.bisect_right(starts, start)]

modules = {}
counts = collections.Counter()
phasecounts = collections.defaultdict(collections.Counter)
phase = None
for line in open(profile, errors="replace"):
    m = re.match(r"module (\d+): (.*)$", line)
    if m:
        modules[int(m[1])] = m[2].strip()
        continue
    m = re.match(r"phase (.+): (\d+) samples$", line.strip())
    if m:
        phase = m[1]
        continue
    m = re.match(r"(\d+) ([0-9a-f]+) (\d+)$", line.strip())
    if m and modules.get(int(m[1]), "").lower().endswith("ringracers-uwp.exe"):
        off = int(m[2], 16)
        if start <= off < end:
            counts[off] += int(m[3])
            phasecounts[phase][off] += int(m[3])

total = sum(counts.values())
print(f"{name}: rva {start:x}-{end:x} ({end - start} bytes), {total} samples; by phase: " +
      ", ".join(f"{p} {sum(c.values())}" for p, c in phasecounts.items()))
dis = subprocess.run(["objdump", "-d", "--no-show-raw-insn", f"--start-address={base + start:#x}", f"--stop-address={base + end:#x}", exe],
                     capture_output=True, text=True).stdout
for line in dis.splitlines():
    m = re.match(r"\s*([0-9a-f]+):\s+(.*)$", line)
    if not m:
        continue
    rva = int(m[1], 16) - base
    n = counts.get(rva, 0)
    ins = m[2]
    call = re.search(r"callq\s+0x([0-9a-f]+)", ins)
    if call:
        ins += f"   <{name_of(int(call[1], 16) - base)}>"
    if n >= minsamples or call or "j" in ins.split()[0] and n:
        print(f"{n:6d} {rva:8x}  {ins}")
