"""Actions glue for the map sweep: the plan, the game's data, and the report. API calls use the job's read-only GH_TOKEN
through gh."""
import hashlib
import io
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request
import zipfile

from renderers import CROSS, DEFAULT_SIZE, RENDERERS, SIZES

GHOSTS = Path(__file__).with_name('ghosts.txt')
UNSTABLE = json.loads(Path(__file__).with_name('unstable.json').read_text())
ASSETS_URL = 'https://github.com/KartKrewDev/RingRacers/releases/download/v2.4/Dr.Robotnik.s-Ring-Racers-v2.4-Assets.zip'
# The v2.4 release's own digest of that file, which GitHub shows on the release page
ASSETS_SHA256 = 'eebad71b872c20f3323425bc4f4321b9b6256472bc485010d0a73c43fe0af120'
SHARD_MINUTES = 30  # what the planner aims for per capture job; the job's own limit is longer
MAX_SHARDS = 36
SUMMARY_ROWS = 60  # changed frames listed in the job summary; report.json has all of them


def unstable_maps(renderer):
    """{map: reason} of the maps known to differ from run to run with this renderer (unstable.json)."""
    entry = UNSTABLE.get(renderer, {})
    return {m: entry['reason'] for m in entry.get('maps', [])}


def api(path):
    return json.loads(subprocess.check_output(['gh', 'api', path], text=True))


def download(artifact, dest):
    class Redirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            redirected = super().redirect_request(req, fp, code, msg, headers, newurl)
            if redirected is not None:
                redirected.remove_header('Authorization')
            return redirected
    request = urllib.request.Request(artifact['archive_download_url'], headers={
        'Authorization': 'Bearer ' + os.environ['GH_TOKEN'],
        'Accept': 'application/vnd.github+json',
    })
    with urllib.request.build_opener(Redirect()).open(request, timeout=120) as response:
        data = response.read()
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        z.extractall(dest)


def baseline(renderer, dest, size, explicit=''):
    """The frames to compare with: the newest healthy, complete sweep of master at this size, or the run asked for.
    Returns that run's ID, or '' if there is none."""
    repo = os.environ['GITHUB_REPOSITORY']
    if explicit:
        runs = [{'id': int(explicit)}]
    else:
        runs = api(f'repos/{repo}/actions/workflows/map-sweep.yml/runs?branch=master&per_page=50')['workflow_runs']
    for run in runs:
        if str(run['id']) == os.environ['GITHUB_RUN_ID']:
            continue
        artifacts = api(f"repos/{repo}/actions/runs/{run['id']}/artifacts?per_page=100")['artifacts']
        for art in artifacts:
            if art['name'] == f'baseline-{renderer}' and not art['expired']:
                download(art, dest)
                m = json.loads((dest / 'manifest.json').read_text())
                if explicit or (not m['failures'] and m.get('full') and m.get('baseline_eligible')
                                and m.get('size') == [int(v) for v in size.split('x')]):
                    return str(run['id'])
                shutil.rmtree(dest)
    if explicit:
        raise RuntimeError('Explicit baseline has no healthy artifact for ' + renderer)
    return ''


def shard_count(renderer, size, maps):
    """How many capture jobs a renderer's share of the sweep is split into."""
    seconds = RENDERERS[renderer]['seconds'][size]
    return max(1, min(maps, MAX_SHARDS, math.ceil(maps * seconds / (SHARD_MINUTES * 60))))


