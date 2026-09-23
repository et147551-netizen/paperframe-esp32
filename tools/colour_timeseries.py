#!/usr/bin/env python3
"""Turn a long camera run of the colour chart into numbers. Ticket issues/20.

The USB camera is the only colour instrument this project has. The colorimeter on the
bench is a monitor probe -- it measures emissive displays, and E Ink is reflective, so
it cannot read this panel at all. There is no absolute-colorimetry path to wait for.
That does not make a webcam a colorimeter, and nothing here pretends otherwise:

  * every number is RELATIVE, normalised inside each photograph against white and black
    patches that were in the same frame under the same light;
  * every claim is stated against a measured noise floor -- repeated shots of an
    unchanged panel -- and a movement smaller than that floor is reported as not
    resolved, not as a small effect;
  * no L*a*b*, no dE2000, no pass/fail verdict. tools/colour_check.py grades against the
    module manual's optical spec and still needs an instrument that can read a
    reflective surface.

What it CAN do, and what issues/19 actually needs, is discriminate between the candidate
palettes: three published tables disagree by 30-40 RGB units about what this panel looks
like, and a relative measurement is enough to say which of them our unit resembles.
--compare does that, and reports the answer under two assumptions about the camera's
transfer function so that a conclusion which depends on the assumption is visible as one.

    python tools/colour_timeseries.py --self-test
    python tools/colour_timeseries.py --calibrate shot.png \\
        --panel "410,120 1480,140 1470,980 400,960"
    python tools/colour_timeseries.py extract .scratch/captures \\
        --log .scratch/captures/colourchart-long-*.log -o run.csv
    python tools/colour_timeseries.py --report run.csv
    python tools/colour_timeseries.py --compare run.csv

Standard library only; shells out to ffmpeg for pixels, like the other tools here.
"""

import argparse
import csv
import glob
import io
import json
import pathlib
import re
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
import zlib

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
RUN_DIR = REPO_ROOT / ".scratch" / "digital-frame" / "colour-run"
DEFAULT_ROIS = RUN_DIR / "rois.json"

# Fraction of each cell that is sampled, centred. The cells meet edge to edge (see
# epd_pack_chart), so the boundary between two patches is a hard colour edge; staying
# well inside keeps a slightly wrong quad from mixing two patches into one reading.
INNER_FRACTION = 0.6
SAMPLE_GRID = 25  # 625 samples per patch

# Rows 0 and 4 of the chart are the in-frame reference pair (src/colour_chart_main.c).
REF_ROWS = (0, 4)

PALETTE_NAMES = {0: "black", 1: "white", 2: "yellow", 3: "red", 5: "blue", 6: "green"}

# The candidate palettes issues/19 lists, as sRGB. The question that ticket cannot
# answer from documents is which one this unit resembles.
CANDIDATES = {
    "m5gfx_device": {  # what src/core/epd_dither.c currently matches against
        "white": (255, 255, 255), "black": (0, 0, 0), "red": (191, 0, 0),
        "yellow": (255, 243, 56), "blue": (100, 64, 255), "green": (67, 138, 28),
    },
    "manual_lab": {  # EL040EF1 module manual, L*a*b* converted to sRGB
        "white": (154, 164, 162), "black": (33, 29, 47), "red": (123, 25, 19),
        "yellow": (160, 148, 28), "blue": (44, 84, 129), "green": (61, 97, 46),
    },
    "epdoptimize_spectra6": {
        "white": (185, 199, 201), "black": (31, 34, 38), "red": (98, 32, 30),
        "yellow": (170, 151, 55), "blue": (54, 76, 121), "green": (73, 105, 63),
    },
    "epdoptimize_aitjcize": {
        "white": (190, 200, 200), "black": (2, 2, 2), "red": (135, 19, 0),
        "yellow": (200, 190, 30), "blue": (30, 50, 130), "green": (50, 110, 60),
    },
}

SHOT_RE = re.compile(
    r"^(?:(?P<run>.*)-)?chart-(?P<variant>[AB])-c(?P<cycle>\d+)-t(?P<t>\d+)"
    r"(?:-s(?P<shot>\d+))?-(?P<stamp>\d{8}-\d{6})\.png$")

SAMPLE_RE = re.compile(
    r"^#\s*sample\s+cycle=(?P<cycle>\d+)\s+variant=(?P<variant>[AB])\s+"
    r"t_req_s=(?P<t>-?\d+)\s+t_actual_ms=(?P<actual>-?\d+)")

LAYOUT_RE = re.compile(
    r"^#\s*layout\s+(?P<variant>[AB])\s+r(?P<row>\d+)c(?P<col>\d+)\s+"
    r"index=(?P<index>\d+)")

FIELD_RE = re.compile(r"(\w+)=(NA|-?[\d.]+)")

