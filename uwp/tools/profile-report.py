#!/usr/bin/env python3
"""Turns E:\\ringracers\\uwp-profile.txt into the functions the main thread spent its time in.

usage: profile-report.py uwp-profile.txt ringracers-uwp.map [top N] [--dll name.dll=path ...]

With call stacks in the profile (builds after 9aa8798), also shows each function's time including what it called
("inclusive"), and which game functions the hottest code was called from. --dll maps addresses in other modules to
their exports (like memset in VCRUNTIME140_APP.dll), given a copy of that DLL (the VCLibs .appx in the
ringracers-uwp artifact is a zip that has one). Needs objdump (llvm-objdump) for that.
"""
import bisect, collections, re, subprocess, sys

def undecorate(name):
    # Rough MSVC undecoration: ?func@Class@ns@@... -> ns::Class::func, ??$func@args@@... -> func<args>
    if name.startswith("??$"):
        func, _, rest = name[3:].partition("@")
        return f"{func}<{rest.split('@@')[0]}>"
    if not name.startswith("?") or name.startswith("??"):
        return name
    parts = name[1:].split("@@")[0].split("@")
    return "::".join(reversed([p for p in parts if p]))

def read_map(path):
    base = None
    syms = []
    insyms = False
    for line in open(path, errors="replace"):
        m = re.search(r"Preferred load address is ([0-9a-fA-F]+)", line)
        if m:
            base = int(m[1], 16)
        if "Publics by Value" in line or "Static symbols" in line:
            insyms = True
            continue
        if not insyms:
            continue
        m = re.match(r"\s*([0-9a-fA-F]{4}):([0-9a-fA-F]{8})\s+(\S+)\s+([0-9a-fA-F]{16})\s+(.*)$", line)
        if m:
            addr = int(m[4], 16)
            if addr == 0:
                continue
            obj = m[5].split()[-1] if m[5].split() else ""
            syms.append((addr - base, undecorate(m[3]), obj))
    syms.sort()
    return syms

def read_exports(path):
    """Exported functions of a DLL as (rva, name), from objdump."""
    out = subprocess.run(["objdump", "-p", path], capture_output=True, text=True).stdout
    exports = []
    for line in out.splitlines():
        m = re.match(r"\s+\d+\s+0x([0-9a-f]+)\s+(\S+)$", line)
        if m:
            exports.append((int(m[1], 16), m[2]))
    return sorted(set(exports))

