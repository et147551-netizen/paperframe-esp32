#!/usr/bin/env python3
"""Six-colour chart patch means from flatbed scans, and pairwise deltas.

The harness (`src/entry/harness_main.c`) draws a 2x5 chart per arm at the end of a run and
`tools/bringup_capture.py --scanner` scans each one, so a sweep leaves one BMP per
setting with nothing having moved on the bed between them. This reads those BMPs and
prints the mean RGB of each patch, plus the per-channel delta of every later scan
against the first.

    python tools/chart_patches.py .scratch/captures/<run>-frs-0x08-busy-*.bmp \\
                                  .scratch/captures/<run>-frs-0x06-busy-*.bmp

Stdlib only -- there is no PIL on this machine.

Read the deltas against the controls established in
`.scratch/epd-refresh-optimization/issues/08`: the panel's own first draw of new content
differs from its second by 4.7-6.6 LSB, a same-setting repeat by 1.1-2.3 LSB, and the
scanner floor is 0.66 LSB at 300 dpi. **A difference under about 7 LSB between a first
draw and a later one is not an effect.** Compare settled state to settled state: render
each setting twice and use the second.

The panel's position on the bed is fixed by `bringup_capture.py`'s scan window, so the
geometry below is a fraction of the image and works at any dpi. It was read off a coarse
brightness map of a 150 dpi scan on 2026-09-04; --panel and --inner are there for when
the bed layout changes.
"""

import argparse
import pathlib
import struct
import sys

# Fractions of the scan that the panel occupies, and how much of each cell to sample.
PANEL = (0.095, 0.031, 0.705, 0.765)  # x0, y0, x1, y1
INNER = 0.55
COLS, ROWS = 2, 5

# The chart in src/entry/harness_main.c, in scan order.
NAMES = ["WHITE", "BLACK", "RED", "YELLOW", "GREEN", "BLUE", "WHITE2", "BLACK2",
         "BLACK3", "WHITE3"]


def read_bmp(path):
    data = pathlib.Path(path).read_bytes()
    if data[:2] != b"BM":
        raise SystemExit("%s: not a BMP" % path)
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp != 24:
        raise SystemExit("%s: %d bpp, expected 24" % (path, bpp))
    stride = (w * 3 + 3) & ~3
    top_down = h < 0
    h = abs(h)
    rows = []
    for y in range(h):
        src = off + (y if top_down else h - 1 - y) * stride
        rows.append(data[src:src + w * 3])
    return w, h, rows


def patches(path, panel, inner):
    w, h, rows = read_bmp(path)
    px0, py0 = int(panel[0] * w), int(panel[1] * h)
    px1, py1 = int(panel[2] * w), int(panel[3] * h)
    cw, ch = (px1 - px0) / COLS, (py1 - py0) / ROWS
    out = []
    for r in range(ROWS):
        for c in range(COLS):
            cx, cy = px0 + (c + 0.5) * cw, py0 + (r + 0.5) * ch
            hw, hh = cw * inner / 2, ch * inner / 2
            acc = [0.0, 0.0, 0.0]
            n = 0
            for y in range(int(cy - hh), int(cy + hh), 2):
                row = rows[y]
                for x in range(int(cx - hw), int(cx + hw), 2):
                    i = x * 3
                    acc[2] += row[i]      # BMP is BGR
                    acc[1] += row[i + 1]
                    acc[0] += row[i + 2]
                    n += 1
            if not n:
                raise SystemExit("%s: empty sample window; check --panel" % path)
            out.append(tuple(v / n for v in acc))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scans", nargs="+", help="BMPs; the first is the reference")
    ap.add_argument("--panel", type=float, nargs=4, default=PANEL,
                    metavar=("X0", "Y0", "X1", "Y1"),
                    help="panel bounding box as fractions of the scan")
    ap.add_argument("--inner", type=float, default=INNER,
                    help="fraction of each cell sampled, centred (default %.2f)" % INNER)
    args = ap.parse_args()

    ref = None
    for path in args.scans:
        p = patches(path, args.panel, args.inner)
        print("\n== %s" % pathlib.Path(path).name)
        for name, rgb in zip(NAMES, p):
            print("   %-7s %6.2f %6.2f %6.2f" % (name, rgb[0], rgb[1], rgb[2]))
        if ref is None:
            ref = (pathlib.Path(path).name, p)
            continue
        print("   -- delta against %s --" % ref[0])
        worst, worst_at = 0.0, ""
        for name, a, b in zip(NAMES, ref[1], p):
            d = [b[i] - a[i] for i in range(3)]
            m = max(abs(v) for v in d)
            if m > worst:
                worst, worst_at = m, name
            print("   %-7s %+6.2f %+6.2f %+6.2f" % (name, d[0], d[1], d[2]))
        print("   worst per-channel delta: %.2f LSB at %s" % (worst, worst_at))
    return 0


if __name__ == "__main__":
    sys.exit(main())
