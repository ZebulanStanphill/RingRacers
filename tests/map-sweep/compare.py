"""Compare renderer captures; percentages are pixels, not RGB channels."""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image


def near(a, b):
    h, w, _ = a.shape
    pad = np.pad(b, ((1, 1), (1, 1), (0, 0)), mode='edge')
    found = np.zeros((h, w), bool)
    for y in range(3):
        for x in range(3):
            found |= np.abs(a - pad[y:y+h, x:x+w]).max(-1) <= 8
    return found


def measures(a, b):
    diff = (a != b).any(-1)
    structural = diff & ~(near(a, b) & near(b, a)) if diff.any() else diff.copy()
    return float(diff.mean() * 100), float(structural.mean() * 100), diff, structural


def stack(environment):
    """What of a runner's environment can change a picture: the OS family, architecture, and graphics stack (Mesa and LLVM, or
    WARP), but not the runner image's own version or the CPU model (both are recorded). Hosted images roll out over days with two
    versions in use at once, and hosted CPUs vary; pictures stayed the same across both, and a version that carries a new
    graphics stack shows in the stack itself."""
    if 'runner_environments' in environment:
        return sorted({json.dumps(stack(e), sort_keys=True) for e in environment['runner_environments']})
    return {k: v for k, v in environment.items() if k not in ('ImageVersion', 'RR_CPU')}


def images(manifest):
    """The runner image versions a capture ran on."""
    found = {e.get('ImageVersion') for e in manifest.get('map_environments', {}).values()}
    found |= {e.get('ImageVersion') for e in manifest.get('environment', {}).get('runner_environments', [])}
    found.add(manifest.get('environment', {}).get('ImageVersion'))
    return sorted(v for v in found if v)


def describe(failure):
    if isinstance(failure, str):
        return failure
    parts = [f"{failure['map']}: exit {failure['exit']}"]
    if failure.get('missing'):
        parts.append(f"missing tics {failure['missing']}")
    parts += [e for e in failure.get('errors', [])][:6]
    return '; '.join(parts)


