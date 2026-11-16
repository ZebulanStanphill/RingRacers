#!/usr/bin/env python3
"""Runs the macOS game with VibeRant Metal under Metal's validation and collects what it drew.

  capture.py sweep    --game BIN --data DIR --out DIR [--shard K/N] [--ghosts FILE] [--tics LIST] [--only A,B]
  capture.py scenes   --game BIN --data DIR --out DIR [--only A,B]
  capture.py hardware --game BIN --data DIR --out DIR [--only A,B]
  (each: [--idle SECONDS] [--timeout SECONDS] [-- more arguments for the game])

sweep: plays staff ghosts in the engine's frame capture mode (-shotghost, -shottics; tests/frame-compare/README.md),
one game per ghost, and moves the screenshots to <out>/frames. scenes: what the ghosts don't reach (the title screen,
a menu, models, Screen Scaling, fullscreen, changing the renderer while the game runs, quitting), as screenshots the
game takes itself and pictures of the screen (screencapture), in <out>/scenes/<scene>. hardware: what only a Mac with
a display can show and a log can tell, for a Mac nobody sits at: the display sync on and off, fullscreen and back,
another video mode and back, every change of renderer the game allows while it runs, and how long a replay takes with
each renderer. It takes no pictures of the screen and presses no keys, so it needs none of the system's permissions,
and its games present even while the screen is locked (-mtlunseen).

--idle SECONDS starts each game only once nobody has touched the Mac's keyboard or mouse for that long, and waits
otherwise: the game opens a window on the Mac's screen.

Every run gets a fresh home directory and MTL_DEBUG_LAYER=1 and MTL_SHADER_VALIDATION=1, and its output goes to
<out>/logs. A run fails if the game doesn't exit with 0 in time, if the log doesn't say that the Metal backend with
both kinds of validation is in use, if a frame is missing, if the presenter didn't present the frames it was given
(all but a few: the window is expected to be on screen, or the game to have -mtlunseen), or if the log has a
validation message, a "not implemented" line, or an error of the driver's. With -mtlcheck among the game's arguments
(some scenes have it; a sweep takes it after "--"), the presenter also reads drawables back and compares them with
the frames it placed in them, and a run fails if one differs. The exit status is 1 if any run failed;
<out>/summary.json has the details. Standard library only, so that it runs on a macOS runner as it is. See
tests/README.md, "Metal frames". test_capture.py checks the judging against made-up logs.
"""
import argparse
import json
import os
import re
import shutil
import struct
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
    (re.compile(r"presenter: drawable check \d+ FAILED"), "the drawable isn't the frame"),
    (re.compile(r"(UWP D3D12|VibeRant \w+): .*(failed|error|couldn't|can't place|removed|gave no drawable)", re.I), "driver error"),
    # The driver waits half a second for the GPU and then goes on without it: a screenshot whose readback it gave up
    # on is whatever its memory held
    (re.compile(r"(UWP D3D12|VibeRant \w+): .*gave up waiting", re.I), "the GPU fell behind"),
    (re.compile(r"I_Error|SIGSEGV|SIGABRT|signal: |An error occurred within", re.I), "crash"),
    (re.compile(r"couldn't start \(the log says why\)"), "fell back to Software"),
]
# Lines that match the above and are fine
ALLOWED = [
    re.compile(r"frame \d+ done: 0 errors"),
]
DEVICE_LINE = re.compile(r"Metal: device: .*")
# The presenter's last word on a window it presented on: given back to Software, or closed with the game
PRESENTER_DONE = r"presenter: (?:gave the game's OpenGL window back|closed the Metal window as the game quits; the game's OpenGL window is \w+)"
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


def screen_locked():
    """Whether the Mac's screen is locked (None if the system doesn't say): windows aren't shown then, the system
    takes no frames from them, and it makes no fullscreen Space"""
    try:
        text = subprocess.run(["ioreg", "-n", "Root", "-d1"], capture_output=True, text=True, timeout=20).stdout
    except (OSError, subprocess.TimeoutExpired):
        return None
    found = re.search(r'"CGSSessionScreenIsLocked"\s*=\s*(\w+)', text)
    if found:
        return found.group(1) == "Yes"
    # The session's record is there and has no such entry while the screen is unlocked
    return False if "IOConsoleUsers" in text else None


