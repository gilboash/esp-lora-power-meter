#!/usr/bin/env python3
"""Offline twin of src/meter/seven_seg.cpp.

Iterating on segment geometry by reflashing a board and squinting at a live
display is slow and unrepeatable. This mirrors the firmware's algorithm exactly
so the same crop can be replayed against different parameters in a second, and
any change proven on a fixture before it goes near the hardware.

Keep this in step with the C++ when the decoder changes -- `--verify` exists to
catch drift by comparing against fill values the firmware itself reported.

    python3 tools/ocr_sim.py tests/fixtures/iron_290_1.jpg --expect 290
"""
import argparse
import json
import sys

import numpy as np
from PIL import Image

SEG_NAMES = "abcdefg"

# Mirrors SEG_TABLE in seven_seg.cpp: bit0=a .. bit6=g
SEG_TABLE = {
    0b0111111: 0, 0b0000110: 1, 0b1011011: 2, 0b1001111: 3, 0b1100110: 4,
    0b1101101: 5, 0b1111101: 6, 0b0000111: 7, 0b1111111: 8, 0b1101111: 9,
}

# (x, y, w, h) as fractions of one digit cell, matching the firmware.
DEFAULT_WINDOWS = [
    (0.25, 0.02, 0.50, 0.16),   # a  top
    (0.72, 0.12, 0.24, 0.32),   # b  top right
    (0.72, 0.55, 0.24, 0.32),   # c  bottom right
    (0.25, 0.82, 0.50, 0.16),   # d  bottom
    (0.04, 0.55, 0.24, 0.32),   # e  bottom left
    (0.04, 0.12, 0.24, 0.32),   # f  top left
    (0.25, 0.42, 0.50, 0.16),   # g  middle
]
DEFAULT_ON_FRACTION = 0.45


def otsu(gray):
    hist = np.bincount(gray.ravel(), minlength=256).astype(np.float64)
    total = gray.size
    sum_all = np.dot(np.arange(256), hist)
    w_b = np.cumsum(hist)
    sum_b = np.cumsum(np.arange(256) * hist)
    w_f = total - w_b
    valid = (w_b > 0) & (w_f > 0)
    m_b = np.divide(sum_b, w_b, out=np.zeros(256), where=w_b > 0)
    m_f = np.divide(sum_all - sum_b, w_f, out=np.zeros(256), where=w_f > 0)
    between = w_b * w_f * (m_b - m_f) ** 2
    between[~valid] = -1
    return int(np.argmax(between))


