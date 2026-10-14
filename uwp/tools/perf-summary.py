#!/usr/bin/env python3
"""Summarizes the 5-second 'UWP perf' blocks of a latest-log.txt, including the Direct3D 12 'D3D12:' lines: averages
for each stretch of racing, and with -t, a table of every block.

usage: perf-summary.py latest-log.txt [-t]
"""
import re, sys

# Keys whose captures are kept as text rather than parsed as numbers.
STRING_KEYS = {'cpus', 'path'}

PATTERNS = [
    (r'UWP perf:\s+([\d.]+) frames/s \(([\d.]+) with 3D\) from ([\d.]+) loops/s; work ([\d.]+) ms per loop, ([\d.]+) ms at worst',
     ['fps', 'fps3d', 'loops', 'work', 'worst']),
    (r'UWP perf:\s+3D ([\d.]+) ms per frame \(BSP ([\d.]+), sprite clip ([\d.]+), portals and skybox ([\d.]+), planes ([\d.]+), masked ([\d.]+)\)',
     ['r3d', 'bsp', 'spr', 'portal', 'planes', 'masked']),
    (r'UWP perf:\s+UI ([\d.]+) ms, software screen ([\d.]+) ms, finish update ([\d.]+) ms', ['ui', 'sw', 'fin']),
    (r'UWP perf:\s+([\d.]+) draws', ['draws']),
    (r'UWP perf:\s+([\d.]+) tics/s; logic ([\d.]+) ms per tic \(players ([\d.]+), thinkers ([\d.]+), Lua ([\d.]+), ACS ([\d.]+)\), bots ([\d.]+)',
     ['tics', 'logic', 'players', 'thinkers', 'lua', 'acs', 'bots']),
    (r'UWP perf:\s+mobjs now: (\d+) thinking, (\d+) scenery, (\d+) not thinking; per tic: ([\d.]+) P_CheckPosition calls, ([\d.]+) Lua mobj hooks',
     ['mobjs', 'scenery', 'nothink', 'checkpos', 'luahooks']),
    (r'UWP perf:\s+main thread on processors (.*); profiler paused it ([\d.]+) ms per second', ['cpus', 'paused']),
    (r'UWP perf:\s+([\d.]+) R_PointInSubsector calls per second, ([\d.]+)% answered from its cache', ['pointlookups', 'pointcached']),
    (r'UWP perf:\s+D3D12: ([\d.]+) frames/s; ([\d.]+) submissions per frame \(mid-frame: [^)]*\); ([\d.]+) barriers per frame; \d+ command allocators \(\d+ made\); \d+ releases pending; (\d+) errors',
     ['d12fps', 'submits', 'barriers', 'd12errors']),
    (r'UWP perf:\s+D3D12: fence waits (\d+), ([\d.]+) ms total \(worst ([\d.]+)\), (\d+) timeouts; presenting ([\d.]+) ms per frame',
     ['fencewaits', 'fencems', 'fenceworst', 'fencetimeouts', 'presentms']),
    (r'UWP perf:\s+D3D12: per frame ([\d.]+) clears, ([\d.]+) polygons, ([\d.]+) batches, ([\d.]+) lines; ([\d.]+) draws, ([\d.]+) vertices, ([\d.]+) indices, ([\d.]+) constant updates',
     ['clears', 'polys', 'batches', 'lines', 'd12draws', 'verts', 'indices', 'constupd']),
    (r'UWP perf:\s+D3D12: pipeline: ([\d.]+) PSO changes/frame; cache (\d+) alive, (\d+) created \(([\d.]+) ms total, worst ([\d.]+) ms\); ([\d.]+) root CBV sets, ([\d.]+) table sets/frame',
     ['psochanges', 'psoalive', 'psocreated', 'psoms', 'psoworst', 'rootsets', 'tablesets']),
    (r'UWP perf:\s+D3D12: textures: (\d+) created, (\d+) uploads \(([\d.]+) MB\), (\d+) deleted, (\d+) flushes, (\d+) alive',
     ['texcreated', 'texuploads', 'texmb', 'texdeleted', 'texflushes', 'texalive']),
    (r'UWP perf:\s+D3D12: present: interop path (\w+), sync [^;]*; per frame copy ([\d.]+) ms, copy-fence wait ([\d.]+) ms \(worst ([\d.]+), \d+ of \d+ not shown\), GL ([\d.]+), swap ([\d.]+) ms; ANGLE read waits (\d+), ([\d.]+) ms total; (\d+) timeouts',
     ['path', 'copyms', 'copywait', 'copyworst', 'glms', 'swapms', 'anglewaits', 'anglewaitms', 'angletimeouts']),
    (r'UWP perf:\s+D3D12: GPU ([\d.]+) ms per frame \(worst ([\d.]+), (\d+) timed\)', ['gpums', 'gpuworst', 'gputimed']),
    # The 'N dedicated,' field is missing from older logs, so its capture is None there and parses as 0.
    (r'UWP perf:\s+D3D12: texture heaps: (\d+) placed, (?:(\d+) dedicated, )?(\d+) committed created; (\d+) heaps \(([\d.]+) MB used, ([\d.]+) MB allocated, peak ([\d.]+) MB\)',
     ['placed', 'dedicated', 'committed', 'heaps', 'heapused', 'heapalloc', 'heappeak']),
]