CSV_COLUMNS = [
    "stamp", "cycle", "variant", "t_req_s", "t_actual_ms", "shot", "cell", "index",
    "colour", "is_ref", "r", "g", "b", "r_norm", "g_norm", "b_norm",
    "sd_r", "sd_g", "sd_b", "n", "air_c", "rh", "reject",
]

# Raw units the white references must sit above the black ones by, for a photograph to
# be worth normalising. A real frame separates them by about 120. The first long run
# produced four photographs of a panel mid-waveform -- every cell a flat warm grey,
# references four units apart -- and dividing by that turned a nothing into readings of
# 40 and 60 normalised units. The frames are kept, marked, and left out of the maths.
MIN_REFERENCE_SPAN = 20.0


# ------------------------------------------------------------------- image reading

class Image:
    """Just enough of an image to read pixels out of one."""

    def __init__(self, width, height, rgb):
        self.width = width
        self.height = height
        self.rgb = rgb

    def pixel(self, x, y):
        i = (y * self.width + x) * 3
        return self.rgb[i], self.rgb[i + 1], self.rgb[i + 2]


def png_size(path):
    """Width and height straight out of the IHDR, without decoding the image."""
    with open(path, "rb") as fh:
        head = fh.read(24)
    if head[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path} is not a PNG")
    return struct.unpack(">II", head[16:24])


def load_image(path):
    width, height = png_size(path)
    proc = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", str(path),
         "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        capture_output=True)
    expected = width * height * 3
    if proc.returncode != 0 or len(proc.stdout) != expected:
        raise RuntimeError(
            f"decode of {path} failed ({proc.returncode}), got {len(proc.stdout)} "
            f"bytes, expected {expected}\n{proc.stderr.decode('utf-8', 'replace')}")
    return Image(width, height, proc.stdout)


def write_png(path, width, height, rgb):
    """Minimal RGB PNG writer, for the self-test's synthetic frames."""
    raw = b"".join(b"\x00" + bytes(rgb[y * width * 3:(y + 1) * width * 3])
                   for y in range(height))

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(
            ">I", zlib.crc32(body) & 0xFFFFFFFF)

    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        fh.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        fh.write(chunk(b"IEND", b""))


# ------------------------------------------------------------------------ geometry

def parse_quad(text):
    """'x,y x,y x,y x,y' as top-left, top-right, bottom-right, bottom-left."""
    points = []
    for part in text.replace(";", " ").split():
        x, y = part.split(",")
        points.append((float(x), float(y)))
    if len(points) != 4:
        raise ValueError("--panel needs four 'x,y' corners: TL TR BR BL")
    return points


def bilinear(quad, u, v):
    """Point at (u, v) in [0,1]^2 across the quad. Handles a panel photographed
    slightly rotated or off-axis; it does not correct real perspective, which is why
    the ROIs are checked against an overlay rather than trusted."""
    (x0, y0), (x1, y1), (x2, y2), (x3, y3) = quad
    top = ((1 - u) * x0 + u * x1, (1 - u) * y0 + u * y1)
    bottom = ((1 - u) * x3 + u * x2, (1 - u) * y3 + u * y2)
    return ((1 - v) * top[0] + v * bottom[0], (1 - v) * top[1] + v * bottom[1])


def cell_samples(quad, cols, rows, row, col, inner=INNER_FRACTION, grid=SAMPLE_GRID):
    """Sample points inside the inner part of one cell, in image coordinates."""
    margin = (1.0 - inner) / 2.0
    points = []
    for iy in range(grid):
        v = (row + margin + inner * (iy + 0.5) / grid) / rows
        for ix in range(grid):
            u = (col + margin + inner * (ix + 0.5) / grid) / cols
            points.append(bilinear(quad, u, v))
    return points


def measure_cell(img, quad, cols, rows, row, col):
    """Median and standard deviation per channel over one cell."""
    chans = ([], [], [])
    for x, y in cell_samples(quad, cols, rows, row, col):
        xi, yi = int(round(x)), int(round(y))
        if 0 <= xi < img.width and 0 <= yi < img.height:
            r, g, b = img.pixel(xi, yi)
            chans[0].append(r)
            chans[1].append(g)
            chans[2].append(b)
    if not chans[0]:
        raise ValueError(f"cell r{row}c{col} sampled entirely outside the image")
    med = [statistics.median(c) for c in chans]
    sd = [statistics.pstdev(c) if len(c) > 1 else 0.0 for c in chans]
    return med, sd, len(chans[0])


# ------------------------------------------------------------------ log and layout

def read_rois(path):
    with open(path) as fh:
        data = json.load(fh)
    data["quad"] = [tuple(p) for p in data["quad"]]
    return data


def parse_log(paths):
    """Layout tables and per-sample conditions out of the firmware's own log."""
    layouts = {}
    samples = {}
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = LAYOUT_RE.match(line)
                if m:
                    key = (m["variant"], int(m["row"]), int(m["col"]))
                    layouts[key] = int(m["index"])
                    continue
                m = SAMPLE_RE.match(line)
                if m:
                    fields = dict(FIELD_RE.findall(line))
                    samples[(int(m["cycle"]), m["variant"], int(m["t"]))] = {
                        "t_actual_ms": int(m["actual"]),
                        "air_c": fields.get("air_c", "NA"),
                        "rh": fields.get("rh", "NA"),
                    }
    return layouts, samples