class Symbols:
    def __init__(self, mapfile, dlls):
        self.syms = read_map(mapfile)
        self.starts = [s[0] for s in self.syms]
        self.dlls = {}
        for name, path in dlls.items():
            exports = read_exports(path)
            self.dlls[name.lower()] = ([e[0] for e in exports], [e[1] for e in exports])

    def name(self, modpath, offset, withobj=True):
        modname = modpath.replace("\\", "/").split("/")[-1]
        if modname.lower() == "ringracers-uwp.exe":
            i = bisect.bisect_right(self.starts, offset) - 1
            if i < 0:
                return "?"
            return f"{self.syms[i][1]}  [{self.syms[i][2]}]" if withobj else self.syms[i][1]
        if modname.lower() in self.dlls:
            starts, names = self.dlls[modname.lower()]
            i = bisect.bisect_right(starts, offset) - 1
            if i >= 0:
                return f"{modname}!{names[i]}"
        return modname

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--dll")]
    dlls = {}
    for i, a in enumerate(sys.argv):
        if a == "--dll":
            name, _, path = sys.argv[i + 1].partition("=")
            dlls[name] = path
            args.remove(sys.argv[i + 1])
    profile, mapfile = args[0], args[1]
    top = int(args[2]) if len(args) > 2 else 40
    symbols = Symbols(mapfile, dlls)

    modules = {}
    phases = collections.OrderedDict()
    frames = {}
    stacks = collections.OrderedDict()
    section = None
    header = ""
    for line in open(profile, errors="replace"):
        line = line.rstrip("\n")
        if line.startswith("Ring Racers UWP profile"):
            header = line
            continue
        m = re.match(r"module (\d+): (.*)$", line)
        if m:
            modules[int(m[1])] = m[2]
            continue
        m = re.match(r"phase (.+): (\d+) samples$", line)
        if m:
            section = ("phase", m[1])
            phases[m[1]] = []
            continue
        m = re.match(r"frame (\d+) (\d+) ([0-9a-f]+)$", line)
        if m:
            frames[int(m[1])] = (int(m[2]), int(m[3], 16))
            continue
        m = re.match(r"stacks (.+): (\d+)$", line)
        if m:
            section = ("stacks", m[1])
            stacks[m[1]] = []
            continue
        if section and section[0] == "phase":
            m = re.match(r"(\d+) ([0-9a-f]+) (\d+)$", line)
            if m:
                phases[section[1]].append((int(m[1]), int(m[2], 16), int(m[3])))
        elif section and section[0] == "stacks":
            parts = line.split()
            if parts and parts[0].isdigit():
                stacks[section[1]].append((int(parts[0]), [int(p) for p in parts[1:]]))

    print(header)
    total = sum(c for entries in phases.values() for _, _, c in entries)
    allfuncs = collections.Counter()
    for name, entries in phases.items():
        n = sum(c for _, _, c in entries)
        print(f"\n== {name}: {n} samples, {100.0 * n / max(total, 1):.1f}% of all in levels")
        funcs = collections.Counter()
        mods = collections.Counter()
        for mod, off, count in entries:
            path = modules.get(mod, "?")
            mods[path.replace("\\", "/").split("/")[-1]] += count
            key = symbols.name(path, off)
            funcs[key] += count
            allfuncs[key] += count
        print("   modules: " + ", ".join(f"{m} {100.0 * c / n:.1f}%" for m, c in mods.most_common(6)))
        for key, count in funcs.most_common(top):
            print(f"   {100.0 * count / n:5.1f}%  {100.0 * count / total:5.1f}% all  {key}")
    print(f"\n== all phases: {total} samples")
    for key, count in allfuncs.most_common(top):
        print(f"   {100.0 * count / total:5.1f}%  {key}")

    if not stacks:
        return

    def frame_name(fid, index):
        mod, off = frames[fid]
        # Return addresses point after the call; step back into it to find the caller
        return symbols.name(modules.get(mod, "?"), off - 1 if index else off, withobj=False)

    def is_game(fid):
        mod, _ = frames[fid]
        return modules.get(mod, "").lower().endswith("ringracers-uwp.exe")

    for name, entries in stacks.items():
        n = sum(c for c, _ in entries)
        if not n:
            continue
        inclusive = collections.Counter()
        callers = collections.defaultdict(collections.Counter)
        leaf = collections.Counter()
        for count, ids in entries:
            names = [frame_name(fid, i) for i, fid in enumerate(ids)]
            for fname in set(names):
                inclusive[fname] += count
            leaf[names[0]] += count
            # The first game function above code outside the game, or the caller of a game function
            for i in range(1, len(ids)):
                if names[i] != names[0] and (is_game(ids[i]) or not is_game(ids[0])):
                    callers[names[0]][names[i]] += count
                    break
        print(f"\n== {name}, inclusive (with what each function called, up to {max(len(i) for _, i in entries)} calls deep): {n} samples with stacks")
        for fname, count in inclusive.most_common(top):
            print(f"   {100.0 * count / n:5.1f}%  {fname}")
        print(f"\n== {name}, callers of the hottest code")
        for fname, count in leaf.most_common(min(top, 25)):
            parts = ", ".join(f"{c2} {100.0 * k / count:.0f}%" for c2, k in callers[fname].most_common(4))
            print(f"   {100.0 * count / n:5.1f}%  {fname}  <-  {parts}")

main()
