#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["numpy", "pillow"]
# ///
"""Compares two directories of same-named frames (<MAP>_<tic>.png), such as VibeRant Metal's from a macOS runner and
VibeRant Vulkan's from a PC, and says how two GPUs' pictures of the same frame differ.

  uv run tests/metal-frames/compare.py DIR_A DIR_B OUT [--sheets N]

Per frame, the share of pixels that
- differ at all;
- differ by more than one level in some channel (a blended surface rounds to the next level on one GPU and not on
  another, over whole bands of a color);
- differ "beyond texel edges": neither frame has the pixel's color (within 8 levels) anywhere in the 3x3 pixels
  around it in the other frame. A pixel whose texture coordinate falls on the edge between two texels takes either
  texel, and that is not a difference in what was drawn.

OUT gets summary.json, and for the N frames with the most pixels of the last kind (20 unless given) a sheet with
frame A, frame B, and a map: gray where the pixels differ, red where they differ beyond texel edges. Prints the
medians, the 95th percentiles, the maxima, and the frames that stand out.
"""
import argparse
import json
import os
import statistics

import numpy as np
from PIL import Image


def near(image, other, tolerance):
    """For each pixel of image: is there a pixel within the 3x3 neighborhood of other whose color is within tolerance?"""
    height, width, _ = image.shape
    padded = np.pad(other, ((1, 1), (1, 1), (0, 0)), mode="edge")
    found = np.zeros((height, width), bool)
    for dy in range(3):
        for dx in range(3):
            found |= np.abs(image - padded[dy:dy + height, dx:dx + width]).max(-1) <= tolerance
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("a")
    parser.add_argument("b")
    parser.add_argument("out")
    parser.add_argument("--sheets", type=int, default=20)
    options = parser.parse_args()
    os.makedirs(options.out, exist_ok=True)

    rows, missing, masks = [], [], {}
    names = sorted(name for name in os.listdir(options.a) if name.endswith(".png"))
    missing += [name + " (only in B)" for name in sorted(os.listdir(options.b)) if name.endswith(".png") and name not in names]
    for name in names:
        if not os.path.exists(os.path.join(options.b, name)):
            missing.append(name + " (only in A)")
            continue
        a = np.array(Image.open(os.path.join(options.a, name)).convert("RGB")).astype(np.int16)
        b = np.array(Image.open(os.path.join(options.b, name)).convert("RGB")).astype(np.int16)
        if a.shape != b.shape:
            missing.append("%s (sizes %s and %s)" % (name, a.shape[:2], b.shape[:2]))
            continue
        levels = np.abs(a - b).max(-1)
        differ = levels > 0
        beyond = differ & ~(near(a, b, 8) & near(b, a, 8))
        rows.append({"frame": name, "differ": round(100 * differ.mean(), 4), "over_one_level": round(100 * (levels > 1).mean(), 4),
                     "beyond_texel_edges": round(100 * beyond.mean(), 4), "beyond_pixels": int(beyond.sum()), "max_levels": int(levels.max())})
        masks[name] = int(beyond.sum())
    if not rows:
        raise SystemExit("no frames in common")

    for name in sorted(masks, key=masks.get, reverse=True)[:options.sheets]:
        if not masks[name]:
            break
        a = np.array(Image.open(os.path.join(options.a, name)).convert("RGB"))
        b = np.array(Image.open(os.path.join(options.b, name)).convert("RGB"))
        wide_a, wide_b = a.astype(np.int16), b.astype(np.int16)
        differ = np.abs(wide_a - wide_b).max(-1) > 0
        beyond = differ & ~(near(wide_a, wide_b, 8) & near(wide_b, wide_a, 8))
        mask = np.zeros(a.shape, np.uint8)
        mask[differ] = (90, 90, 90)
        mask[beyond] = (255, 0, 0)
        Image.fromarray(np.concatenate([a, b, mask], 1)).save(os.path.join(options.out, name[:-4] + "-sheet.png"))

    with open(os.path.join(options.out, "summary.json"), "w") as handle:
        json.dump({"a": options.a, "b": options.b, "frames": rows, "missing": missing}, handle, indent=1)

    count = len(rows)

    def figures(key):
        values = sorted(row[key] for row in rows)
        return "median %.3f%%, 95%% of the frames under %.3f%%, worst %.3f%%" % (
            statistics.median(values), values[max(0, int(0.95 * count) - 1)], values[-1])

    print("%d frames compared; %d not: %s" % (count, len(missing), ", ".join(missing[:10]) or "-"))
    print("pixels that differ:                     " + figures("differ"))
    print("pixels more than one level apart:       " + figures("over_one_level"))
    print("pixels that differ beyond texel edges:  " + figures("beyond_texel_edges"))
    print("frames with at most 0.02%% beyond texel edges: %d; with more than 0.1%%: %d" % (
        sum(1 for row in rows if row["beyond_texel_edges"] <= 0.02), sum(1 for row in rows if row["beyond_texel_edges"] > 0.1)))
    for title, key in (("most pixels beyond texel edges", "beyond_texel_edges"), ("most pixels more than one level apart", "over_one_level"),
                       ("most pixels that differ", "differ")):
        print(title + ":")
        for row in sorted(rows, key=lambda row: -row[key])[:12]:
            print("  %s: differ %.3f%%, over one level %.3f%%, beyond texel edges %.4f%% (%d pixels)" % (
                row["frame"], row["differ"], row["over_one_level"], row["beyond_texel_edges"], row["beyond_pixels"]))


if __name__ == "__main__":
    main()