def plan():
    repo = os.environ['GITHUB_REPOSITORY']
    ref = os.environ.get('INPUT_REF') or 'master'
    sha = api(f'repos/{repo}/commits/{ref}')['sha']
    renderers = [r for r in (os.environ.get('INPUT_RENDERERS') or ','.join(RENDERERS)).split(',') if r]
    if set(renderers) - set(RENDERERS):
        raise ValueError(f'Unknown renderer in {renderers}; known: {list(RENDERERS)}')
    size = os.environ.get('INPUT_SIZE') or DEFAULT_SIZE
    if size not in SIZES:
        raise ValueError(f'Unknown size {size}; the game takes {SIZES}')
    ghosts = GHOSTS.read_text().splitlines()
    names = {g.split('-')[0] for g in ghosts}
    asked = [m.strip().upper() for m in (os.environ.get('INPUT_MAPS') or '').split(',') if m.strip()]
    if set(asked) - names:
        raise ValueError(f'Unknown maps: {sorted(set(asked) - names)}')
    maps = len(set(asked)) or len(ghosts)
    skip = False
    if os.environ['GITHUB_EVENT_NAME'] == 'schedule':
        assert os.environ['GITHUB_REF'] == 'refs/heads/master'
        # The nightly run is for a master that has moved: one whose commit already has a complete sweep is skipped
        runs = api(f'repos/{repo}/actions/workflows/map-sweep.yml/runs?branch=master&per_page=50')['workflow_runs']
        for run in runs:
            if run['head_sha'] != sha or str(run['id']) == os.environ['GITHUB_RUN_ID']:
                continue
            artifacts = api(f"repos/{repo}/actions/runs/{run['id']}/artifacts?per_page=100")['artifacts']
            if any(a['name'] == 'master-sweep-' + sha for a in artifacts):
                skip = True
                break
    renderers = list(dict.fromkeys(renderers))
    shards = {r: shard_count(r, size, maps) for r in renderers}
    matrix = [dict(renderer=r, os=RENDERERS[r]['os'], shard=s, shards=shards[r])
              for r in renderers for s in range(shards[r])]
    master = ref in ('master', 'refs/heads/master') and os.environ['GITHUB_REF'] == 'refs/heads/master'
    builds = sorted({RENDERERS[r]['build'] for r in renderers})
    outputs = dict(sha=sha, skip=str(skip).lower(), master=str(master).lower(), size=size, maps=','.join(asked),
                   shards=json.dumps(shards), renderers=','.join(renderers), builds=json.dumps(builds),
                   matrix=json.dumps({'include': matrix}))
    with open(os.environ['GITHUB_OUTPUT'], 'a') as f:
        f.write(''.join(f'{k}={v}\n' for k, v in outputs.items()))
    print(f"Sweep of {sha} at {size}: {len(matrix)} capture jobs, shards {shards}, builds {builds}, "
          f"{'nothing to do (master already has a complete sweep)' if skip else 'go'}")


def assets(dest):
    """The official v2.4 game data, checked against the digest of the release."""
    if (dest / 'bios.pk3').exists():
        return
    dest.mkdir(parents=True, exist_ok=True)
    archive = dest.parent / 'assets.zip'
    digest = hashlib.sha256()
    with urllib.request.urlopen(ASSETS_URL, timeout=120) as response, archive.open('wb') as out:
        while chunk := response.read(1 << 20):
            digest.update(chunk)
            out.write(chunk)
    if digest.hexdigest() != ASSETS_SHA256:
        archive.unlink()
        raise RuntimeError(f'The assets archive has SHA-256 {digest.hexdigest()}, not the pinned {ASSETS_SHA256}')
    with zipfile.ZipFile(archive) as z:
        z.extractall(dest)
    archive.unlink()
    if not (dest / 'bios.pk3').exists():
        raise RuntimeError('bios.pk3 is not at the top of the assets archive')


def negative_control(source, output):
    """Invert one real capture; prove the report rejects it in the same environment."""
    from PIL import Image, ImageOps
    from compare import compare
    old, new = output / 'control-before', output / 'control-after'
    frame = next((source / 'frames').glob('*.png'))
    metadata = json.loads((source / 'manifest.json').read_text())
    metadata['failures'] = []
    for dest in (old, new):
        (dest / 'frames').mkdir(parents=True, exist_ok=True)
        (dest / 'manifest.json').write_text(json.dumps(metadata))
    shutil.copy2(frame, old / 'frames' / frame.name)
    with Image.open(frame) as image:
        ImageOps.invert(image.convert('RGB')).save(new / 'frames' / frame.name)
    result = compare(old, new, output / 'negative-control')
    if not result['red'] or result['frames'][0]['plain_pct'] != 100:
        raise RuntimeError('Deliberately inverted capture did not trigger a regression')
    shutil.rmtree(old)
    shutil.rmtree(new)


def find_parts(artifacts):
    """The shard folders in what was downloaded, by renderer. One artifact is extracted straight into the folder,
    several each into a folder of their own, so what a shard is comes from its manifest, not from its place."""
    found = {}
    for manifest in sorted(artifacts.rglob('manifest.json')):
        found.setdefault(json.loads(manifest.read_text())['renderer'], []).append(manifest.parent)
    return found