def normalise(value, black, white):
    span = white - black
    if abs(span) < 1e-9:
        return ""
    return (value - black) / span


# ---------------------------------------------------------------------- extraction

def extract(photo_paths, rois, layouts, samples, out):
    cols, rows = rois["cols"], rois["rows"]
    quad = rois["quad"]

    writer = csv.writer(out)
    writer.writerow(CSV_COLUMNS)

    written, skipped, rejected = 0, [], []
    for path in sorted(photo_paths):
        m = SHOT_RE.match(pathlib.Path(path).name)
        if not m:
            # Named chart-*.png but not by this run -- ticket 07 left files like
            # chart-png-dither-*.png in the same directory. Say which, rather than
            # quietly reading fewer photographs than the run took.
            skipped.append(pathlib.Path(path).name)
            continue
        variant = m["variant"]
        cycle, t_req = int(m["cycle"]), int(m["t"])
        shot = int(m["shot"] or 1)
        cond = samples.get((cycle, variant, t_req), {})

        img = load_image(path)
        cells = {}
        for row in range(rows):
            for col in range(cols):
                cells[(row, col)] = measure_cell(img, quad, cols, rows, row, col)

        # Normalisation references: the white and black patches of the reference rows,
        # in this photograph, under this light. Averaging the pair at each end is what
        # keeps a lamp on one side of the room from being read as a colour.
        refs = {1: [], 0: []}
        for (row, col), (med, _sd, _n) in cells.items():
            if row in REF_ROWS:
                index = layouts.get((variant, row, col))
                if index in refs:
                    refs[index].append(med)
        white_ref = [statistics.mean(c) for c in zip(*refs[1])] if refs[1] else None
        black_ref = [statistics.mean(c) for c in zip(*refs[0])] if refs[0] else None

        # A photograph in which white and black read almost the same is not a photograph
        # of the chart -- the camera caught the panel mid-waveform, or saw nothing.
        reject = ""
        if not white_ref or not black_ref:
            reject = "no_references"
        else:
            span = min(white_ref[i] - black_ref[i] for i in range(3))
            if span < MIN_REFERENCE_SPAN:
                reject = "reference_span_%.1f" % span
                rejected.append(m["stamp"])

        for (row, col), (med, sd, n) in sorted(cells.items()):
            index = layouts.get((variant, row, col))
            norm = ["", "", ""]
            if not reject:
                norm = [normalise(med[i], black_ref[i], white_ref[i]) for i in range(3)]
            writer.writerow([
                m["stamp"], cycle, variant, t_req, cond.get("t_actual_ms", ""), shot,
                f"r{row}c{col}", "" if index is None else index,
                PALETTE_NAMES.get(index, ""), int(row in REF_ROWS),
                *[f"{v:.2f}" for v in med],
                *[f"{v:.5f}" if v != "" else "" for v in norm],
                *[f"{v:.2f}" for v in sd], n,
                cond.get("air_c", ""), cond.get("rh", ""), reject,
            ])
            written += 1

    if skipped:
        sys.stderr.write("skipped %d file(s) whose names are not from a chart run: %s\n"
                         % (len(skipped), ", ".join(sorted(skipped)[:4])))
    if rejected:
        sys.stderr.write("%d photograph(s) rejected, references too close together: %s\n"
                         % (len(rejected), ", ".join(rejected)))
    return written


# -------------------------------------------------------------------- the reporting

def load_rows(path):
    with open(path, newline="") as fh:
        return list(csv.DictReader(fh))


def fnum(row, key):
    value = row.get(key, "")
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def spread(values):
    return max(values) - min(values) if len(values) > 1 else 0.0


def noise_floor(rows):
    """Spread between shots taken at the same instant of an unchanged panel. This is
    the camera and the room; nothing smaller than it is a fact about the ink."""
    groups = {}
    for row in rows:
        key = (row["cycle"], row["variant"], row["t_req_s"], row["cell"])
        for i, ch in enumerate("rgb"):
            value = fnum(row, f"{ch}_norm")
            if value is not None:
                groups.setdefault(key, ([], [], []))[i].append(value)

    # Keyed by cell AND variant: the same cell holds a different colour in A and B, and
    # the floor is an order of magnitude worse on a saturated patch than a neutral one.
    per_patch = {}
    for (_c, variant, _t, cell), chans in groups.items():
        for chan in chans:
            if len(chan) > 1:
                per_patch.setdefault((cell, variant), []).append(spread(chan))
    return per_patch


