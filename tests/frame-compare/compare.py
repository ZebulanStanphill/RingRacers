"""Compares the screenshots of two sides frame by frame and writes the report (see framecompare.py)."""

import html
import json
import re
from pathlib import Path

import numpy as np
from PIL import Image

NAME = re.compile(r"^(?P<map>.+)_(?P<tic>\d+)\.png$")


def load(path):
    return np.asarray(Image.open(path).convert("RGB"))


def frames(results, side):
    """{(replay, tic): path} of the screenshots a side took."""
    out = {}
    side_dir = results / side
    for replay_dir in sorted(p for p in side_dir.iterdir() if p.is_dir()) if side_dir.is_dir() else []:
        for png in sorted(replay_dir.glob("*.png")):
            m = NAME.match(png.name)
            if m:
                out[(replay_dir.name, int(m.group("tic")))] = png
    return out


def compare_arrays(a, b, threshold):
    """Numbers about two same-sized frames: how many pixels differ by more than threshold in some channel, and by
    how much."""
    diff = np.abs(a.astype(np.int16) - b.astype(np.int16))
    worst = diff.max(axis=2)  # per pixel: the largest channel difference
    differing = worst > threshold
    return {
        "identical": not bool(diff.any()),
        "differing_pixels": int(differing.sum()),
        "differing_share": float(differing.mean()),
        "mean_channel_diff": float(diff.mean()),
        "max_channel_diff": int(diff.max()),
        "bbox": bbox(differing),
    }, worst


def bbox(mask):
    if not mask.any():
        return None
    ys, xs = np.nonzero(mask)
    return [int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1]  # x0, y0, x1, y1