def compare(before, after, output, informational=False, limit=None, title=None, unstable=None):
    """Compare two capture folders (frames/ and manifest.json). limit: how many changed frames the summary lists. unstable:
    {map: reason} for maps whose frames are known to differ from run to run; their differences are listed but never red."""
    unstable = unstable or {}
    output.mkdir(parents=True, exist_ok=True)
    new = json.loads((after / 'manifest.json').read_text())
    old = json.loads((before / 'manifest.json').read_text()) if before else None
    changed_env = False
    rows = []
    names = {p.name for p in (after / 'frames').glob('*.png')}
    previous = {p.name for p in (before / 'frames').glob('*.png')} if old else set()
    for name in sorted(names | previous):
        mapname = Path(name).stem.rsplit('_', 1)[0]
        frame_env_changed = bool(old and stack(old.get('map_environments', {}).get(mapname, old['environment'])) !=
                                 stack(new.get('map_environments', {}).get(mapname, new['environment'])))
        changed_env |= frame_env_changed
        row = {'frame': name, 'map': mapname, 'environment_changed': frame_env_changed, 'unstable': mapname in unstable}
        if name not in names:
            row['status'] = 'missing'
        elif not old:
            row['status'] = 'new baseline'
        elif name not in previous:
            row['status'] = 'extra'
        else:
            a = np.asarray(Image.open(before / 'frames' / name).convert('RGB'), dtype=np.int16)
            b = np.asarray(Image.open(after / 'frames' / name).convert('RGB'), dtype=np.int16)
            if a.shape != b.shape:
                row['status'] = 'size changed'
            else:
                plain, structural, diff, mask = measures(a, b)
                row.update(status='identical' if plain == 0 else 'changed', plain_pct=plain,
                           structural_pct=structural)
        rows.append(row)
    worst = sorted((r for r in rows if r.get('plain_pct', 0)),
                   key=lambda r: (r['structural_pct'], r['plain_pct']), reverse=True)[:12]
    for row in worst:
        name = row['frame']
        a = np.asarray(Image.open(before / 'frames' / name).convert('RGB'), dtype=np.int16)
        b = np.asarray(Image.open(after / 'frames' / name).convert('RGB'), dtype=np.int16)
        _, _, diff, mask = measures(a, b)
        ys, xs = np.where(diff)
        x0, x1, y0, y1 = max(0, xs.min()-16), min(a.shape[1], xs.max()+17), max(0, ys.min()-16), min(a.shape[0], ys.max()+17)
        heat = np.zeros(a.shape, np.uint8)
        heat[diff] = (90, 90, 90)
        heat[mask] = (255, 0, 0)
        for label, pixels in [('before', a), ('after', b), ('difference', heat)]:
            Image.fromarray(pixels.astype('uint8')[y0:y1, x0:x1]).save(output / f'{name[:-4]}-{label}.png')
    health = new.get('failures', [])
    inventory = any(r['status'] in ('missing', 'extra', 'size changed') for r in rows)
    regression = any((r.get('structural_pct', 0) > 0.1 or r.get('plain_pct', 0) > 2)
                     and not r['environment_changed'] and not r['unstable'] for r in rows)
    red = not informational and bool(health or inventory or (old and regression))
    result = dict(red=red, environment_changed=changed_env, baseline=old and old.get('commit'),
                  commit=new['commit'], failures=health, frames=rows,
                  identical=sum(r['status'] == 'identical' for r in rows))
    (output / 'report.json').write_text(json.dumps(result, indent=2))
    recorded = sum(r['status'] == 'new baseline' for r in rows)
    lines = [f"## {title or new['renderer']}: {'RED' if red else 'GREEN'}" + (' (information only)' if informational else '')]
    if informational:
        lines += [f"Commit: `{new['commit']}`. Before is the first renderer's frames, after the second's, so differences are "
                  'between renderers and never a verdict. Both measures are in the table.',
                  f"Identical: **{result['identical']} / {len(rows)}**."]
    else:
        lines += [f"Commit: `{new['commit']}`; baseline: `{result['baseline'] or 'none'}`.",
                  f"Graphics stack changed: **{changed_env}**. Identical: **{result['identical']} / {len(rows)}**. "
                  f"Health failures: **{len(health)}**.",
                  f"Runner images: {', '.join(images(new)) or 'unknown'}; baseline's: {', '.join(images(old)) if old else 'none'}.",
                  'Red for a health failure, a missing, extra, or resized frame, or (with the same environment) a frame '
                  'whose structural difference is above 0.1% or plain difference above 2%.']
    quiet = sorted({r['map'] for r in rows if r['unstable'] and r['status'] == 'changed'})
    if quiet:
        reasons = sorted({unstable[m] for m in quiet})
        lines.append('Known to differ from run to run, so never red (`tests/map-sweep/unstable.json`): ' + ', '.join(quiet)
                     + '. ' + ' '.join(r if r.endswith('.') else r + '.' for r in reasons))
    if recorded:
        lines.append(f'No baseline to compare with: these {recorded} frames are recorded as the first one.')
    lines += ['', '| Frame | Status | Plain % | Structural % |', '|---|---|---:|---:|']
    listed = sorted((r for r in rows if r['status'] not in ('identical', 'new baseline')),
                    key=lambda r: (r.get('structural_pct', 0), r.get('plain_pct', 0)), reverse=True)
    lines += [f"| {r['frame']}{' (unstable)' if r['unstable'] else ''} | {r['status']} | {r.get('plain_pct', 0):.4f} | {r.get('structural_pct', 0):.4f} |"
              for r in listed[:limit]]
    if limit is not None and len(listed) > limit:
        lines.append(f'| ... {len(listed) - limit} more frames, in report.json | | | |')
    lines += ['', 'Health failures:', *[f'- {describe(f)}' for f in health[:30]]] if health else []
    if len(health) > 30:
        lines.append(f'- ... {len(health) - 30} more, in report.json')
    (output / 'summary.md').write_text('\n'.join(lines) + '\n')
    return result


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('after', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--before', type=Path)
    p.add_argument('--informational', action='store_true')
    a = p.parse_args()
    raise SystemExit(compare(a.before, a.after, a.output, a.informational)['red'])
