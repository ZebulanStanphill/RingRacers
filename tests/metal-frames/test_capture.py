#!/usr/bin/env python3
"""Checks how capture.py judges a run's log, with logs made up here from the presenter's own lines: what must pass,
and, for each thing a run can fail for, a log that must fail for it and nothing else. Runs anywhere (ctest runs it as
metal_capture_judging); it starts no game."""
import os
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capture  # noqa: E402

DEVICE = ("UWP: VibeRant backend: Metal\n"
          "Metal: device: Apple M4 Pro, Metal (MTLDebugDevice > MTLGPUDebugDevice > AGXG16SDevice), unified memory, depth-stencil D32FS8, "
          "working set 18186 MB, API validation on, shader validation on\n")
TOOK = "VibeRant Metal: presenter: presenting on a Metal window in place of the game's OpenGL window (waits for the display)\n"
COLORS = ('VibeRant Metal: presenter: colors are matched to the display as the OpenGL window\'s are: the layer has its window\'s '
          'color space, "sRGB IEC61966-2.1" (the system gave it), and the display\'s is "Color LCD"\n')


def done(presented, unseen=0, no_drawable=0, probes=(0, 0, 0), how="closed the Metal window as the game quits; the game's OpenGL window is hidden"):
    return ("VibeRant Metal: presenter: %s; %d frames presented, %d skipped while the window was out of sight, %d without a drawable; "
            "%d of those presented went to a window the system called out of sight (%d shown, %d dropped); "
            "the layer took 0.41 ms to give a drawable on average, 8.3 ms at worst; Metal reported 5 drawables with a time shown and 7 without, 12 of its reports within 250 ms and 0 later\n"
            % ((how, presented, unseen, no_drawable) + probes))


GAVE_BACK = "gave the game's OpenGL window back"


def judge(text, **kwargs):
    """The kinds of problem check_log finds in a log"""
    with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as handle:
        handle.write(text)
    try:
        problems, _, read = capture.check_log(handle.name, **kwargs)
    finally:
        os.unlink(handle.name)
    assert read == text
    return sorted(set(kind for kind, _ in problems))


def presenting(text, **kwargs):
    problems = []
    presented = capture.check_presenting(text, problems, **kwargs)
    return presented, [kind for kind, _ in problems]


def drawables(text):
    problems = []
    checked = capture.check_drawables(text, problems)
    return checked, [kind for kind, _ in problems]