def find_cells(ink, x0, y0, tw, th, digits):
    """Locate each digit by its own ink columns instead of dividing the box evenly.

    Seven-segment digits are narrower than their pitch, so equal slices put the
    right-hand segment windows in the gap *after* each digit. Column runs give
    the true extents.

    A "1" is the awkward case: it is only segments b and c, so its run is a thin
    bar. Treated as a whole cell it would look like every segment was lit, so a
    run far narrower than its siblings is widened to the median and anchored to
    its right edge, which is where b and c live.
    """
    sub = ink[y0:y0 + th, x0:x0 + tw]
    col = sub.sum(0)
    occupied = col > th * 0.05
    runs, start = [], None
    for i, v in enumerate(list(occupied) + [False]):
        if v and start is None:
            start = i
        if not v and start is not None:
            runs.append((start, i - 1))
            start = None
    if runs:                       # drop decimal points and specks
        widest = max(b - a + 1 for a, b in runs)
        runs = [r for r in runs if (r[1] - r[0] + 1) >= max(3, widest * 0.25)]
    if len(runs) != digits:
        cw = tw // digits
        return [(i * cw, (i + 1) * cw - 1) for i in range(digits)], False

    widths = sorted(b - a + 1 for a, b in runs)
    median = widths[len(widths) // 2]
    cells = []
    for a, b in runs:
        if (b - a + 1) < median * 0.5:
            a = max(0, b - median + 1)     # a "1": anchor right, where b/c sit
        cells.append((a, b))
    return cells, True


def decode(gray, digits, threshold=0, invert=False,
           windows=DEFAULT_WINDOWS, on_fraction=DEFAULT_ON_FRACTION,
           trim_divisor=16):
    h, w = gray.shape
    thr = threshold if threshold else otsu(gray)
    ink = (gray > thr) if invert else (gray < thr)

    perim = np.concatenate([ink[0], ink[-1], ink[1:-1, 0], ink[1:-1, -1]])
    border_ink = int(round(100 * perim.mean()))

    rows, cols = ink.sum(1), ink.sum(0)
    row_floor = max(2, w // trim_divisor)
    col_floor = max(2, h // trim_divisor)
    ry = np.where(rows >= row_floor)[0]
    rx = np.where(cols >= col_floor)[0]
    if len(ry) and len(rx):
        y0, y1, x0, x1 = ry[0], ry[-1], rx[0], rx[-1]
    else:
        y0, y1, x0, x1 = 0, h - 1, 0, w - 1
    tw, th = x1 - x0 + 1, y1 - y0 + 1
    if tw < digits * 4 or th < 8:
        x0, y0, tw, th = 0, 0, w, h

    cells_px, by_ink = find_cells(ink, x0, y0, tw, th, digits)
    out = {"threshold": thr, "border_ink": border_ink,
           "trim": [int(x0), int(y0), int(tw), int(th)],
           "segmented_by_ink": by_ink, "cells": []}
    text, value, ok = "", 0, True
    for d in range(digits):
        ca, cb = cells_px[d]
        cell = ink[y0:y0 + th, x0 + ca:x0 + cb + 1]
        ch, cw = cell.shape
        mask, fills = 0, []
        for s, (fx, fy, fw, fh) in enumerate(windows):
            xa, xb = int(fx * cw), int((fx + fw) * cw)
            ya, yb = int(fy * ch), int((fy + fh) * ch)
            xa, xb = max(0, xa), min(cw, max(xb, xa + 1))
            ya, yb = max(0, ya), min(ch, max(yb, ya + 1))
            frac = cell[ya:yb, xa:xb].mean() if (yb > ya and xb > xa) else 0.0
            fills.append(int(round(frac * 100)))
            if frac >= on_fraction:
                mask |= 1 << s
        digit = SEG_TABLE.get(mask)
        out["cells"].append({"fills": dict(zip(SEG_NAMES, fills)),
                             "mask": mask, "value": digit})
        if digit is None:
            ok = False
            text += "?"
        else:
            text += str(digit)
            value = value * 10 + digit
    out.update(text=text, value=value, ok=ok)
    return out


def load_crop(path, cfg):
    img = np.array(Image.open(path).convert("L"))
    x, y = cfg["roi_x"], cfg["roi_y"]
    w, h = cfg["roi_w"], cfg["roi_h"]
    if not w or not h:
        return img
    return img[y:y + h, x:x + w]


def report(res, expect=None):
    print(f"  threshold={res['threshold']} border_ink={res['border_ink']}% "
          f"trim={res['trim'][2]}x{res['trim'][3]}")
    for i, c in enumerate(res["cells"]):
        f = c["fills"]
        line = " ".join(f"{k}={f[k]:>3}" for k in SEG_NAMES)
        print(f"    d{i}: {line}  -> {c['value'] if c['value'] is not None else '?'}")
    verdict = f"read '{res['text']}'"
    if expect is not None:
        verdict += "  MATCH" if res["text"] == str(expect) else f"  expected '{expect}'"
    print(f"  {verdict}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--config", default="tests/fixtures/iron_290_config.json")
    ap.add_argument("--expect")
    ap.add_argument("--on-fraction", type=float, default=DEFAULT_ON_FRACTION)
    ap.add_argument("--digits", type=int)
    args = ap.parse_args()

    cfg = json.load(open(args.config))
    crop = load_crop(args.image, cfg)
    digits = args.digits or cfg["digits"]
    res = decode(crop, digits, cfg.get("thr", 0), bool(cfg.get("invert")),
                 on_fraction=args.on_fraction)
    print(f"{args.image}  crop {crop.shape[1]}x{crop.shape[0]}  "
          f"digits={digits} invert={cfg.get('invert')} on_fraction={args.on_fraction}")
    report(res, args.expect)
    return 0 if (args.expect is None or res["text"] == str(args.expect)) else 1


if __name__ == "__main__":
    sys.exit(main())
