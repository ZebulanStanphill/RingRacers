"""Runs the game once per side and replay and collects the screenshots (see framecompare.py)."""

import json
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

from replays import ghost_args
from sides import GAME_DIR

# The windows of the game must stay on the hidden workspace the Hyprland rule sends them to; a window anywhere else
# would cover the desktop or take focus, and a game that loses focus would pause and give invalid results.
WINDOW_CLASS = re.compile(r"(?i).*(ringracers|rr-d3d12|rr-three|rr-fc).*")
HYPR_RULE = ('hl.window_rule({ name = "rr-wine-test", match = { class = "(?i).*(ringracers|rr-d3d12|rr-three|rr-fc).*" }, '
             'workspace = "special:rrtest silent", no_initial_focus = true, no_focus = true })')


def install_hypr_rule():
    subprocess.run(["hyprctl", "eval", HYPR_RULE], check=True)


def stray_windows():
    """Game windows that are not on the hidden workspace (empty if Hyprland is not there to ask)."""
    try:
        out = subprocess.run(["hyprctl", "clients", "-j"], capture_output=True, text=True, timeout=5).stdout
        clients = json.loads(out)
    except (OSError, subprocess.SubprocessError, ValueError):
        return []
    return [c for c in clients
            if WINDOW_CLASS.match(c.get("class", "")) and not c.get("workspace", {}).get("name", "").startswith("special:rrtest")]


class Watchdog(threading.Thread):
    """Kills the game if its window shows up on a visible workspace."""

    def __init__(self, process):
        super().__init__(daemon=True)
        self.process, self.stop, self.tripped = process, threading.Event(), None

    def run(self):
        while not self.stop.wait(1.0):
            stray = stray_windows()
            if stray:
                self.tripped = "game window on workspace %r" % stray[0].get("workspace", {}).get("name")
                self.process.kill()
                return


def run_one(side, replay, out_dir, size, timeout, keep_home, watch):
    """One game run. Returns a dict about it; the screenshots are in out_dir."""
    out_dir.mkdir(parents=True, exist_ok=True)
    for old in out_dir.glob("*.png"):
        old.unlink()
    home = out_dir / "home"
    if home.exists():
        shutil.rmtree(home)
    home.mkdir()

    tics = ",".join(str(t) for t in replay["tics"])
    extra = side.args + ["-shottics", tics] + ghost_args(replay)
    command = side.launcher.command(home, extra, size)
    started = time.time()
    status = "ok"
    with open(out_dir / "run.log", "wb") as log:
        process = subprocess.Popen(command, cwd=GAME_DIR, env=side.launcher.env(), stdout=log, stderr=subprocess.STDOUT)
        dog = Watchdog(process) if watch else None
        if dog:
            dog.start()
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            code, status = None, "timeout after %ds" % timeout
        if dog:
            dog.stop.set()
            if dog.tripped:
                status = dog.tripped
    seconds = time.time() - started

    # The game saves its screenshots in <home>/.../framecapture (the home directory it was given, under Wine too)
    found = sorted(home.rglob("framecapture/*.png"))
    for png in found:
        shutil.move(str(png), str(out_dir / png.name))
    if not keep_home:
        shutil.rmtree(home, ignore_errors=True)

    got = sorted(p.name for p in out_dir.glob("*.png"))
    if status == "ok" and code != 0:
        status = "exit code %s" % code
    if status == "ok" and len(got) != len(replay["tics"]):
        status = "%d of %d screenshots" % (len(got), len(replay["tics"]))
    return {"side": side.name, "replay": replay["name"], "status": status, "exit": code, "seconds": round(seconds, 1),
            "tics": replay["tics"], "screenshots": got, "command": command}


def run_all(sides, replays, results, size, timeout, resume, keep_home, watch):
    results.mkdir(parents=True, exist_ok=True)
    for side in sides:
        side.launcher.check()
    manifest_path = results / "manifest.json"
    manifest = {"sides": [s.describe() for s in sides], "size": list(size), "runs": []}
    if manifest_path.exists():
        try:
            old = json.loads(manifest_path.read_text())
            manifest["runs"] = old.get("runs", [])
            # keep the sides of earlier invocations that this one doesn't mention (results dirs can grow side by side)
            names = {s.name for s in sides}
            manifest["sides"] += [s for s in old.get("sides", []) if s["name"] not in names]
        except ValueError:
            pass

    failures = 0
    for replay in replays:
        for side in sides:
            out_dir = results / side.name / replay["name"]
            if resume and len(list(out_dir.glob("*.png"))) == len(replay["tics"]):
                print("%-22s %-14s already done" % (replay["name"], side.name))
                continue
            print("%-22s %-14s running ..." % (replay["name"], side.name), end="", flush=True)
            info = run_one(side, replay, out_dir, size, timeout, keep_home, watch)
            print(" %s in %.0fs, %d screenshots" % (info["status"], info["seconds"], len(info["screenshots"])))
            manifest["runs"] = [r for r in manifest["runs"] if (r["side"], r["replay"]) != (side.name, replay["name"])]
            manifest["runs"].append(info)
            manifest_path.write_text(json.dumps(manifest, indent=1))
            if info["status"] != "ok":
                failures += 1
                if info["status"].startswith("game window"):
                    sys.exit("stopped: %s (is the Hyprland rule installed? see README)" % info["status"])
    return failures