def parse(path):
    rows, cur = [], None
    for i, l in enumerate(open(path, encoding='utf-8', errors='replace').read().splitlines(), 1):
        m = re.match(r'UWP perf: ([\d.]+) s at(?: (\d+)x(\d+))?', l)
        if m:
            if cur:
                rows.append(cur)
            cur = {'line': i, 'dur': float(m.group(1)),
                   'resolution': f'{m.group(2)}x{m.group(3)}' if m.group(2) else 'unknown'}
            continue
        if cur is None:
            continue
        for pat, keys in PATTERNS:
            m = re.match(pat, l)
            if m:
                cur.update({k: (v if k in STRING_KEYS else 0.0 if v is None else float(v)) for k, v in zip(keys, m.groups())})
                break
    if cur:
        rows.append(cur)
    return rows

def g(r, k):
    return r.get(k, 0.0)

def show(rows):
    print(f"{'line':>5} {'resolution':>10} {'fps':>5} {'3dfps':>5} {'loops':>5} {'work':>5} {'idle%':>5} | {'3D':>5} {'bsp':>5} {'spr':>4} {'port':>4} {'pln':>4} {'msk':>4} | "
          f"{'ui':>4} {'sw':>4} {'fin':>4} | {'tics':>5} {'logic':>5} {'plyr':>4} {'thnk':>5} {'bots':>4} | {'mobjs':>5} {'chkpos':>6} {'pause':>5} | "
          f"{'gpu':>5} {'pso':>4} {'subm':>4} {'copyw':>5}  cpus")
    for r in rows:
        idle = 100 - g(r, 'loops') * g(r, 'work') / 10
        print(f"{r['line']:>5} {r['resolution']:>10} {g(r,'fps'):5.1f} {g(r,'fps3d'):5.1f} {g(r,'loops'):5.1f} {g(r,'work'):5.1f} {idle:5.0f} | "
              f"{g(r,'r3d'):5.1f} {g(r,'bsp'):5.1f} {g(r,'spr'):4.1f} {g(r,'portal'):4.1f} {g(r,'planes'):4.1f} {g(r,'masked'):4.1f} | "
              f"{g(r,'ui'):4.1f} {g(r,'sw'):4.1f} {g(r,'fin'):4.1f} | {g(r,'tics'):5.1f} {g(r,'logic'):5.1f} {g(r,'players'):4.1f} {g(r,'thinkers'):5.1f} {g(r,'bots'):4.1f} | "
              f"{g(r,'mobjs'):5.0f} {g(r,'checkpos'):6.0f} {g(r,'paused'):5.1f} | "
              f"{g(r,'gpums'):5.2f} {g(r,'psochanges'):4.0f} {g(r,'submits'):4.2f} {g(r,'copywait'):5.2f}  {r.get('cpus', '')}")

def races(rows):
    """Groups racing blocks at the same resolution, where every frame drew 3D and tics ran at full speed."""
    groups, cur = [], []
    for r in rows:
        if g(r, 'fps3d') > 0 and abs(g(r, 'fps3d') - g(r, 'fps')) < 0.2 and g(r, 'tics') > 33:
            if cur and r['resolution'] != cur[-1]['resolution']:
                groups.append(cur)
                cur = []
            cur.append(r)
        else:
            if cur:
                groups.append(cur)
            cur = []
    if cur:
        groups.append(cur)
    return groups

def summarize(group):
    n = len(group)
    avg = lambda k: sum(g(r, k) for r in group) / n
    idle = 100 - sum(r['loops'] * r['work'] for r in group) / n / 10
    duration = sum(r['dur'] for r in group)
    text = (f"lines {group[0]['line']}-{group[-1]['line']} ({duration:g} s at {group[0]['resolution']}): {avg('fps'):.1f} FPS, {avg('loops'):.1f} loops/s, idle {idle:.0f}%, "
            f"3D {avg('r3d'):.1f} ms (BSP {avg('bsp'):.1f}, sprclip {avg('spr'):.1f}, portals {avg('portal'):.1f}, planes {avg('planes'):.1f}, masked {avg('masked'):.1f}), "
            f"UI {avg('ui'):.1f}, sw {avg('sw'):.1f}, finish {avg('fin'):.1f}; logic {avg('logic'):.1f} ms/tic (players {avg('players'):.1f}, thinkers {avg('thinkers'):.1f}, bots {avg('bots'):.1f})")
    if any('mobjs' in r for r in group):
        text += f"; {avg('mobjs'):.0f} thinking mobjs, {avg('checkpos'):.0f} P_CheckPosition/tic, profiler pauses {avg('paused'):.1f} ms/s"
    if any('pointcached' in r for r in group):
        text += f"; R_PointInSubsector {avg('pointlookups'):.0f}/s, {avg('pointcached'):.0f}% cached"
    # The PSO cache's created count is cumulative, so the stretch's own creations are its last minus its first.
    if any('d12fps' in r for r in group):
        text += (f"; D3D12: {avg('submits'):.2f} submits/frame, {avg('barriers'):.1f} barriers, {avg('psochanges'):.0f} PSO changes "
                 f"({g(group[-1], 'psocreated') - g(group[0], 'psocreated'):.0f} created during it, worst {avg('psoworst'):.1f} ms), GPU {avg('gpums'):.2f} ms, "
                 f"present copy {avg('copyms'):.2f} + wait {avg('copywait'):.2f} ms (path {group[0].get('path', '?')}), "
                 f"{avg('d12draws'):.0f} draws/frame, textures {avg('texalive'):.0f} alive")
    return text

if __name__ == '__main__':
    rows = parse(sys.argv[1])
    if '-t' in sys.argv:
        show(rows)
    for grp in races(rows):
        if len(grp) >= 4:
            print(summarize(grp))