def report(rows, out):
    if not rows:
        out.write("no rows\n")
        return 1

    rejected = {r["stamp"] for r in rows if r.get("reject")}
    if rejected:
        out.write("%d photograph(s) excluded: the white and black references were "
                  "closer\ntogether than %g raw units, which means the camera was not "
                  "looking at a\nsettled chart. They are in the CSV with a reject "
                  "reason and no normalised\nvalues.\n\n"
                  % (len(rejected), MIN_REFERENCE_SPAN))

    floors = noise_floor(rows)
    flat = [v for values in floors.values() for v in values]

    # A cell holds a different colour in each variant, so every table below is keyed by
    # both. Keying on the cell alone takes the median of two different colours, which is
    # a number with no referent.
    colour_of = {}
    for row in rows:
        if row["colour"]:
            colour_of[(row["cell"], row["variant"])] = row["colour"]

    def label(patch):
        return "%s/%s" % patch, colour_of.get(patch, "")

    out.write("CAMERA NOISE FLOOR (normalised units, spread between shots of an\n"
              "unchanged panel; a movement below this is not resolved by this run)\n\n")
    if not flat:
        out.write("  NONE MEASURED -- the run took one shot per marker. Re-run with\n"
                  "  --shots 3; without it nothing below can be called a result.\n\n")
        overall = None
    else:
        overall = statistics.median(flat)
        out.write("%-10s %-8s %9s %9s %6s\n"
                  % ("cell/var", "colour", "median", "max", "n"))
        for patch in sorted(floors):
            values = floors[patch]
            name, colour = label(patch)
            out.write("%-10s %-8s %9.4f %9.4f %6d\n"
                      % (name, colour, statistics.median(values), max(values),
                         len(values)))
        out.write("\n  overall median %.4f, worst %.4f\n\n" % (overall, max(flat)))

    # --- settling: normalised value against time since the refresh, median over cycles
    out.write("SETTLING -- median over cycles, by seconds since the end of DRF. Each\n"
              "patch is shown in whichever channel moved the most: the movement is not\n"
              "in the same channel for every colour, and reading one channel is how a\n"
              "real effect gets missed.\n\n"
              "The comparison that decides it is `spread(t)` against `spread(cycle)`,\n"
              "both over the times where every cycle has data. spread(t) is how much\n"
              "the value depends on time since the refresh; spread(cycle) is how much\n"
              "the same instant differs between one refresh and the next. Time only\n"
              "means something if it beats that. Comparing spread(t) against the\n"
              "three-shot floor instead would compare a range over many points with a\n"
              "range over three, and call every patch a mover.\n\n")

    times = sorted({int(r["t_req_s"]) for r in rows})
    subjects = sorted({(r["cell"], r["variant"]) for r in rows if r["is_ref"] == "0"})

    # Times where more than one cycle was sampled. The tail belongs to the last cycle
    # only, so it is printed but cannot take part in a between-cycle comparison.
    cycles_at = {}
    for row in rows:
        cycles_at.setdefault(int(row["t_req_s"]), set()).add(row["cycle"])
    shared = sorted(t for t, cyc in cycles_at.items() if len(cyc) > 1)

    out.write("%-10s %-8s %-4s" % ("cell/var", "colour", "ch"))
    for t in times:
        out.write(" %8s" % f"t{t}")
    out.write("   %9s %9s\n" % ("spread(t)", "spr(cyc)"))

    moved, comparable = 0, 0
    for patch in subjects:
        # value[channel][t][cycle] -> median over that cycle's shots
        per_channel = {}
        for row in rows:
            if (row["cell"], row["variant"]) != patch:
                continue
            for ch in "rgb":
                value = fnum(row, f"{ch}_norm")
                if value is not None:
                    per_channel.setdefault(ch, {}).setdefault(
                        int(row["t_req_s"]), {}).setdefault(
                            row["cycle"], []).append(value)
        if not per_channel:
            continue

        def collapse(by_t):
            return {t: statistics.median(
                        [statistics.median(v) for v in by_cycle.values()])
                    for t, by_cycle in by_t.items()}

        medians = {ch: collapse(by_t) for ch, by_t in per_channel.items()}
        ch = max(medians, key=lambda c: spread(list(medians[c].values())))
        series, by_t = medians[ch], per_channel[ch]

        name, colour = label(patch)
        out.write("%-10s %-8s %-4s" % (name, colour, ch))
        for t in times:
            out.write(" %8s" % (f"{series[t]:.4f}" if t in series else "-"))

        usable = [t for t in shared if t in series]
        if len(usable) > 1:
            time_spread = spread([series[t] for t in usable])
            cycle_spreads = [spread([statistics.median(v)
                                     for v in by_t[t].values()])
                             for t in usable if len(by_t[t]) > 1]
            if cycle_spreads:
                cycle_spread = statistics.median(cycle_spreads)
                comparable += 1
                tag = ""
                if time_spread > cycle_spread:
                    moved += 1
                else:
                    tag = " (below the between-refresh spread)"
                out.write("   %9.4f %9.4f%s" % (time_spread, cycle_spread, tag))
        out.write("\n")

    out.write("\n  %d of %d patches vary with time since the refresh by more than they\n"
              "  vary between one refresh and the next.\n\n" % (moved, comparable))
    if shared and max(times) > max(shared):
        out.write("  Times after t%d come from one cycle only and take no part in that\n"
                  "  comparison -- they are a single observation, not a median.\n\n"
                  % max(shared))

    # --- cycle-to-cycle repeatability at matched time
    out.write("REPEATABILITY -- spread across cycles at the same time after the\n"
              "refresh, in the worst of the three channels.\n\n")
    out.write("%-10s %-8s %-4s %9s %9s\n"
              % ("cell/var", "colour", "ch", "median", "worst"))
    for patch in subjects:
        best = None
        for ch in "rgb":
            per_t = {}
            for row in rows:
                if (row["cell"], row["variant"]) != patch:
                    continue
                value = fnum(row, f"{ch}_norm")
                if value is not None:
                    per_t.setdefault(int(row["t_req_s"]), {}).setdefault(
                        row["cycle"], []).append(value)
            spreads = [spread([statistics.median(v) for v in by_cycle.values()])
                       for by_cycle in per_t.values() if len(by_cycle) > 1]
            if spreads and (best is None or statistics.median(spreads) > best[1]):
                best = (ch, statistics.median(spreads), max(spreads))
        if best:
            name, colour = label(patch)
            out.write("%-10s %-8s %-4s %9.4f %9.4f\n"
                      % (name, colour, best[0], best[1], best[2]))
    out.write("\n")

    # --- the same colour in two different cells, across the two chart variants
    out.write("VARIANT A vs B -- the same colour, in a different cell and beside\n"
              "different neighbours. A difference here is the panel or the lighting\n"
              "being non-uniform, not the colour.\n\n")
    by_colour = {}
    for row in rows:
        if not row["colour"]:
            continue
        for ch in "rgb":
            value = fnum(row, f"{ch}_norm")
            if value is not None:
                by_colour.setdefault((row["colour"], ch), {}).setdefault(
                    row["variant"], []).append(value)
    out.write("%-8s %-4s %9s %9s %9s\n" % ("colour", "ch", "A", "B", "A-B"))
    for colour, ch in sorted(by_colour):
        sides = by_colour[(colour, ch)]
        if "A" in sides and "B" in sides:
            a, b = statistics.median(sides["A"]), statistics.median(sides["B"])
            out.write("%-8s %-4s %9.4f %9.4f %+9.4f\n" % (colour, ch, a, b, a - b))

    out.write("\nRelative measurements from an uncalibrated webcam. No L*a*b*, no\n"
              "dE2000, no verdict: the bench colorimeter is a monitor probe and cannot\n"
              "read a reflective panel, so nothing here is traceable to the manual's\n"
              "optical spec. See issues/20.\n")
    return 0