def merge(renderer, parts, shards, eligible, total):
    """One renderer's shards as a single capture folder with a manifest. Returns (folder, manifest)."""
    dest = Path(f'baseline-{renderer}')
    (dest / 'frames').mkdir(parents=True, exist_ok=True)
    manifests = [json.loads((p / 'manifest.json').read_text()) for p in parts if (p / 'manifest.json').exists()]
    if not manifests:
        raise RuntimeError('No manifests for ' + renderer)
    m = manifests[0].copy()
    m['baseline_eligible'] = eligible
    m['failures'] = [f for x in manifests for f in x['failures']]
    if len(manifests) != shards:
        m['failures'].append(f'Missing shard manifests: {len(manifests)} of {shards}')
    m['environment'] = {'runner_environments': [json.loads(x) for x in sorted(
        {json.dumps(x['environment'], sort_keys=True) for x in manifests})]}
    m['map_environments'] = {g.split('-')[0]: x['environment'] for x in manifests for g in x['maps']}
    m['maps'] = [g for x in manifests for g in x['maps']]
    m['runs'] = [r for x in manifests for r in x['runs']]
    if len(m['runs']) != len(m['maps']):
        m['failures'].append('A shard did not finish all requested maps')
    m['full'] = len(m['maps']) == total and len(set(m['maps'])) == total
    m['seconds'] = sum(x['seconds'] for x in manifests)
    for part in parts:
        for frame in (part / 'frames').glob('*.png'):
            shutil.copy2(frame, dest / 'frames' / frame.name)
    wanted = sum(len(r['tics']) for r in m['runs'])
    found = len(list((dest / 'frames').glob('*.png')))
    if found != wanted:
        m['failures'].append(f'Frame inventory has {found} frames; the maps that ran asked for {wanted}')
    (dest / 'manifest.json').write_text(json.dumps(m, indent=2))
    return dest, m


def report():
    from compare import compare
    artifacts = Path('incoming')
    shards = json.loads(os.environ['SHARDS'])
    size = os.environ.get('SIZE') or DEFAULT_SIZE
    eligible = os.environ.get('BASELINE_ELIGIBLE') == 'true'
    total = len(GHOSTS.read_text().splitlines())
    statuses, overview, merged, commit = [], [], {}, ''
    control_done = False
    all_full = True
    found = find_parts(artifacts)
    for renderer in RENDERERS:
        parts = found.get(renderer, [])
        if renderer not in shards:
            continue
        if not parts:
            raise RuntimeError(f'No captures arrived for {renderer}')
        dest, m = merge(renderer, parts, shards[renderer], eligible, total)
        merged[renderer] = dest
        commit = m['commit']
        all_full &= m['full']
        if not control_done and any((dest / 'frames').glob('*.png')):
            negative_control(dest, Path('reports'))
            control_done = True
        old = Path(f'previous-{renderer}')
        run = baseline(renderer, old, size, os.environ.get('INPUT_BASELINE', ''))
        if run and not m['full']:
            # A quick run compares only its requested maps, including maps that failed.
            scope = {g.split('-')[0] for g in m['maps']}
            for frame in (old / 'frames').glob('*.png'):
                if frame.stem.rsplit('_', 1)[0] not in scope:
                    frame.unlink()
        result = compare(old if run else None, dest, Path(f'reports/{renderer}'), limit=SUMMARY_ROWS,
                         title=RENDERERS[renderer]['label'], unstable=unstable_maps(renderer))
        statuses.append(result['red'])
        runs = [r['seconds'] for r in m['runs']]
        overview.append(f"| {RENDERERS[renderer]['label']} | {'RED' if result['red'] else 'green'} | {len(m['maps'])} | "
                        f"{len(list((dest / 'frames').glob('*.png')))} | {len(m['failures'])} | "
                        f"{result['identical']} / {len(result['frames'])} | {run or 'none'} | "
                        f"{m['seconds'] / 60:.0f} | {(sum(runs) / len(runs) if runs else 0):.0f} |")
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as f:
            f.write(Path(f'reports/{renderer}/summary.md').read_text() + '\n')
    for first, second in CROSS:
        if first in merged and second in merged:
            compare(merged[first], merged[second], Path('reports/cross-backend'), True, limit=SUMMARY_ROWS,
                    title=f"{RENDERERS[first]['label']} against {RENDERERS[second]['label']}, same commit")
            with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as f:
                f.write(Path('reports/cross-backend/summary.md').read_text() + '\n')
    if not statuses:
        raise RuntimeError('No captures')
    head = ['| Renderer | Verdict | Maps | Frames | Health failures | Identical to baseline | Baseline run | '
            'Capture minutes (all shards) | Seconds per map |', '|---|---|---:|---:|---:|---:|---|---:|---:|']
    table = '\n'.join(['# Map sweep', f"Size {size}. Capture minutes count the games only; "
                       'builds, setup, and uploads are in the jobs\' own times.', '', *head, *overview, ''])
    summary = Path(os.environ['GITHUB_STEP_SUMMARY'])
    summary.write_text(table + '\n' + summary.read_text())
    if eligible and all_full and len(statuses) == len(RENDERERS):
        Path('master-sweep.json').write_text(json.dumps({'commit': commit}))
    return any(statuses)


if __name__ == '__main__':
    command = sys.argv[1]
    if command == 'plan':
        plan()
    elif command == 'assets':
        assets(Path(sys.argv[2]))
    elif command == 'report':
        raise SystemExit(report())
