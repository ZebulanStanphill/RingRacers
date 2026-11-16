"""Play real staff ghosts in the frame capture mode, one game per map, with a hard per-map deadline.

Every run gets a fresh home directory, so no setting of the machine reaches the game. The result is a manifest
(commit, environment, per-map seconds, health failures), one log per map, and the frames. The exit status is 1 if any
map failed a health check, but the frames of the maps that ran are kept either way.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import time

from renderers import DEFAULT_SIZE, RENDERERS, SIZES

TICS = [70, 350, 700, 1400, 2100]
# Lines that fail a map. "failed: 0x" is the Direct3D 12 driver's own error line; "VUID-" and "Validation Error" are
# Vulkan's validation layers; "D3D12 debug ERROR" is the Direct3D debug layer, which the driver copies to the log.
ERROR = re.compile(r'I_Error|not implemented|Validation Error|VUID-|D3D12 debug ERROR|An error occurred within|'
                   r'SIGSEGV|signal:|device (?:lost|removed)|failed: 0x|Falling back to Software|'
                   r'couldn.t start.*Software renderer', re.I)
# What the game prints when it is about to show its error dialog and wait for a button nobody presses
STOPPED = re.compile(r'An error occurred within|Process killed by signal|I_Error\(\)')
POLL = 2  # seconds between looks at a running game
STOP_GRACE = 5  # seconds a game that printed its error gets to exit by itself


def selected(ghosts, maps, shard, count):
    subset = {m.upper() for m in maps.split(',') if m}
    known = {g.split('-')[0] for g in ghosts}
    if subset - known:
        raise ValueError(f'Unknown maps: {sorted(subset - known)}')
    pool = [g for g in ghosts if not subset or g.split('-')[0] in subset]
    return pool[shard::count]


def log_errors(renderer, text):
    """The lines of a log that fail a map: what every renderer's log can say, and what this one's debugging layer says."""
    extra = RENDERERS[renderer]['errors']
    pattern = re.compile('|'.join([ERROR.pattern, *extra]), re.I) if extra else ERROR
    return sorted(set(pattern.findall(text)))


def backend_problems(renderer, text):
    """What the log must say for the renderer that was asked for to be the one that ran."""
    spec = RENDERERS[renderer]
    problems = [f"The log doesn't contain {line!r}, so {spec['label']} may not be what ran"
                for line in spec['requires'] if line not in text]
    problems += [f"The log contains {line!r}, so something other than {spec['label']} ran"
                 for line in spec['forbids'] if line in text]
    return problems


def png_problems(path, width, height):
    header = path.read_bytes()[:24]
    if len(header) != 24 or header[:8] != b'\x89PNG\r\n\x1a\n':
        return [f'Not a PNG: {path.name}']
    if struct.unpack('>II', header[16:24]) != (width, height):
        return [f'Unexpected dimensions: {path.name}']
    return []


def engine_log(home):
    """The engine's own log in a home directory (it keeps a few copies of it), or ''. It can hold lines that redirected output
    lacks, and on Windows redirected output arrives in blocks."""
    logs = [p for p in home.rglob('*.txt') if 'log' in p.name]
    return max(logs, key=lambda p: p.stat().st_size).read_text(errors='replace') if logs else ''


def log_watch(log):
    """What run_game watches by default: the number of frames the log says were captured, and the log."""
    def watch():
        text = log.read_text(errors='replace')
        return text.count('framecapture: tic '), text
    return watch


def run_game(cmd, cwd, env, log, timeout, stall, watch=None):
    """Run one game. Returns its exit code, or why it was killed: 'timeout' (it ran for `timeout` seconds in all),
    'hang' (`stall` seconds passed without a new frame captured, from the start or from the last one), or 'stopped'
    (the game printed that it had crashed or raised an error, and is waiting at a dialog, so it was killed after a few
    seconds). Slow is fine and a hang is not: lavapipe and WARP take minutes per map, but never minutes per frame.
    watch() returns (frames so far, log text); a game that writes its log to a file in blocks, as one on Windows can,
    needs a watch that counts the frames' files and reads the engine's own log too."""
    watch = watch or log_watch(log)
    started = last_progress = time.monotonic()
    stopped_at = None
    captured = 0
    with log.open('w') as stream:
        process = subprocess.Popen(cmd, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT, env=env)
        while True:
            try:
                return process.wait(timeout=POLL)
            except subprocess.TimeoutExpired:
                pass
            now = time.monotonic()
            frames, text = watch()
            if frames > captured:
                captured = frames
                last_progress = now
            if stopped_at is None and STOPPED.search(text):
                stopped_at = now
            if stopped_at is not None and now - stopped_at > STOP_GRACE:
                code = 'stopped'
            elif now - started > timeout:
                code = 'timeout'
            elif now - last_progress > stall:
                code = 'hang'
            else:
                continue
            break
        process.kill()
        process.wait()
        return code


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--data', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--renderer', choices=RENDERERS, required=True)
    p.add_argument('--maps', default='')
    p.add_argument('--size', choices=SIZES, default=DEFAULT_SIZE)
    p.add_argument('--shard', type=int, default=0)
    p.add_argument('--shards', type=int, default=8)
    p.add_argument('--timeout', type=int, default=1500, help='seconds for one map in all')
    p.add_argument('--stall', type=int, default=300, help='seconds without a new frame captured that make a hang')
    a = p.parse_args()
    width, height = a.size.split('x')
    started = time.monotonic()
    out = a.output.resolve()
    frames = out / 'frames'
    frames.mkdir(parents=True, exist_ok=True)
    folder = Path(__file__).parent
    ghosts = selected((folder / 'ghosts.txt').read_text().splitlines(), a.maps, a.shard, a.shards)
    schedules = json.loads((folder / 'schedules.json').read_text())
    manifest = dict(renderer=a.renderer, commit=os.environ.get('RR_COMMIT') or subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
                    environment={k: os.environ.get(k, '') for k in ['ImageOS', 'ImageVersion', 'RR_DRIVER', 'RR_CPU', 'RUNNER_ARCH']},
                    tics=TICS, size=[int(width), int(height)], maps=ghosts, failures=[], runs=[])
    env = dict(os.environ, RINGRACERSWADDIR=str(a.data.resolve()))
    for ghost in ghosts:
        prefix = ghost.split('-')[0]
        tics = schedules.get(prefix, {}).get('tics', TICS)
        home = out / 'homes' / ghost
        home.mkdir(parents=True, exist_ok=True)
        cmd = [str(a.binary.resolve()), '-home', str(home), '-shotdir', str(frames), '-win', '-width', width,
               '-height', height, '-noaudio', *RENDERERS[a.renderer]['flags'], '-shotghost', ghost,
               '-shottics', ','.join(map(str, tics)), '+fpscap', '300', '+showfocuslost', 'off',
               '+pauseifunfocused', 'off']
        tick = time.monotonic()
        log = out / f'{ghost}.log'
        def watch(log=log, home=home, prefix=prefix):
            return len(list(frames.glob(f'{prefix}_*.png'))), log.read_text(errors='replace') + '\n' + engine_log(home)
        code = run_game(cmd, a.data, env, log, a.timeout, a.stall, watch)
        seconds = time.monotonic() - tick
        text = log.read_text(errors='replace')
        text += '\n' + engine_log(home)
        log.write_text(text, errors='replace')
        missing = [t for t in tics if not (frames / f'{prefix}_{t:05d}.png').exists()]
        errors = log_errors(a.renderer, text)
        for tic in tics:
            frame = frames / f'{prefix}_{tic:05d}.png'
            if frame.exists():
                errors += png_problems(frame, int(width), int(height))
        errors += backend_problems(a.renderer, text)
        if f'done; {len(tics)} of {len(tics)} screenshots taken' not in text:
            errors.append('The game did not report all screenshots taken')
        if code != 0 or missing or errors:
            manifest['failures'].append(dict(map=prefix, exit=code, missing=missing, errors=errors))
        manifest['runs'].append(dict(ghost=ghost, tics=tics, seconds=round(seconds, 1), exit=code))
        manifest['seconds'] = time.monotonic() - started
        (out / 'manifest.json').write_text(json.dumps(manifest, indent=2))
        print(ghost, code, f'{seconds:.0f}s', 'missing', missing, 'errors', errors, flush=True)
        shutil.rmtree(home, ignore_errors=True)
    # Even an empty shard must contribute metadata.
    manifest['seconds'] = time.monotonic() - started
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    return bool(manifest['failures'])


if __name__ == '__main__':
    raise SystemExit(main())