# --------------------------------------------------------------- palette comparison

def srgb_to_linear(v):
    v = v / 255.0
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def candidate_normalised(table, gamma):
    """Put a published palette on the same footing as a camera reading: white to 1,
    black to 0, per channel, under an assumed transfer function."""
    def decode(rgb):
        if gamma == "srgb":
            return [srgb_to_linear(c) for c in rgb]
        return [(c / 255.0) ** gamma for c in rgb]

    white, black = decode(table["white"]), decode(table["black"])
    out = {}
    for name, rgb in table.items():
        value = decode(rgb)
        out[name] = [normalise(value[i], black[i], white[i]) for i in range(3)]
    return out


def compare(rows, out):
    """Which published palette does this unit resemble? Relative measurement is enough
    to answer that, and it is issues/19's open question."""
    measured = {}
    for row in rows:
        colour = row["colour"]
        if not colour:
            continue
        values = [fnum(row, f"{ch}_norm") for ch in "rgb"]
        if all(v is not None for v in values):
            measured.setdefault(colour, ([], [], []))
            for i in range(3):
                measured[colour][i].append(values[i])
    if not measured:
        out.write("no normalised rows to compare\n")
        return 1

    med = {c: [statistics.median(ch) for ch in chans] for c, chans in measured.items()}

    out.write("MEASURED, normalised against the in-frame white and black references\n"
              "(median over every photograph in the run)\n\n")
    out.write("%-8s %8s %8s %8s\n" % ("colour", "r", "g", "b"))
    for colour in sorted(med):
        out.write("%-8s %8.4f %8.4f %8.4f\n" % (colour, *med[colour]))

    out.write("\nAGREEMENT WITH THE PUBLISHED TABLES -- RMS difference in normalised\n"
              "units over the four non-reference colours. Lower is closer.\n\n")

    # The camera's transfer function is unknown. Running the comparison under two very
    # different assumptions is the check that matters: a ranking that survives both is
    # a result, and one that does not is an artefact of the assumption.
    assumptions = [("gamma 1.0 (linear)", 1.0), ("sRGB decode", "srgb")]
    rankings = []
    out.write("%-24s" % "palette")
    for label, _ in assumptions:
        out.write(" %18s" % label)
    out.write("\n")

    for name, table in CANDIDATES.items():
        out.write("%-24s" % name)
        scores = []
        for _label, gamma in assumptions:
            cand = candidate_normalised(table, gamma)
            errs = []
            for colour, values in med.items():
                if colour in ("white", "black") or colour not in cand:
                    continue
                errs += [(values[i] - cand[colour][i]) ** 2 for i in range(3)]
            score = (sum(errs) / len(errs)) ** 0.5 if errs else float("nan")
            scores.append(score)
            out.write(" %18.4f" % score)
        rankings.append((name, scores))
        out.write("\n")

    out.write("\n")
    best = [min(rankings, key=lambda r: r[1][i])[0] for i in range(len(assumptions))]
    if len(set(best)) == 1:
        out.write("Closest under both assumptions: %s. The ranking does not depend on\n"
                  "what the camera does to its pixels, which is the only reason it can\n"
                  "be quoted at all.\n" % best[0])
    else:
        out.write("Closest depends on the assumed transfer function (%s). That makes\n"
                  "this inconclusive, not a close call: the camera would have to be\n"
                  "characterised before either answer means anything.\n"
                  % ", ".join(best))

    out.write("\nThis compares SHAPE, not absolute colour. It cannot say the panel's\n"
              "white is L* 66.5; it can say which table's relationships between the six\n"
              "colours match what the camera sees on this unit.\n")
    return 0