def half(img):
    """Half size by averaging 2x2 blocks."""
    h, w = img.shape[0] // 2 * 2, img.shape[1] // 2 * 2
    img = img[:h, :w].astype(np.uint16)
    return ((img[0::2, 0::2] + img[1::2, 0::2] + img[0::2, 1::2] + img[1::2, 1::2]) // 4).astype(np.uint8)


def diff_map(a, worst):
    """The difference as an image: black where the frames agree, from red (a difference of 1) to yellow (large)
    where they don't, on a dimmed copy of the first frame for context."""
    out = (a.astype(np.uint16) // 5).astype(np.uint8)
    d = worst > 0
    out[d, 0] = 255
    out[d, 1] = np.minimum(255, worst[d].astype(np.uint16) * 4).astype(np.uint8)
    out[d, 2] = 0
    return out


def write_sheet(path, a, b, worst):
    """Side by side: the first frame, the second, and the difference map, each at half size."""
    dm = diff_map(a, worst)
    # A one-pixel difference must stay visible when halving: take the brightest of each 2x2 block of the map
    h, w = dm.shape[0] // 2 * 2, dm.shape[1] // 2 * 2
    blocks = dm[:h, :w].reshape(h // 2, 2, w // 2, 2, 3)
    dm_half = blocks.max(axis=(1, 3))
    sheet = np.concatenate([half(a), half(b), dm_half], axis=1)
    Image.fromarray(sheet).save(path, optimize=False)


def compare_sides(results, side_a, side_b, threshold, out_dir):
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "sheets").mkdir(exist_ok=True)
    fa, fb = frames(results, side_a), frames(results, side_b)
    rows = []
    for key in sorted(set(fa) | set(fb)):
        replay, tic = key
        row = {"replay": replay, "tic": tic, "a": str(fa[key].relative_to(results)) if key in fa else None,
               "b": str(fb[key].relative_to(results)) if key in fb else None}
        if key not in fa or key not in fb:
            row["status"] = "missing in " + (side_b if key in fa else side_a)
        else:
            a, b = load(fa[key]), load(fb[key])
            if a.shape != b.shape:
                row["status"] = "size differs"
                row["sizes"] = [list(a.shape[1::-1]), list(b.shape[1::-1])]
            else:
                stats, worst = compare_arrays(a, b, threshold)
                row.update(stats)
                row["status"] = "identical" if stats["differing_pixels"] == 0 and (threshold > 0 or stats["identical"]) \
                    else "different"
                if row["status"] == "different":
                    sheet = out_dir / "sheets" / ("%s_%05d.png" % (replay, tic))
                    write_sheet(sheet, a, b, worst)
                    row["sheet"] = "sheets/" + sheet.name
        rows.append(row)
    return rows


def summarize(rows):
    total = len(rows)
    counts = {}
    for r in rows:
        counts[r["status"].split(" in ")[0]] = counts.get(r["status"].split(" in ")[0], 0) + 1
    shares = [r["differing_share"] for r in rows if "differing_share" in r]
    return {
        "frames": total,
        "identical": sum(1 for r in rows if r["status"] == "identical"),
        "different": sum(1 for r in rows if r["status"] == "different"),
        "missing": sum(1 for r in rows if r["status"].startswith("missing")),
        "size_differs": sum(1 for r in rows if r["status"] == "size differs"),
        "worst_share": max(shares) if shares else 0.0,
        "worst_max_channel_diff": max((r.get("max_channel_diff", 0) for r in rows), default=0),
    }


CSS = """
body{font:14px/1.4 system-ui,sans-serif;margin:1.5rem auto;max-width:1100px;padding:0 1rem;color:#222}
h1{font-size:1.4rem} h2{font-size:1.1rem;margin-top:2rem}
table{border-collapse:collapse;width:100%} th,td{padding:.25rem .5rem;border-bottom:1px solid #ddd;text-align:right}
th:first-child,td:first-child{text-align:left} th{background:#f4f4f4}
.identical{color:#176b2c} .different{color:#a01616;font-weight:600} .missing,.size{color:#8a5a00;font-weight:600}
img{max-width:100%;margin:.4rem 0;border:1px solid #bbb} code{background:#f0f0f0;padding:0 .25rem}
.note{color:#555}
"""


def pct(x):
    return "%.4f%%" % (100 * x) if x < 0.1 else "%.2f%%" % (100 * x)


def write_report(out_dir, results, side_a, side_b, threshold, rows, sides_info):
    summary = summarize(rows)
    report = {"a": side_a, "b": side_b, "threshold": threshold, "sides": sides_info, "summary": summary, "frames": rows}
    (out_dir / "report.json").write_text(json.dumps(report, indent=1))

    rel = lambda p: "../" + p  # the report sits in <results>/<name>/, the screenshots in <results>/<side>/...
    h = ["<!doctype html><meta charset=utf-8><title>%s vs %s</title><style>%s</style>" % (
        html.escape(side_a), html.escape(side_b), CSS)]
    h.append("<h1>%s vs %s</h1>" % (html.escape(side_a), html.escape(side_b)))
    h.append("<p><b>%d</b> frames: <span class=identical>%d identical</span>, <span class=different>%d different</span>"
             ", <span class=missing>%d missing</span>, <span class=size>%d with another size</span>. "
             "Worst frame: %s of its pixels differ, by up to %d in a channel. A pixel counts as differing when a "
             "channel differs by more than %d (of 255).</p>" % (
                 summary["frames"], summary["identical"], summary["different"], summary["missing"],
                 summary["size_differs"], pct(summary["worst_share"]), summary["worst_max_channel_diff"], threshold))
    h.append("<p class=note>Difference images: the first frame (%s), the second (%s), and where they differ, "
             "red for a small difference to yellow for a large one. Half size.</p>" % (
                 html.escape(side_a), html.escape(side_b)))

    h.append("<h2>Frames that differ</h2>")
    bad = sorted((r for r in rows if r["status"] != "identical"),
                 key=lambda r: -r.get("differing_share", 1.0))
    if not bad:
        h.append("<p class=identical>None: every frame is identical.</p>")
    for r in bad:
        title = "%s, tic %d: %s" % (r["replay"], r["tic"], r["status"])
        h.append("<h3>%s</h3>" % html.escape(title))
        if "differing_share" in r:
            h.append("<p>%s of the pixels differ (%d); mean channel difference %.3f, maximum %d; "
                     "differences within x %d-%d, y %d-%d. "
                     "<a href='%s'>first frame</a>, <a href='%s'>second frame</a>.</p>" % (
                         pct(r["differing_share"]), r["differing_pixels"], r["mean_channel_diff"],
                         r["max_channel_diff"], *( [r["bbox"][0], r["bbox"][2], r["bbox"][1], r["bbox"][3]]
                                                   if r["bbox"] else [0, 0, 0, 0]),
                         html.escape(rel(r["a"])), html.escape(rel(r["b"]))))
            h.append("<a href='%s'><img src='%s' alt='%s'></a>" % (html.escape(r["sheet"]), html.escape(r["sheet"]),
                                                                    html.escape(title)))

    h.append("<h2>All frames</h2><table><tr><th>replay<th>tic<th>result<th>differing pixels<th>mean diff<th>max diff")
    for r in rows:
        cls = {"identical": "identical", "different": "different"}.get(r["status"], "missing")
        h.append("<tr><td>%s<td>%d<td class=%s>%s<td>%s<td>%s<td>%s" % (
            html.escape(r["replay"]), r["tic"], cls, html.escape(r["status"]),
            pct(r["differing_share"]) if "differing_share" in r else "",
            "%.3f" % r["mean_channel_diff"] if "mean_channel_diff" in r else "",
            r.get("max_channel_diff", "")))
    h.append("</table>")
    h.append("<h2>Sides</h2><ul>")
    for s in sides_info:
        h.append("<li><b>%s</b>: %s <code>%s</code> %s</li>" % (
            html.escape(s["name"]), html.escape(s["kind"]), html.escape(s["target"]),
            html.escape(" ".join(s["args"]))))
    h.append("</ul>")
    (out_dir / "index.html").write_text("\n".join(h))
    return summary
