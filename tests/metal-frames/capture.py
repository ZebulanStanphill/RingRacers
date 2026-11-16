#!/usr/bin/env python3
"""Runs the macOS game with VibeRant Metal under Metal's validation and collects what it drew.

  capture.py sweep  --game BIN --data DIR --out DIR [--shard K/N] [--ghosts FILE] [--tics LIST] [--only A,B]
  capture.py scenes --game BIN --data DIR --out DIR [--only A,B]
  (each: [--idle SECONDS] [--timeout SECONDS] [-- more arguments for the game])

sweep: plays staff ghosts in the engine's frame capture mode (-shotghost, -shottics; tests/frame-compare/README.md),
one game per ghost, and moves the screenshots to <out>/frames. scenes: what the ghosts don't reach (the title screen,
a menu, models, Screen Scaling, fullscreen, changing the renderer while the game runs, quitting), as screenshots the
game takes itself and pictures of the screen (screencapture), in <out>/scenes/<scene>.

Every run gets a fresh home directory and MTL_DEBUG_LAYER=1 and MTL_SHADER_VALIDATION=1, and its output goes to
<out>/logs. A run fails if the game doesn't exit with 0 in time, if the log doesn't say that the Metal backend with
both kinds of validation is in use, if a frame is missing, or if the log has a validation message, a "not
implemented" line, or an error of the driver's. The exit status is 1 if any run failed; <out>/summary.json has the
details. Standard library only, so that it runs on a macOS runner as it is. See tests/README.md, "Metal frames".

--idle SECONDS starts each game only once nobody has touched the Mac's keyboard or mouse for that long, and waits
otherwise: the game opens a window on the Mac's screen, which matters on a Mac that is someone's own.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_TICS = "70,350,700,1400,2100"
def game_args(width=1280, height=720, renderer="-metal", window=True):
    """What every run of the game gets: a window of a fixed size (or the whole screen), no sound, and a game that keeps
    running without focus"""
    return ((["-win"] if window else []) + ["-width", str(width), "-height", str(height), "-noaudio"] + ([renderer] if renderer else [])
            + ["+showfocuslost", "off", "+pauseifunfocused", "off"])


# Lines of the log that fail a run. Metal's shader validation and failed command buffers come through mtl12 ("Metal:
# ..."); the debug layer writes its own lines and then stops the program.
BAD = [
    (re.compile(r"not implemented", re.I), "not implemented"),
    (re.compile(r"validation (error|fault|failure)|failed assertion|-\[MTL\w*Debug\w* |MTLDebug.*:\d+: failed", re.I), "Metal validation"),
    (re.compile(r"Metal: (?!device: ).*(command buffer .*(failed|error)|shader validation|device (was )?(lost|removed))", re.I), "Metal error"),
    (re.compile(r"(UWP D3D12|VibeRant \w+): .*(failed|error|couldn't|can't place|removed|gave no drawable)", re.I), "driver error"),
    (re.compile(r"I_Error|SIGSEGV|SIGABRT|signal: |An error occurred within", re.I), "crash"),
    (re.compile(r"couldn't start \(the log says why\)"), "fell back to Software"),
]
# Lines that match the above and are fine
ALLOWED = [
    re.compile(r"frame \d+ done: 0 errors"),
]
DEVICE_LINE = re.compile(r"Metal: device: .*")
# What the log says when the game is about to show its error dialog
STOPPED = re.compile(r"An error occurred within|Process killed by signal|I_Error\(\)|failed assertion")


def bad_lines(text):
    found = []
    for line in text.splitlines():
        if any(ok.search(line) for ok in ALLOWED):
            continue
        for pattern, kind in BAD:
            if pattern.search(line):
                found.append((kind, line.strip()[:300]))
                break
    return found


IDLE_SECONDS = 0  # --idle


def idle_seconds():
    """How long the Mac has had no keyboard or mouse input"""
    try:
        text = subprocess.run(["ioreg", "-c", "IOHIDSystem"], capture_output=True, text=True, timeout=20).stdout
        return int(re.search(r'"HIDIdleTime" = (\d+)', text).group(1)) // 1000000000
    except (OSError, AttributeError, subprocess.TimeoutExpired):
        return 0


def wait_until_idle():
    said = False
    while IDLE_SECONDS and idle_seconds() < IDLE_SECONDS:
        if not said:
            print("someone is at the Mac: waiting until it has been idle for %d s" % IDLE_SECONDS, flush=True)
            said = True
        time.sleep(20)


def run_game(game, data, home, args, log_path, timeout, during=None, validation=True):
    """One run of the game. during(process) runs on another thread while the game does. Returns (exit code or None on a
    timeout, seconds)."""
    wait_until_idle()
    shutil.rmtree(home, ignore_errors=True)
    os.makedirs(home)
    env = dict(os.environ)
    env["RINGRACERSWADDIR"] = data
    if validation:
        env["MTL_DEBUG_LAYER"] = "1"
        env["MTL_SHADER_VALIDATION"] = "1"
    command = [game, "-home", home] + args
    start = time.time()
    with open(log_path, "wb") as log:
        log.write((" ".join(command) + "\n").encode())
        log.flush()
        process = subprocess.Popen(command, cwd=data, env=env, stdout=log, stderr=subprocess.STDOUT)
        thread = None
        if during:
            thread = threading.Thread(target=during, args=(process,), daemon=True)
            thread.start()
        # A game that crashes or stops with an error shows a dialog and waits for its button, which nobody presses here
        code = None
        stopped = None
        while time.time() - start < timeout:
            try:
                code = process.wait(timeout=2)
                break
            except subprocess.TimeoutExpired:
                pass
            if stopped is None:
                with open(log_path, errors="replace") as written:
                    if STOPPED.search(written.read()):
                        stopped = time.time()
            elif time.time() - stopped > 5:
                break
        if code is None and process.poll() is None:
            process.kill()
            process.wait()
            code = "killed after an error" if stopped else None
        if thread:
            thread.join(timeout=30)
    return code, time.time() - start


def check_log(log_path, need_metal=True, validation=True):
    with open(log_path, errors="replace") as handle:
        text = handle.read()
    problems = bad_lines(text)
    device = DEVICE_LINE.search(text)
    if need_metal:
        if "VibeRant backend: Metal" not in text:
            problems.append(("no Metal", "the log doesn't say that the VibeRant backend is Metal"))
        if not device:
            problems.append(("no Metal", "the log has no Metal device line"))
        elif validation and not ("API validation on" in device.group(0) and "shader validation on" in device.group(0)):
            problems.append(("no validation", "the device wasn't made with API and shader validation: " + device.group(0)))
    presented = [int(n) for n in re.findall(r"presenter: gave the game's OpenGL window back; (\d+) frames presented", text)]
    return problems, (device.group(0) if device else ""), sum(presented)


def pngs_under(directory):
    found = []
    for root, _, files in os.walk(directory):
        found += [os.path.join(root, name) for name in files if name.lower().endswith(".png")]
    return sorted(found)


def sweep(options):
    with open(options.ghosts) as handle:
        ghosts = [line.strip() for line in handle if line.strip() and not line.startswith("#")]
    if options.only:
        wanted = options.only.split(",")
        ghosts = [g for g in ghosts if any(w.lower() in g.lower() for w in wanted)]
    k, n = (int(part) for part in options.shard.split("/"))
    mine = [g for i, g in enumerate(ghosts) if i % n == k]
    tics = options.tics.split(",")
    frames_dir = os.path.join(options.out, "frames")
    logs_dir = os.path.join(options.out, "logs")
    os.makedirs(frames_dir, exist_ok=True)
    os.makedirs(logs_dir, exist_ok=True)
    home = os.path.join(options.out, "home")
    results = []
    for ghost in mine:
        log_path = os.path.join(logs_dir, ghost + ".log")
        args = game_args() + ["-shotghost", ghost, "-shottics", options.tics, "+fpscap", "300"] + options.extra
        code, seconds = run_game(options.game, options.data, home, args, log_path, options.timeout)
        problems, device, presented = check_log(log_path)
        shots = [shot for shot in pngs_under(home) if os.path.basename(os.path.dirname(shot)) == "framecapture"]
        for shot in shots:
            shutil.move(shot, os.path.join(frames_dir, os.path.basename(shot)))
        # A replay shorter than the last tics ends the run with fewer frames, which the log says
        with open(log_path, errors="replace") as handle:
            text = handle.read()
        finished = re.search(r"framecapture: .*; (\d+) of (\d+) screenshots taken", text)
        if code is None:
            problems.append(("timeout", "the game didn't exit in %d s" % options.timeout))
        elif code != 0:
            problems.append(("exit", "the game exited with %s" % code))
        if not finished:
            problems.append(("capture", "the log has no framecapture summary"))
        elif int(finished.group(1)) != len(shots):
            problems.append(("capture", "the log says %s screenshots and %d files were written" % (finished.group(1), len(shots))))
        if not shots:
            problems.append(("capture", "no frame was written"))
        result = {"ghost": ghost, "exit": code, "seconds": round(seconds, 1), "frames": len(shots), "of": len(tics),
                  "presented": presented, "device": device, "problems": problems}
        results.append(result)
        print("%s: %d/%d frames, exit %s, %.0f s, %d frames presented%s" % (ghost, len(shots), len(tics), code, seconds, presented,
              "" if not problems else "  PROBLEMS: " + "; ".join("%s: %s" % p for p in problems[:5])), flush=True)
    shutil.rmtree(home, ignore_errors=True)
    return results


def screencapture(path):
    try:
        subprocess.run(["screencapture", "-x", path], timeout=20, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        print("screencapture failed: %s" % error, flush=True)


def keys(codes, pause=0.6):
    """Presses keys by their macOS key codes, each held for a quarter of a second, as a person at the keyboard would:
    they reach whatever window has the keyboard, which is what this checks. (The game reads its menu keys once per
    tic, so a key that goes down and up at once is missed.)"""
    script = ("ObjC.import('CoreGraphics'); function key(code, down) { $.CGEventPost($.kCGHIDEventTap, "
              "$.CGEventCreateKeyboardEvent($(), code, down)); } key(%d, true); delay(0.25); key(%d, false);")
    for code in codes:
        subprocess.run(["osascript", "-l", "JavaScript", "-e", script % (code, code)], timeout=20, check=False)
        time.sleep(pause)


KEY_RETURN, KEY_A, KEY_D = 36, 0, 2


def scene_list():
    """name -> the game's arguments ("args"), what to do while it runs ("during"), how many screenshots of its own the
    game must write ("shots"), the frame's size ("size", 1280x720 unless given), and the renderer's argument
    ("renderer", -metal unless given; None for the game's default)"""
    ghost = "RR_AERIALHIGHLANDS-battlekukku-FerronNuff"
    race = ["-shotghost", ghost, "-shottics", "350,700", "+fpscap", "300"]
    # A replay to look at, without the capture mode's screenshots (the level time 99999 is never reached). The capture
    # mode starts the ghost after the command line's commands, so these start it themselves, to go on once it plays.
    replay = ["-shotghost", ghost, "-shottics", "99999", "+framecapture_ghost", ghost]
    playing = r"framecapture: playing staff ghost"
    gave_back = r"presenter: gave the game's OpenGL window back"
    took = r"presenter: presenting on a Metal window"

    def wait(tics):
        return ["+wait", str(tics)]

    shot = ["+screenshot"] + wait(5)

    def pictures(*marks):
        """Takes a picture of the screen some seconds after the log's nth line that matches a pattern: (pattern, n,
        seconds, name of the picture)"""
        def during(process, scene_dir, log_path):
            for pattern, nth, seconds, name in marks:
                while True:
                    with open(log_path, errors="replace") as written:
                        if len(re.findall(pattern, written.read())) >= nth:
                            break
                    if process.poll() is not None:
                        return
                    time.sleep(0.3)
                time.sleep(seconds)
                if process.poll() is None:
                    screencapture(os.path.join(scene_dir, "screen-%s.png" % name))
        return during

    def menu(process, scene_dir, log_path):
        # The title screen, then the keyboard: Return leaves the title screen, A (the menus' confirm key) leaves the
        # warning that follows for a new player's first menu, A again takes that menu's Exit Program, and D cancels
        # the question it asks. The pictures of the screen say how far the keys got, which is the check that the Metal
        # window has the keyboard.
        time.sleep(25)
        screencapture(os.path.join(scene_dir, "screen-1-title.png"))
        for index, (key, name) in enumerate([(KEY_RETURN, "warning"), (KEY_A, "menu"), (KEY_A, "question"), (KEY_D, "menu-again")]):
            keys([key], pause=3)
            screencapture(os.path.join(scene_dir, "screen-%d-%s.png" % (index + 2, name)))

    def scaling(mode):
        # A 320x200 frame on the whole screen, so that Fit, Integer, and 1:1 put it in three different rectangles. The
        # game starts in fullscreen, as it does for a player (-win, which the other scenes have, keeps it in a window).
        return {"args": ["+scr_scaling", mode] + replay + wait(900) + shot + ["+quit"],
                "during": pictures((playing, 1, 9, "fullscreen")), "shots": 1, "size": (320, 200), "window": False}

    return {
        # The game as a player starts it: no renderer named, so the default has to be VibeRant Metal
        "default-title": {"args": wait(700) + shot + ["+quit"], "during": pictures((took, 1, 9, "intro")), "shots": 1,
                          "renderer": None},
        "title-menu": {"args": wait(2100) + shot + ["+quit"], "during": menu, "shots": 1},
        "models-on": {"args": race + ["+gr_models", "on"], "shots": 2},
        "models-off": {"args": race + ["+gr_models", "off"], "shots": 2},
        "switch-renderer": {"args": replay + wait(600) + shot + ["+renderer", "Software"] + wait(400) + shot
                            + ["+renderer", "VibeRant Metal"] + wait(400) + shot + ["+renderer", "Software"] + wait(100)
                            + ["+renderer", "VibeRant Metal"] + wait(300) + shot + ["+quit"],
                            "during": pictures((playing, 1, 6, "1-metal"), (gave_back, 1, 3, "2-software"), (took, 2, 3, "3-metal")),
                            "shots": 4},
        "software-start": {"args": replay + wait(600) + shot + ["+renderer", "VibeRant Metal"] + wait(400) + shot + ["+quit"],
                           "during": pictures((playing, 1, 3, "1-software-start"), (playing, 1, 5, "2-software"),
                                              (took, 1, 3, "3-metal")), "shots": 2,
                           "renderer": "-software"},
        "scaling-fit": scaling("Fit"),
        "scaling-integer": scaling("Integer"),
        "scaling-1to1": scaling("1:1"),
        "vsync": {"args": ["+vid_wait", "on"] + replay + wait(700) + shot + ["+quit"],
                  "during": pictures((playing, 1, 8, "race")), "shots": 1},
        # -mtldebug has Metal end every pass with an encoder of its own, which is what the paravirtual GPU's driver
        # couldn't take before mtl12 kept its encoders small
        "mtldebug": {"args": race + ["-mtldebug"], "shots": 2},
    }


def scenes(options):
    scenes_dir = os.path.join(options.out, "scenes")
    logs_dir = os.path.join(options.out, "logs")
    os.makedirs(logs_dir, exist_ok=True)
    home = os.path.join(options.out, "home")
    results = []
    for name, scene in scene_list().items():
        if options.only and name not in options.only.split(","):
            continue
        scene_dir = os.path.join(scenes_dir, name)
        os.makedirs(scene_dir, exist_ok=True)
        log_path = os.path.join(logs_dir, "scene-" + name + ".log")
        width, height = scene.get("size", (1280, 720))
        renderer = scene.get("renderer", "-metal")
        args = game_args(width, height, renderer, scene.get("window", True)) + scene["args"] + options.extra
        during = scene.get("during")
        wrapped = (lambda process, d=during, s=scene_dir, l=log_path: d(process, s, l)) if during else None
        code, seconds = run_game(options.game, options.data, home, args, log_path, options.timeout, wrapped)
        problems, device, presented = check_log(log_path)
        shots = pngs_under(home)
        for index, shot in enumerate(shots):
            shutil.move(shot, os.path.join(scene_dir, "game-%d-%s" % (index + 1, os.path.basename(shot))))
        if code is None:
            problems.append(("timeout", "the game didn't exit in %d s" % options.timeout))
        elif code != 0:
            problems.append(("exit", "the game exited with %s" % code))
        if len(shots) != scene["shots"]:
            problems.append(("capture", "%d screenshots of the game's own, expected %d" % (len(shots), scene["shots"])))
        if presented == 0:
            problems.append(("present", "the log doesn't say that any frame was presented"))
        results.append({"scene": name, "exit": code, "seconds": round(seconds, 1), "screenshots": len(shots), "presented": presented,
                        "device": device, "problems": problems})
        print("%s: exit %s, %.0f s, %d screenshots, %d frames presented%s" % (name, code, seconds, len(shots), presented,
              "" if not problems else "  PROBLEMS: " + "; ".join("%s: %s" % p for p in problems[:5])), flush=True)
    shutil.rmtree(home, ignore_errors=True)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mode", choices=["sweep", "scenes"])
    parser.add_argument("--game", required=True, help="the game's binary")
    parser.add_argument("--data", required=True, help="the game's data directory (bios.pk3, data/, models/)")
    parser.add_argument("--out", required=True)
    parser.add_argument("--shard", default="0/1", help="K/N: every Nth ghost of the list, starting at the Kth")
    parser.add_argument("--ghosts", default=os.path.join(HERE, "ghosts.txt"))
    parser.add_argument("--tics", default=DEFAULT_TICS)
    parser.add_argument("--only", default="", help="sweep: ghosts whose name contains one of these; scenes: these scenes")
    parser.add_argument("--timeout", type=int, default=600, help="seconds a run of the game may take")
    parser.add_argument("--idle", type=int, default=0,
                        help="start a game only once the Mac has had no keyboard or mouse input for this many seconds")
    # More arguments for the game come after "--", wherever the mode stands
    arguments, extra = sys.argv[1:], []
    if "--" in arguments:
        extra = arguments[arguments.index("--") + 1:]
        arguments = arguments[:arguments.index("--")]
    options = parser.parse_args(arguments)
    options.extra = extra
    global IDLE_SECONDS
    IDLE_SECONDS = options.idle
    options.game = os.path.abspath(options.game)
    options.data = os.path.abspath(options.data)
    options.out = os.path.abspath(options.out)
    os.makedirs(options.out, exist_ok=True)
    results = sweep(options) if options.mode == "sweep" else scenes(options)
    failed = [r for r in results if r["problems"]]
    with open(os.path.join(options.out, "summary.json"), "w") as handle:
        json.dump({"mode": options.mode, "runs": results}, handle, indent=1)
    print("%d runs, %d with problems" % (len(results), len(failed)))
    return 1 if failed or not results else 0


if __name__ == "__main__":
    sys.exit(main())