# ------------------------------------------------------------------------ calibrate

def calibrate(photo, quad, cols, rows, rois_path, overlay_path):
    img = load_image(photo)
    rois = {"source": str(photo), "quad": [list(p) for p in quad], "cols": cols,
            "rows": rows, "inner_fraction": INNER_FRACTION}
    rois_path.parent.mkdir(parents=True, exist_ok=True)
    with open(rois_path, "w") as fh:
        json.dump(rois, fh, indent=2)

    # Corner marks rather than boxes: the ROI of a panel photographed even slightly
    # off-square is not an axis-aligned rectangle, and drawing it as one would show a
    # box that is not what gets sampled.
    marks = []
    for row in range(rows):
        for col in range(cols):
            margin = (1.0 - INNER_FRACTION) / 2.0
            for du in (margin, 1.0 - margin):
                for dv in (margin, 1.0 - margin):
                    x, y = bilinear(quad, (col + du) / cols, (row + dv) / rows)
                    marks.append("drawbox=x=%d:y=%d:w=9:h=9:color=magenta:t=fill"
                                 % (int(x) - 4, int(y) - 4))
    proc = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(photo),
         "-vf", ",".join(marks), str(overlay_path)], capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write("overlay failed: %s\n" % proc.stderr)

    print("rois:    %s" % rois_path)
    print("overlay: %s   <- LOOK AT THIS before starting a long run\n" % overlay_path)
    print("%-8s %8s %8s %8s   %s" % ("cell", "r", "g", "b", "sd"))
    for row in range(rows):
        for col in range(cols):
            med, sd, _n = measure_cell(img, quad, cols, rows, row, col)
            print("r%dc%d     %8.1f %8.1f %8.1f   %.1f"
                  % (row, col, med[0], med[1], med[2], statistics.mean(sd)))
    print("\nThe two reference rows (0 and 4) must read light in column 0 and dark in\n"
          "column 1. If they do not, the quad is wrong or upside down.")
    return 0


# ------------------------------------------------------------------------ self-test