def main():
    good = DEVICE + TOOK + COLORS + done(2049, 3)
    assert judge(good) == []

    # The log itself
    assert judge(TOOK + COLORS + done(2049)) == ["no Metal"]
    assert judge(good.replace("API validation on", "API validation off")) == ["no validation"]
    assert judge(good.replace("API validation on", "API validation off"), validation=False) == []
    assert judge(good + "Metal: CreateQueryHeap is not implemented by mtl12\n") == ["not implemented"]
    assert judge(good + "VibeRant Metal: gave up waiting for ReadRect after 500.0 ms (fence 12, GPU at 11); 0 stall warnings suppressed\n") == ["the GPU fell behind"]
    assert judge(good + "VibeRant Metal: frame 12 done: 0 errors\n") == []
    # A game that ends in Software says nothing of Metal's windows at the end, and isn't asked to
    assert judge("I_StartupGraphics...\n", need_metal=False) == []

    # Quitting: the OpenGL window, which nothing drew into, must still be hidden
    assert judge(DEVICE + TOOK + COLORS + done(2049, how="closed the Metal window as the game quits; the game's OpenGL window is shown")) == ["quit"]
    assert judge(DEVICE + TOOK + COLORS + done(2049, how=GAVE_BACK)) == ["quit"]
    assert judge(DEVICE + TOOK + COLORS + done(300, how=GAVE_BACK) + TOOK + COLORS + done(400)) == []
    # A scene that ends in Software after VibeRant Metal had the window
    assert judge(DEVICE + TOOK + COLORS + done(2049, how=GAVE_BACK), quits_with_metal=False) == []
    # Colors: the layer must have its window's color space
    assert judge(DEVICE + TOOK + done(2049)) == ["colors"]
    assert judge(DEVICE + TOOK + 'VibeRant Metal: presenter: the window has no color space to give the layer, so the frame\'s colors '
                 'reach the display ("Color LCD") as they are\n' + done(2049)) == ["colors"]

    # Presenting. Nothing presented fails wherever the window is.
    assert presenting(done(0, 2049)) == (0, ["present"])
    assert presenting(done(0, 2049), in_sight=False) == (0, ["present"])
    assert presenting("") == (0, ["present"])
    # A window in sight: the few frames before the system calls a new window visible are fine, and so is a twentieth
    assert presenting(done(2049)) == (2049, [])
    assert presenting(done(2043, 6)) == (2043, [])
    assert presenting(done(295, 6)) == (295, [])
    assert presenting(done(40, 20)) == (40, [])
    assert presenting(done(1900, 100)) == (1900, [])
    assert presenting(done(1900, 101)) == (1900, ["present"])
    assert presenting(done(40, 21)) == (40, ["present"])
    # The whole point: a sweep behind a locked screen without -mtlunseen, or with a presenter that takes its window
    # for hidden, used to pass on its first frame alone
    assert presenting(done(1, 2048)) == (1, ["present"])
    assert presenting(done(5, 1482, probes=(5, 0, 5))) == (5, ["present"])
    # ... and a layer that gives no drawables counts like a window out of sight
    assert presenting(done(1000, 0, 1049)) == (1000, ["present"])
    # Where the window isn't expected to be seen, one frame presented is all that is asked
    assert presenting(done(5, 1482, probes=(5, 0, 5)), in_sight=False) == (5, [])
    # Every window of a run counts: Metal, Software, Metal again
    switched = done(300, 3, how=GAVE_BACK) + done(400, 2)
    assert capture.presenting(switched) == (700, 5, 0)
    assert presenting(switched) == (700, [])
    assert presenting(done(300, how=GAVE_BACK) + done(1, 400)) == (301, ["present"])

    # -mtlcheck: drawables read back and compared with their frames
    checked = "VibeRant Metal: presenter: %d drawables were read back and compared with the frames placed in them (-mtlcheck): %d differed, %d unread\n"
    assert drawables(checked % (37, 0, 0)) == (37, [])
    assert drawables(checked % (20, 0, 0) + checked % (17, 0, 0)) == (37, [])
    assert drawables(checked % (37, 1, 0)) == (37, ["drawable"])
    assert drawables(checked % (37, 0, 35)) == (2, ["drawable"])
    assert drawables(checked % (2, 0, 0)) == (2, ["drawable"])
    assert drawables("") == (0, ["drawable"])  # the game wasn't given -mtlcheck, or the presenter ignores it
    failed = ("VibeRant Metal: presenter: drawable check 4 FAILED: the 1280x720 frame is NOT what is in the 1280x720 drawable at 0,0 as "
              "1280x720 (at its own size; a block's mean color at most 97.20 levels off, 601203 of 921600 pixels differ, 0 outside aren't black)\n")
    passed = failed.replace(" FAILED", "").replace("is NOT what is in", "is in")
    assert judge(good + passed) == []
    assert judge(good + failed) == ["the drawable isn't the frame"]

    # -mtlblind: the presenter takes its window for out of sight, and must go on presenting, a frame whenever the
    # system is done with the one before: ten a second or more, whatever the reports say of their times
    def blind(text, locked):
        capture.screen_locked = lambda: locked
        problems = []
        said = capture.check_blind(text, problems)
        return said, [kind for kind, _ in problems]

    assert blind(done(601, 4000, probes=(601, 600, 1)), False) == ("601 frames presented blind (600 reported with a time shown, 1 without)", [])
    assert blind(done(601, 4000, probes=(601, 0, 601)), False)[1] == []
    assert blind(done(150, 4000, probes=(150, 150, 0)), False)[1] == []
    assert blind(done(149, 4000, probes=(149, 149, 0)), False)[1] == ["blind"]
    assert blind(done(9, 433, probes=(9, 0, 9)), False)[1] == ["blind"]
    assert blind(done(9, 433, probes=(9, 0, 9)), True)[1] == ["blind"]
    assert blind(done(1200, 300, probes=(1200, 0, 1200)), True) == ("the screen is locked: 1200 frames presented blind (0 reported with a time shown, 1200 without)", [])
    assert blind(done(601, 4000, 12, probes=(601, 600, 1)), False)[1] == ["blind"]
    assert capture.drawable_waits(done(300, how=GAVE_BACK) + done(400)) == (0.41, 8.3)
    assert capture.drawable_waits("") is None
    print("test_capture: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
