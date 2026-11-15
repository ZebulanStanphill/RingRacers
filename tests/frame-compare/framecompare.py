#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["numpy", "pillow"]
# ///
"""Plays the same replays in two builds or renderers of Ring Racers, takes screenshots at the same game tics, and
compares them pixel by pixel. See README.md.

  uv run tests/frame-compare/framecompare.py run RESULTS --side "a=native:BIN -software" --side "b=wine:rr.exe -d3d12"
  uv run tests/frame-compare/framecompare.py compare RESULTS a b
  uv run tests/frame-compare/framecompare.py list
"""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import compare  # noqa: E402
import replays as replay_sets  # noqa: E402
import runner  # noqa: E402
from sides import Side  # noqa: E402


def pick_replays(args):
    chosen = replay_sets.QUICK_REPLAYS if args.quick else replay_sets.DEFAULT_REPLAYS
    if args.replays:
        wanted = args.replays.split(",")
        by_name = {r["name"]: r for r in replay_sets.DEFAULT_REPLAYS}
        unknown = [w for w in wanted if w not in by_name]
        if unknown:
            sys.exit("unknown replays: %s (see the list command)" % ", ".join(unknown))
        chosen = [by_name[w] for w in wanted]
    chosen = [dict(r) for r in chosen]
    if args.tics:
        for r in chosen:
            r["tics"] = sorted({int(t) for t in args.tics.split(",")})
    return chosen


def cmd_run(args):
    sides = [Side.parse(s) for s in args.side]
    if len({s.name for s in sides}) != len(sides):
        sys.exit("side names must differ")
    if args.install_hypr_rule:
        runner.install_hypr_rule()
    failures = runner.run_all(sides, pick_replays(args), Path(args.results).resolve(),
                              (args.width, args.height), args.timeout, args.resume, args.keep_home,
                              not args.no_watchdog)
    if failures:
        print("%d runs did not finish cleanly (see run.log in their directories)" % failures)
    return 1 if failures else 0


def cmd_compare(args):
    results = Path(args.results).resolve()
    out_dir = results / ("compare-%s-vs-%s" % (args.a, args.b))
    rows = compare.compare_sides(results, args.a, args.b, args.threshold, out_dir)
    manifest = results / "manifest.json"
    sides_info = json.loads(manifest.read_text())["sides"] if manifest.exists() else []
    summary = compare.write_report(out_dir, results, args.a, args.b, args.threshold, rows,
                                   [s for s in sides_info if s["name"] in (args.a, args.b)])
    print("%s vs %s: %d frames, %d identical, %d different, %d missing, %d other size; worst %s of pixels, max %d" % (
        args.a, args.b, summary["frames"], summary["identical"], summary["different"], summary["missing"],
        summary["size_differs"], compare.pct(summary["worst_share"]), summary["worst_max_channel_diff"]))
    print("report: %s" % (out_dir / "index.html"))
    bad = summary["different"] + summary["missing"] + summary["size_differs"]
    return 1 if (bad and args.fail_on_diff) else 0


def cmd_list(args):
    for r in replay_sets.DEFAULT_REPLAYS:
        print("%-20s %-48s tics %s\n%24s%s" % (r["name"], r.get("ghost", r.get("args")),
                                              ",".join(map(str, r["tics"])), "", r["why"]))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("run", help="play the replays on each side and collect the screenshots")
    p.add_argument("results", help="results directory (RESULTS/<side>/<replay>/<MAP>_<tic>.png)")
    p.add_argument("--side", action="append", required=True, metavar="NAME=KIND:TARGET [ARGS]",
                   help='a side, e.g. "sw=native:/path/ringracers -software" or "d3d=wine:rr-d3d12.exe -d3d12" '
                        "(repeat for each side)")
    p.add_argument("--replays", help="comma-separated replay names (default: all; see the list command)")
    p.add_argument("--quick", action="store_true", help="only two replays, to try a setup out")
    p.add_argument("--tics", help="comma-separated level times, instead of each replay's own")
    p.add_argument("--width", type=int, default=1280)
    p.add_argument("--height", type=int, default=720)
    p.add_argument("--timeout", type=int, default=420, help="seconds per run before the game is killed")
    p.add_argument("--resume", action="store_true", help="skip runs that already have all their screenshots")
    p.add_argument("--keep-home", action="store_true", help="keep each run's home directory (config, game log)")
    p.add_argument("--install-hypr-rule", action="store_true", help="install the Hyprland window rule first")
    p.add_argument("--no-watchdog", action="store_true", help="do not check that game windows stay hidden")
    p.set_defaults(func=cmd_run)

    p = sub.add_parser("compare", help="compare two sides of a results directory and write a report")
    p.add_argument("results")
    p.add_argument("a", help="name of the first side")
    p.add_argument("b", help="name of the second side")
    p.add_argument("--threshold", type=int, default=0,
                   help="a pixel differs when a channel differs by more than this (0-255, default 0: any)")
    p.add_argument("--fail-on-diff", action="store_true", help="exit 1 if any frame differs or is missing")
    p.set_defaults(func=cmd_compare)

    p = sub.add_parser("list", help="list the default replays")
    p.set_defaults(func=cmd_list)

    args = parser.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