def synth_frame(path, values, illumination=1.0, cols=2, rows=5, cell=40):
    """A fake photograph of the chart: ten flat cells, optionally under dimmer light."""
    width, height = cols * cell, rows * cell
    rgb = bytearray(width * height * 3)
    for y in range(height):
        for x in range(width):
            r, g, b = values[(y // cell) * cols + (x // cell)]
            i = (y * width + x) * 3
            rgb[i] = min(255, int(r * illumination))
            rgb[i + 1] = min(255, int(g * illumination))
            rgb[i + 2] = min(255, int(b * illumination))
    write_png(path, width, height, rgb)
    return [(0.0, 0.0), (float(width), 0.0), (float(width), float(height)),
            (0.0, float(height))]


def self_test(tmp_dir):
    failures = []

    def check(name, condition, detail=""):
        if condition:
            print("  PASS  %s" % name)
        else:
            print("  FAIL  %s %s" % (name, detail))
            failures.append(name)

    tmp_dir.mkdir(parents=True, exist_ok=True)

    # White refs at r0c0/r4c0, black refs at r0c1/r4c1, four subjects between them.
    values = [
        (200, 200, 200), (30, 30, 30),
        (120, 40, 35), (170, 150, 55),
        (60, 80, 120), (70, 105, 65),
        (200, 200, 200), (30, 30, 30),
        (200, 200, 200), (30, 30, 30),
    ]
    layouts = {}
    for i, index in enumerate([1, 0, 3, 2, 5, 6, 1, 0, 1, 0]):
        layouts[("A", i // 2, i % 2)] = index

    bright = tmp_dir / "chart-A-c1-t0-s1-20260902-120000.png"
    quad = synth_frame(bright, values)
    rois = {"quad": quad, "cols": 2, "rows": 5}

    print("a flat synthetic chart reads back the colours that were drawn:")
    img = load_image(bright)
    med, sd, n = measure_cell(img, quad, 2, 5, 1, 0)
    check("red cell median is the drawn value",
          [round(v) for v in med] == [120, 40, 35], str(med))
    check("a flat cell has zero spread", max(sd) == 0.0, str(sd))
    check("the inner fraction is actually sampled", n == SAMPLE_GRID ** 2, str(n))

    print("\nnormalisation makes a dimmer room read the same:")
    dim = tmp_dir / "chart-A-c1-t15-s1-20260902-120100.png"
    synth_frame(dim, values, illumination=0.5)

    def norms(paths):
        buf = io.StringIO()
        extract(paths, rois, layouts, {}, buf)
        buf.seek(0)
        return {(r["cell"], r["t_req_s"]): fnum(r, "g_norm")
                for r in csv.DictReader(buf)}

    got = norms([bright, dim])
    check("raw values differ between the two frames",
          abs(load_image(dim).pixel(20, 60)[1]
              - load_image(bright).pixel(20, 60)[1]) > 10)
    for cell in ("r1c0", "r1c1", "r2c0", "r2c1"):
        check("%s survives a halved illumination" % cell,
              abs(got[(cell, "0")] - got[(cell, "15")]) < 0.02,
              "%.4f vs %.4f" % (got[(cell, "0")], got[(cell, "15")]))
    check("white reference normalises to 1", abs(got[("r0c0", "0")] - 1.0) < 1e-6)
    check("black reference normalises to 0", abs(got[("r0c1", "0")]) < 1e-6)

    print("\na patch that really changes is not cancelled by the normalisation:")
    moved_values = list(values)
    moved_values[2] = (120, 90, 35)  # red cell, green channel up
    moved = tmp_dir / "chart-A-c2-t0-s1-20260902-120200.png"
    synth_frame(moved, moved_values)
    got2 = norms([bright, moved])
    check("the moved cell moves",
          abs(got2[("r1c0", "0")] - got[("r1c0", "0")]) > 0.2)

    print("\nthe noise floor is measured from repeated shots, not assumed:")
    shot2 = tmp_dir / "chart-A-c1-t0-s2-20260902-120001.png"
    synth_frame(shot2, values, illumination=1.02)
    buf = io.StringIO()
    extract([bright, shot2], rois, layouts, {}, buf)
    buf.seek(0)
    rows_ = list(csv.DictReader(buf))
    floors = noise_floor(rows_)
    check("every cell gets a floor", len(floors) == 10, str(len(floors)))
    check("a uniform light change leaves almost no residual floor",
          max(max(v) for v in floors.values()) < 0.02,
          str(max(max(v) for v in floors.values())))

    print("\nthe report refuses to imply a result it does not have:")
    buf = io.StringIO()
    extract([bright], rois, layouts, {}, buf)
    buf.seek(0)
    single = io.StringIO()
    report(list(csv.DictReader(buf)), single)
    check("a one-shot run says the floor was never measured",
          "NONE MEASURED" in single.getvalue())
    check("and says so before any settling numbers",
          single.getvalue().index("NONE MEASURED")
          < single.getvalue().index("SETTLING"))

    print("\na photograph of a panel mid-waveform is rejected, not normalised:")
    # Every cell nearly the same flat colour, which is what the camera records if it
    # catches the panel being repainted. The first long run put four of these in the
    # data and they normalised to readings of 40 and 60.
    flat_values = [(126, 121, 78)] * 10
    flat_shot = tmp_dir / "chart-A-c3-t1800-s2-20260902-135758.png"
    synth_frame(flat_shot, flat_values)
    buf = io.StringIO()
    extract([flat_shot], rois, layouts, {}, buf)
    buf.seek(0)
    flat_rows = list(csv.DictReader(buf))
    check("the frame is kept, with a reject reason",
          all(r["reject"].startswith("reference_span") for r in flat_rows))
    check("and carries no normalised values",
          all(r["g_norm"] == "" for r in flat_rows))
    buf = io.StringIO()
    report(flat_rows + list(csv.DictReader(io.StringIO())), buf)
    check("the report says how many were excluded", "1 photograph(s) excluded" in
          buf.getvalue())

    print("\nthe same cell in variant A and variant B is not averaged together:")
    # r1c0 is red in A. Make it blue in B, at a clearly different level, and check the
    # report keeps them apart rather than taking the median of the two.
    layouts_b = dict(layouts)
    for i, index in enumerate([1, 0, 5, 6, 3, 2, 1, 0, 1, 0]):
        layouts_b[("B", i // 2, i % 2)] = index
    b_values = list(values)
    b_values[2] = (60, 80, 120)
    b_shot = tmp_dir / "chart-B-c2-t0-s1-20260902-130000.png"
    synth_frame(b_shot, b_values)
    buf = io.StringIO()
    extract([bright, b_shot], rois, layouts_b, {}, buf)
    buf.seek(0)
    mixed = io.StringIO()
    report(list(csv.DictReader(buf)), mixed)
    text = mixed.getvalue()
    check("both r1c0/A and r1c0/B appear", "r1c0/A" in text and "r1c0/B" in text)
    check("each is named by its own colour",
          "r1c0/A     red" in text and "r1c0/B     blue" in text,
          [ln for ln in text.splitlines() if ln.startswith("r1c0")])

    print("\nthe palette comparison is honest about the transfer function:")
    buf = io.StringIO()
    extract([bright, moved], rois, layouts, {}, buf)
    buf.seek(0)
    cmp_out = io.StringIO()
    compare(list(csv.DictReader(buf)), cmp_out)
    check("every candidate is scored",
          all(name in cmp_out.getvalue() for name in CANDIDATES))
    check("both assumptions are reported",
          "gamma 1.0" in cmp_out.getvalue() and "sRGB" in cmp_out.getvalue())
    check("a ranking that flips is called inconclusive, not a winner",
          ("Closest under both assumptions" in cmp_out.getvalue())
          != ("inconclusive" in cmp_out.getvalue()))

    print("\nan identical table scores zero against itself:")
    table = CANDIDATES["epdoptimize_spectra6"]
    cand = candidate_normalised(table, 1.0)
    check("self-distance is zero",
          all(abs(cand[c][i] - candidate_normalised(table, 1.0)[c][i]) < 1e-12
              for c in cand for i in range(3)))
    check("white maps to 1 and black to 0",
          all(abs(v - 1.0) < 1e-9 for v in cand["white"])
          and all(abs(v) < 1e-9 for v in cand["black"]))

    print()
    if failures:
        print("%d self-test failure(s): %s" % (len(failures), ", ".join(failures)))
        return 1
    print("all self-tests passed")
    return 0


# ----------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", nargs="?", default=None,
                    help="'extract' plus a directory or photographs")
    ap.add_argument("paths", nargs="*", help="photographs, or a directory of them")
    ap.add_argument("--rois", type=pathlib.Path, default=DEFAULT_ROIS)
    ap.add_argument("--log", action="append", default=[],
                    help="firmware log(s) from the run; globs accepted")
    ap.add_argument("-o", "--out", type=pathlib.Path,
                    help="CSV to write (default stdout)")
    ap.add_argument("--calibrate", type=pathlib.Path,
                    help="derive the sampling grid from one photograph")
    ap.add_argument("--panel", help="panel corners for --calibrate: 'TL TR BR BL' as "
                                    "x,y pairs")
    ap.add_argument("--cols", type=int, default=2)
    ap.add_argument("--rows", type=int, default=5)
    ap.add_argument("--report", type=pathlib.Path, help="report on an extracted CSV")
    ap.add_argument("--compare", type=pathlib.Path,
                    help="rank the published palettes against an extracted CSV")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        tmp = pathlib.Path(tempfile.mkdtemp(prefix="colour-selftest-"))
        try:
            return self_test(tmp)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    if args.calibrate:
        if not args.panel:
            ap.error("--calibrate needs --panel 'x,y x,y x,y x,y' (TL TR BR BL)")
        RUN_DIR.mkdir(parents=True, exist_ok=True)
        return calibrate(args.calibrate, parse_quad(args.panel), args.cols, args.rows,
                         args.rois, RUN_DIR / "rois-overlay.png")

    if args.report:
        return report(load_rows(args.report), sys.stdout)

    if args.compare:
        return compare(load_rows(args.compare), sys.stdout)

    if args.mode != "extract":
        ap.error("give a mode: extract, --calibrate, --report, --compare or --self-test")

    photos = []
    for path in args.paths:
        p = pathlib.Path(path)
        photos += [str(q) for q in p.glob("*chart-?-c*-t*.png")] if p.is_dir() else [path]
    if not photos:
        ap.error("no chart photographs found; pass a directory or a glob")

    logs = [f for pattern in args.log for f in glob.glob(pattern)]
    if not logs:
        sys.stderr.write("warning: no --log given; patches will have no colour name "
                         "and no normalisation\n")
    layouts, samples = parse_log(logs)

    out = open(args.out, "w", newline="") if args.out else sys.stdout
    try:
        written = extract(photos, read_rois(args.rois), layouts, samples, out)
    finally:
        if args.out:
            out.close()
    sys.stderr.write("%d rows from %d photographs\n" % (written, len(photos)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