def presenting(text):
    """What the presenter says became of the frames it was given, over every window it took: presented, not presented
    because the window was out of sight, and not presented for want of a drawable"""
    found = re.findall(PRESENTER_DONE + r"; (\d+) frames presented, (\d+) skipped while the window was out of sight, (\d+) without a drawable", text)
    return tuple(sum(int(n[i]) for n in found) for i in range(3))


# A window that was only just made doesn't count as visible for its first frames, and a run has a window or a few
FEW_FRAMES = 20


def check_presenting(text, problems, in_sight=True):
    """A run that presents nothing fails. in_sight: the game's window is expected to be on screen (or the game has
    -mtlunseen), so that all but a few frames, or all but a twentieth, must have been presented too. Returns how many
    were."""
    presented, unseen, no_drawable = presenting(text)
    given = presented + unseen + no_drawable
    if presented == 0:
        problems.append(("present", "the log doesn't say that any frame was presented"))
    elif in_sight and unseen + no_drawable > max(FEW_FRAMES, given // 20):
        problems.append(("present", "%d of %d frames weren't presented: %d while the system called the window out of sight, %d for "
                         "want of a drawable. Behind a locked screen, or with several games' windows on top of each other, "
                         "give the game -mtlunseen (after --)" % (unseen + no_drawable, given, unseen, no_drawable)))
    return presented


# With -mtlcheck the presenter reads a window's first three drawables back, and every sixtieth after
FEW_CHECKS = 3


def check_drawables(text, problems):
    """A run with -mtlcheck: the presenter read drawables back and compared each with the frame it had placed in it.
    Returns how many it compared."""
    found = re.findall(r"presenter: (\d+) drawables were read back and compared with the frames placed in them \(-mtlcheck\): "
                       r"(\d+) differed, (\d+) unread", text)
    tried, differed, unread = (sum(int(n[i]) for n in found) for i in range(3))
    if differed:
        problems.append(("drawable", "%d of %d drawables read back didn't hold the frame placed in them" % (differed, tried - unread)))
    if tried - unread < FEW_CHECKS:
        problems.append(("drawable", "%d drawables were read back and compared with their frames, of %d tried: fewer than %d"
                         % (tried - unread, tried, FEW_CHECKS)))
    return tried - unread


def drawable_waits(text):
    """How long the layer took to give a drawable, in ms: the mean of each window's mean, and the worst"""
    found = re.findall(r"the layer took ([\d.]+) ms to give a drawable on average, ([\d.]+) ms at worst", text)
    if not found:
        return None
    return round(sum(float(mean) for mean, _ in found) / len(found), 3), max(float(worst) for _, worst in found)


def spaces(text):
    """The presenter's lines about windows going into and out of fullscreen Spaces, in order"""
    return re.findall(r"presenter: (at \d+ ms the (?:Metal|OpenGL) window [^(\n]*?)(?: \(.*)?$", text, re.M)


def probes(text):
    """Of the frames presented to a window the system called out of sight: how many, how many of them Metal reported
    with a time at which they were shown, and how many without (dropped, or a system that gives no times)"""
    found = re.findall(r"(\d+) of those presented went to a window the system called out of sight \((\d+) shown, (\d+) dropped\)", text)
    return tuple(sum(int(n[i]) for n in found) for i in range(3))


BLIND_TICS = 350  # ten seconds of game


def check_blind(text, problems):
    """A run with -mtlblind, where the presenter acts as if the system called its window out of sight: the picture
    must keep going all the same, a frame presented whenever the system is done with the one before, at ten frames a
    second or more, and the layer must never be without a drawable. (Behind a locked screen the system takes the
    frames too, at a pace of its own, and shows them to nobody.) Returns what to say of it."""
    count, timed, untimed = probes(text)
    waited = sum(int(n) for n in re.findall(r"(\d+) without a drawable", text))
    if waited:
        problems.append(("blind", "the layer had no drawable for %d frames" % waited))
    wanted = 10 * BLIND_TICS // 35
    if count < wanted:
        problems.append(("blind", "%d frames were presented to a window called out of sight: at least %d should have been in %d s"
                         % (count, wanted, BLIND_TICS // 35)))
    return "%s%d frames presented blind (%d reported with a time shown, %d without)" % (
        "the screen is locked: " if screen_locked() else "", count, timed, untimed)


def wait_until_idle():
    said = False
    while IDLE_SECONDS and idle_seconds() < IDLE_SECONDS:
        if not said:
            print("someone is at the Mac: waiting until it has been idle for %d s" % IDLE_SECONDS, flush=True)
            said = True
        time.sleep(20)


def run_game(game, data, home, args, log_path, timeout, during=None, validation=True, fresh_home=True, more_env=None):
    """One run of the game. during(process) runs on another thread while the game does. Returns (exit code or None on a
    timeout, seconds)."""
    wait_until_idle()
    if fresh_home:
        shutil.rmtree(home, ignore_errors=True)
    os.makedirs(home, exist_ok=True)
    env = dict(os.environ)
    env["RINGRACERSWADDIR"] = data
    if validation:
        env["MTL_DEBUG_LAYER"] = "1"
        env["MTL_SHADER_VALIDATION"] = "1"
    env.update(more_env or {})
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


def check_log(log_path, need_metal=True, validation=True, quits_with_metal=True):
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
        # The layer must have its window's color space, as the OpenGL window's content has, or the two renderers'
        # colors differ on a display that isn't sRGB
        if "presenter: " in text and "presenter: colors are matched to the display" not in text:
            problems.append(("colors", "the log doesn't say that the Metal layer has its window's color space"))
        # A game that ends with VibeRant Metal closes the Metal window and leaves its OpenGL window, which nothing ever
        # drew into, hidden: the log says what SDL says of that window at the end
        quit_with = re.findall(r"presenter: closed the Metal window as the game quits; the game's OpenGL window is (\w+)", text)
        if not quits_with_metal:
            pass
        elif not quit_with:
            problems.append(("quit", "the log doesn't say that the Metal window was closed as the game quit"))
        elif quit_with[-1] != "hidden":
            problems.append(("quit", "the game's OpenGL window was %s as the game quit from VibeRant Metal" % quit_with[-1]))
    return problems, (device.group(0) if device else ""), text


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
        problems, device, text = check_log(log_path)
        # The frames themselves are read back from the driver's screen, whatever the window got. So the sweep also
        # holds the presenter to its word that it presented them, and with -mtlcheck to what it found in its drawables.
        presented = check_presenting(text, problems)
        checked = check_drawables(text, problems) if "-mtlcheck" in options.extra else None
        shots = [shot for shot in pngs_under(home) if os.path.basename(os.path.dirname(shot)) == "framecapture"]
        for shot in shots:
            shutil.move(shot, os.path.join(frames_dir, os.path.basename(shot)))
        # A replay shorter than the last tics ends the run with fewer frames, which the log says
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
                  "presented": presented, "not_presented": sum(presenting(text)[1:]), "drawables_checked": checked,
                  "device": device, "problems": problems}
        results.append(result)
        print("%s: %d/%d frames, exit %s, %.0f s, %d frames presented%s%s" % (ghost, len(shots), len(tics), code, seconds, presented,
              "" if checked is None else ", %d drawables checked" % checked,
              "" if not problems else "  PROBLEMS: " + "; ".join("%s: %s" % p for p in problems[:5])), flush=True)
    shutil.rmtree(home, ignore_errors=True)
    return results


def screencapture(path, region=None):
    """A picture of the screen, or of a rectangle of it ("x,y,w,h")"""
    try:
        subprocess.run(["screencapture", "-x"] + (["-R", region] if region else []) + [path], timeout=20, check=False)
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


# A rectangle in the middle of a runner's 1024x768 screen, where the game's window is and the menu bar's clock isn't
MIDDLE = "212,234,600,300"


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
            for pattern, nth, seconds, name, *region in marks:
                while True:
                    with open(log_path, errors="replace") as written:
                        if len(re.findall(pattern, written.read())) >= nth:
                            break
                    if process.poll() is not None:
                        return
                    time.sleep(0.3)
                time.sleep(seconds)
                if process.poll() is None:
                    screencapture(os.path.join(scene_dir, "screen-%s.png" % name), *region)
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

    def menu_quit(process, scene_dir, log_path):
        # The same keys up to Exit Program's question, and then A, which answers it with yes: the game must end by
        # that, since nothing else ends it (its command line only waits, for longer than the run may take)
        time.sleep(25)
        for index, (key, name) in enumerate([(KEY_RETURN, "warning"), (KEY_A, "menu"), (KEY_A, "question")]):
            keys([key], pause=3)
            screencapture(os.path.join(scene_dir, "screen-%d-%s.png" % (index + 1, name)))
        # On a slow runner a key can come before the game takes keys and be lost. A is the only way forward from each
        # of these screens, and yes at the last, so it is pressed until the game has gone.
        for _ in range(8):
            if process.poll() is not None:
                break
            keys([KEY_A], pause=3)

    def scaling(mode):
        # A 320x200 frame on the whole screen, so that Fit, Integer, and 1:1 put it in three different rectangles. The
        # game starts in fullscreen, as it does for a player (-win, which the other scenes have, keeps it in a window).
        return {"args": ["+scr_scaling", mode] + replay + wait(900) + shot + ["+quit"],
                "during": pictures((playing, 1, 9, "fullscreen")), "shots": 1, "size": (320, 200), "window": False, "check": True}

    return {
        # The game as a player starts it: no renderer named, so the default has to be VibeRant Metal
        "default-title": {"args": wait(700) + shot + ["+quit"], "during": pictures((took, 1, 9, "intro")), "shots": 1,
                          "renderer": None, "check": True},
        "title-menu": {"args": wait(2100) + shot + ["+quit"], "during": menu, "shots": 1},
        # Quitting as a player does, through the menu. 20,000 tics are nearly ten minutes, and the run gets two.
        "menu-quit": {"args": wait(20000) + ["+quit"], "during": menu_quit, "shots": 0, "timeout": 120},
        "models-on": {"args": race + ["+gr_models", "on"], "shots": 2},
        "models-off": {"args": race + ["+gr_models", "off"], "shots": 2},
        "switch-renderer": {"args": replay + wait(600) + shot + ["+renderer", "Software"] + wait(400) + shot
                            + ["+renderer", "VibeRant Metal"] + wait(400) + shot + ["+renderer", "Software"] + wait(100)
                            + ["+renderer", "VibeRant Metal"] + wait(300) + shot + ["+quit"],
                            "during": pictures((playing, 1, 6, "1-metal"), (gave_back, 1, 3, "2-software"), (took, 2, 3, "3-metal")),
                            "shots": 4, "check": True},
        "software-start": {"args": replay + wait(600) + shot + ["+renderer", "VibeRant Metal"] + wait(400) + shot + ["+quit"],
                           "during": pictures((playing, 1, 3, "1-software-start"), (playing, 1, 5, "2-software"),
                                              (took, 1, 3, "3-metal")), "shots": 2,
                           "renderer": "-software"},
        # The renderer changed in fullscreen, which is how the game runs for a player: two windows trade places, each
        # in a Space of its own, and the log says in what order they went (spaces, in summary.json)
        "fullscreen-switch": {"args": replay + wait(400) + shot + ["+renderer", "Software"] + wait(400) + shot
                              + ["+renderer", "VibeRant Metal"] + wait(400) + shot + ["+quit"],
                              "during": pictures((playing, 1, 7, "1-metal"), (gave_back, 1, 6, "2-software"), (took, 2, 6, "3-metal")),
                              "shots": 3, "window": False},
        "fullscreen-switch-software": {"args": replay + wait(400) + shot + ["+renderer", "VibeRant Metal"] + wait(400) + shot
                                       + ["+renderer", "Software"] + wait(400) + shot + ["+quit"],
                                       "during": pictures((playing, 1, 7, "1-software"), (took, 1, 6, "2-metal"),
                                                          (gave_back, 1, 6, "3-software")),
                                       "shots": 3, "window": False, "renderer": "-software", "ends_in_software": True},
        "scaling-fit": scaling("Fit"),
        "scaling-integer": scaling("Integer"),
        "scaling-1to1": scaling("1:1"),
        "vsync": {"args": ["+vid_wait", "on"] + replay + wait(700) + shot + ["+quit"],
                  "during": pictures((playing, 1, 8, "race")), "shots": 1},
        # The system calls the window out of sight and is wrong (-mtlblind acts it out): the picture must keep going
        "blind": {"args": ["-mtlblind", "+vid_wait", "on"] + replay + wait(BLIND_TICS) + shot + ["+quit"],
                  "during": pictures((playing, 1, 5, "race-1", MIDDLE), (playing, 1, 2, "race-2", MIDDLE), (playing, 1, 1, "race")),
                  "shots": 1, "blind": True, "moving": ("race-1", "race-2")},
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
        args = (game_args(width, height, renderer, scene.get("window", True)) + (["-mtlcheck"] if scene.get("check") else [])
                + scene["args"] + options.extra)
        during = scene.get("during")
        wrapped = (lambda process, d=during, s=scene_dir, l=log_path: d(process, s, l)) if during else None
        timeout = scene.get("timeout", options.timeout)
        code, seconds = run_game(options.game, options.data, home, args, log_path, timeout, wrapped)
        problems, device, text = check_log(log_path, quits_with_metal=not scene.get("ends_in_software"))
        # A scene's window is on the screen the whole time, except where the scene has the presenter think otherwise
        presented = check_presenting(text, problems, in_sight=not scene.get("blind"))
        checked = check_drawables(text, problems) if scene.get("check") else None
        shots = pngs_under(home)
        for index, shot in enumerate(shots):
            shutil.move(shot, os.path.join(scene_dir, "game-%d-%s" % (index + 1, os.path.basename(shot))))
        if code is None:
            problems.append(("timeout", "the game didn't exit in %d s" % timeout))
        elif code != 0:
            problems.append(("exit", "the game exited with %s" % code))
        if len(shots) != scene["shots"]:
            problems.append(("capture", "%d screenshots of the game's own, expected %d" % (len(shots), scene["shots"])))
        blind = check_blind(text, problems) if scene.get("blind") else ""
        if scene.get("moving"):
            # Two pictures of the middle of the screen, seconds apart while the replay plays: a picture that stands
            # still is the same file twice
            first, second = (os.path.join(scene_dir, "screen-%s.png" % name) for name in scene["moving"])
            try:
                with open(first, "rb") as one, open(second, "rb") as other:
                    if one.read() == other.read():
                        problems.append(("frozen", "the screen showed the same picture %s and %s" % scene["moving"]))
            except OSError:
                problems.append(("frozen", "the pictures of the screen weren't taken"))
        results.append({"scene": name, "exit": code, "seconds": round(seconds, 1), "screenshots": len(shots), "presented": presented,
                        "not_presented": sum(presenting(text)[1:]), "drawables_checked": checked, "blind": blind, "device": device,
                        "spaces": spaces(text), "problems": problems})
        print("%s: exit %s, %.0f s, %d screenshots, %d frames presented%s%s%s" % (name, code, seconds, len(shots), presented,
              "" if checked is None else ", %d drawables checked" % checked, ", " + blind if blind else "",
              "" if not problems else "  PROBLEMS: " + "; ".join("%s: %s" % p for p in problems[:5])), flush=True)
    shutil.rmtree(home, ignore_errors=True)
    return results


def png_size(path):
    with open(path, "rb") as handle:
        return struct.unpack(">II", handle.read(24)[16:24])


def hardware_list():
    """name -> a list of runs of the game, one after another in one home directory. A run: the renderer's argument
    ("renderer", None for the one the config names), whether the game starts in a window ("window"), its arguments
    ("args"), whether Metal's validation is on ("validation", off where a time is measured), how many screenshots it
    must write ("shots") and their sizes in order ("sizes"), the sizes of the presenter's drawables that the log
    must name in order ("drawables", None for the display's own, whatever it is), text the log must have ("needs")
    and must not have ("lacks"), whether the time per frame between the two frames of -shottics is worked out
    ("timed"), whether the game presents even to a window nobody can see ("unseen", -mtlunseen, unless turned off),
    whether the presenter reads drawables back and compares them with their frames ("check", -mtlcheck), whether it
    is made to take its window for out of sight ("blind"), and more of the environment ("env")."""
    ghost = "RR_AERIALHIGHLANDS-battlekukku-FerronNuff"
    replay = ["-shotghost", ghost, "-shottics", "99999", "+framecapture_ghost", ghost]
    # The capture mode draws one frame per tic of game logic as fast as it can, so the time from one level time to
    # another is the time those frames took
    timed = ["-shotghost", ghost, "-shottics", "350,1400", "+fpscap", "-1"]
    took = "presenter: presenting on a Metal window"
    gave_back = "presenter: gave the game's OpenGL window back"

    def wait(tics):
        return ["+wait", str(tics)]

    shot = ["+screenshot"] + wait(5)

    def speed(renderer, vsync, unseen=True):
        return [{"renderer": renderer, "args": timed + ["+vid_wait", vsync], "validation": False, "shots": 2, "timed": True,
                 "unseen": unseen, "needs": [took] if renderer == "-metal" else [], "lacks": [] if renderer == "-metal" else [took]}]

    def fullscreen(env, space=False):
        # Started as a player starts it, on the whole screen; then a window through the game's own setting, and back
        return [{"renderer": "-metal", "window": False, "env": env, "space": space,
                 "args": replay + wait(300) + shot + ["+fullscreen", "No"] + wait(300) + shot + ["+fullscreen", "Yes"]
                 + wait(300) + shot + ["+quit"],
                 "shots": 3, "sizes": [(1280, 720)] * 3, "drawables": [None, (1280, 720), None], "check": True}]

    return {
        # How long a frame of the replay takes with each renderer, with vid_wait off. VibeRant Metal as the game runs
        # on this Mac: presenting if its window can be seen, and only drawing if the screen is locked or asleep.
        "speed-metal": speed("-metal", "off", unseen=False),
        "speed-software": speed("-software", "off"),
        "speed-legacy-gl": speed("-opengl", "off"),
        # vid_wait is the layer's display sync: on, a frame takes a refresh of the display; off, as long as it takes.
        # (To a window nobody can see, the system hands drawables back at a pace of its own, either way.)
        "vsync-on": speed("-metal", "on"),
        "vsync-off": speed("-metal", "off"),
        # The game's fullscreen is a Space of its own, which the system doesn't make while the screen is locked: there
        # the game stays in its window, and this scene says so. Without Spaces (SDL's hint) the window covers the screen
        # as it is, which works either way.
        # The game's main loop has no autorelease pool, so the driver and the presenter bring one to every call that can
        # autorelease; an object autoreleased without one stays for good. The Objective-C runtime reports each such
        # object, by thread, and the driver's thread must have none. (Metal's own threads report several per presented
        # frame, which is the system's business: the game's memory doesn't grow by it.)
        "autorelease": [{"renderer": "-metal", "args": timed + ["+vid_wait", "off"], "validation": False, "shots": 2,
                         "env": {"OBJC_DEBUG_MISSING_POOLS": "YES"}, "pools": True, "needs": [took]}],
        # The system calls the window out of sight and is wrong, which -mtlblind acts out: a frame is presented whenever
        # the system is done with the one before, so the picture keeps going. (Behind a locked screen the system takes
        # the frames too, and shows them to nobody.)
        "blind": [{"renderer": "-metal", "args": ["-mtlblind", "+vid_wait", "on"] + replay + wait(BLIND_TICS) + shot + ["+quit"],
                   "shots": 1, "unseen": False, "blind": True, "needs": [took]}],
        "fullscreen": fullscreen({}, space=True),
        "fullscreen-no-space": fullscreen({"SDL_VIDEO_MAC_FULLSCREEN_SPACES": "0"}),
        # The renderer changed in fullscreen: two windows trade places, each in a Space of its own, and the log says in
        # what order they went. Without -mtlunseen, so that a Metal window left on a Space that isn't showing is found
        # out by the frames it doesn't get.
        "fullscreen-switch": [{"renderer": "-metal", "window": False, "space": True, "unseen": False,
                               "args": replay + wait(400) + shot + ["+renderer", "Software"] + wait(400) + shot
                               + ["+renderer", "VibeRant Metal"] + wait(400) + shot + ["+quit"], "shots": 3,
                               "drawables": [None, None], "needs": ["(?s)%s.*%s.*%s" % (took, gave_back, took)]}],
        "fullscreen-switch-software": [{"renderer": "-software", "window": False, "space": True, "unseen": False,
                                        "args": replay + wait(400) + shot + ["+renderer", "VibeRant Metal"] + wait(400) + shot
                                        + ["+renderer", "Software"] + wait(400) + shot + ["+quit"], "shots": 3,
                                        "drawables": [None], "needs": ["(?s)%s.*%s" % (took, gave_back)]}],
        # vid_mode 14 is 800x600 and 10 is 1280x720 in a window (windowedModes in i_video.cpp)
        "video-mode": [{"renderer": "-metal", "args": replay + wait(300) + shot + ["+vid_mode", "14"] + wait(300) + shot
                        + ["+vid_mode", "10"] + wait(300) + shot + ["+quit"],
                        "shots": 3, "sizes": [(1280, 720), (800, 600), (1280, 720)],
                        "drawables": [(1280, 720), (800, 600), (1280, 720)], "check": True}],
        "metal-software-metal": [{"renderer": "-metal", "args": replay + wait(300) + shot + ["+renderer", "Software"] + wait(300) + shot
                                  + ["+renderer", "VibeRant Metal"] + wait(300) + shot + ["+quit"], "shots": 3, "check": True,
                                  "needs": [took, gave_back, "(?s)%s.*%s.*%s" % (took, gave_back, took)]}],
        "software-metal-software": [{"renderer": "-software", "args": replay + wait(300) + shot + ["+renderer", "VibeRant Metal"]
                                     + wait(300) + shot + ["+renderer", "Software"] + wait(300) + shot + ["+quit"], "shots": 3,
                                     "needs": ["(?s)%s.*%s" % (took, gave_back)]}],
        "legacy-gl-software-legacy-gl": [{"renderer": "-opengl", "args": replay + wait(300) + shot + ["+renderer", "Software"] + wait(300)
                                          + shot + ["+renderer", "Legacy GL"] + wait(300) + shot + ["+quit"], "shots": 3,
                                          "lacks": [took]}],
        # Between the two hardware renderers the setting takes effect at the next start: the first game goes on with
        # VibeRant Metal and saves the setting, and the second, started without naming a renderer, is Legacy GL's
        "metal-then-legacy-gl": [
            {"renderer": "-metal", "args": replay + wait(300) + ["+renderer", "Legacy GL"] + wait(300) + shot + ["+quit"], "shots": 1,
             "needs": [took], "lacks": [gave_back + "(?s:.*)" + took]},
            {"renderer": None, "args": replay + wait(300) + shot + ["+renderer", "VibeRant Metal"] + wait(300) + shot + ["+quit"],
             "shots": 2, "lacks": [took], "metal": False}],
    }


def hardware(options):
    scenes_dir = os.path.join(options.out, "hardware")
    logs_dir = os.path.join(options.out, "logs")
    os.makedirs(logs_dir, exist_ok=True)
    home = os.path.join(options.out, "home")
    results = []
    for name, runs in hardware_list().items():
        if options.only and name not in options.only.split(","):
            continue
        # The game's fullscreen is a Space of its own, which the system doesn't make while the screen is locked: the game
        # stays in its window there, so a scene that is about Spaces has nothing to show
        if any(run.get("space") for run in runs) and screen_locked():
            results.append({"scene": name, "skipped": "the screen is locked, and the system makes no fullscreen Space behind a locked screen",
                            "problems": []})
            print("%s: not run: %s" % (name, results[-1]["skipped"]), flush=True)
            continue
        scene_dir = os.path.join(scenes_dir, name)
        os.makedirs(scene_dir, exist_ok=True)
        for index, run in enumerate(runs):
            log_path = os.path.join(logs_dir, "hardware-%s%s.log" % (name, "-%d" % (index + 1) if len(runs) > 1 else ""))
            renderer = run.get("renderer")
            validation = run.get("validation", True)
            metal = run.get("metal", renderer == "-metal")
            args = (game_args(1280, 720, renderer, run.get("window", True)) + (["-mtlunseen"] if run.get("unseen", True) else [])
                    + (["-mtlcheck"] if run.get("check") else [])
                    + run["args"] + options.extra)
            code, seconds = run_game(options.game, options.data, home, args, log_path, options.timeout, validation=validation,
                                     fresh_home=index == 0, more_env=run.get("env"))
            problems, device, text = check_log(log_path, need_metal=metal, validation=validation)
            presented = presenting(text)[0]
            if metal or presented:
                # With -mtlunseen every frame is presented wherever the window is. Without it, the game is as a
                # player has it: presenting on a screen that can be seen, and not behind a locked one.
                in_sight = not run.get("blind") and (run.get("unseen", True) or screen_locked() is False)
                check_presenting(text, problems, in_sight=in_sight)
            checked = check_drawables(text, problems) if run.get("check") else None
            shots = sorted(pngs_under(home), key=os.path.getmtime)
            result = {"scene": name, "run": index + 1, "exit": code, "seconds": round(seconds, 1), "screenshots": len(shots),
                      "presented": presented, "drawables_checked": checked, "device": device, "problems": problems}
            result["timed"] = bool(run.get("timed"))
            if run.get("timed") and len(shots) == 2:
                # One frame per tic between the two level times
                result["ms_per_frame"] = round((os.path.getmtime(shots[1]) - os.path.getmtime(shots[0])) * 1000 / (1400 - 350), 3)
            sizes = [png_size(shot) for shot in shots]
            for number, shot in enumerate(shots):
                shutil.move(shot, os.path.join(scene_dir, "run%d-%d-%s" % (index + 1, number + 1, os.path.basename(shot))))
            drawables = [(int(w), int(h)) for w, h in re.findall(r"presenter: (\d+)x(\d+) drawables", text)]
            # The game begins at 320x200 before it takes the size it was started with
            drawables = [size for size in drawables if size != (320, 200)]
            result["drawables"] = drawables
            scale = re.findall(r"presenter: \d+x\d+ drawables.*", text)
            result["display"] = scale[-1] if scale else ""
            if code is None:
                problems.append(("timeout", "the game didn't exit in %d s" % options.timeout))
            elif code != 0:
                problems.append(("exit", "the game exited with %s" % code))
            if len(shots) != run["shots"]:
                problems.append(("capture", "%d screenshots, expected %d" % (len(shots), run["shots"])))
            elif "sizes" in run and sizes != [tuple(size) for size in run["sizes"]]:
                problems.append(("capture", "screenshots of %s, expected %s" % (sizes, run["sizes"])))
            if "drawables" in run:
                wanted = run["drawables"]
                if len(drawables) != len(wanted) or any(w is not None and w != d for w, d in zip(wanted, drawables)):
                    problems.append(("present", "drawables of %s, expected %s (None: the display's size)" % (drawables, wanted)))
                elif any(w is None and d == (1280, 720) for w, d in zip(wanted, drawables)):
                    problems.append(("present", "the fullscreen drawables are the window's size: %s" % drawables))
            for pattern in run.get("needs", []):
                if not re.search(pattern, text):
                    problems.append(("log", "the log lacks: " + pattern))
            for pattern in run.get("lacks", []):
                if re.search(pattern, text):
                    problems.append(("log", "the log has: " + pattern))
            if run.get("pools"):
                thread = re.search(r"Metal: the driver's thread is (0x[0-9a-f]+)", text)
                reports = re.findall(r"MISSING POOLS: \((0x[0-9a-f]+)\) Object 0x[0-9a-f]+ of class (\w+)", text)
                if not thread:
                    problems.append(("pools", "the log doesn't say which thread is the driver's"))
                else:
                    mine = [name for who, name in reports if who == thread.group(1)]
                    result["autoreleased_without_pool"] = {"driver_thread": len(mine), "other_threads": len(reports) - len(mine)}
                    if mine:
                        problems.append(("pools", "the driver's thread autoreleased %d objects with no pool in place: %s" % (
                            len(mine), ", ".join("%d %s" % (mine.count(name), name) for name in sorted(set(mine))[:6]))))
            result["spaces"] = spaces(text)
            if metal:
                result["unseen_frames"] = sum(int(n) for n in re.findall(r"(\d+) skipped while the window was out of sight", text))
                result["probes"] = dict(zip(("presented", "shown", "dropped"), probes(text)))
                result["drawable_wait_ms"] = drawable_waits(text)
            if run.get("blind"):
                result["blind"] = check_blind(text, problems)
            results.append(result)
            print("%s%s: exit %s, %.0f s, %d screenshots, %d frames presented%s%s%s%s%s%s%s%s" % (
                name, " (run %d)" % (index + 1) if len(runs) > 1 else "", code, seconds, len(shots), presented,
                "" if checked is None else ", %d drawables checked" % checked,
                ", %d not (the window was out of sight)" % result["unseen_frames"] if result.get("unseen_frames") else "",
                ", " + result["blind"] if result.get("blind") else "",
                ", %.2f ms per frame" % result["ms_per_frame"] if "ms_per_frame" in result else "",
                ", a drawable after %.2f ms (worst %.1f)" % result["drawable_wait_ms"] if result.get("timed") and result.get("drawable_wait_ms") else "",
                ", autoreleased without a pool: %(driver_thread)d on the driver's thread, %(other_threads)d on others"
                % result["autoreleased_without_pool"] if "autoreleased_without_pool" in result else "",
                ", drawables %s" % drawables if drawables else "",
                "" if not problems else "  PROBLEMS: " + "; ".join("%s: %s" % p for p in problems[:5])), flush=True)
    shutil.rmtree(home, ignore_errors=True)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mode", choices=["sweep", "scenes", "hardware"])
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
    results = {"sweep": sweep, "scenes": scenes, "hardware": hardware}[options.mode](options)
    failed = [r for r in results if r["problems"]]
    skipped = [r for r in results if r.get("skipped")]
    with open(os.path.join(options.out, "summary.json"), "w") as handle:
        json.dump({"mode": options.mode, "runs": results}, handle, indent=1)
    print("%d runs, %d with problems%s" % (len(results) - len(skipped), len(failed), ", %d not run" % len(skipped) if skipped else ""))
    return 1 if failed or not results else 0


if __name__ == "__main__":
    sys.exit(main())
