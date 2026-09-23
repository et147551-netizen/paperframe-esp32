#!/usr/bin/env python3
"""Render a PNG through the firmware's own quantiser and write a preview PNG.

Answers "what will this photograph look like on the panel" without a 16-second refresh
and a webcam. The colour decisions come from src/core/epd_epdopt.c and src/core/epd_dither.c
compiled for the host -- this script only moves pixels in and out of them, so what you see
is what the firmware does.

    python tools/render_preview.py .scratch/digital-frame/fixtures/imaged003.png
    python tools/render_preview.py photo.png --simulate
    python tools/render_preview.py photo.png --palette epdopt-original
    python tools/render_preview.py photo.png --palette manual
    python tools/render_preview.py photo.png --mode epdopt --accurate

A bare invocation previews exactly what the frame ships: the M5GFX pair search against
epdoptimize's aitjcize calibration. The `epdopt` modes are the ported epdoptimize pipeline,
which the device does NOT run -- 24 s a photograph against 1.6 s. See
docs/measurements.md.

Output goes next to the input under .scratch/renders/ as <stem>-<mode>.png, at the panel
palette's ideal RGB values -- so it will look more saturated than the real panel, which
is reflective and dark. Judge structure, texture and colour *choices* here; judge actual
colour with a colorimeter and tools/colour_check.py.

**IT READS TRUECOLOUR PNG ONLY -- colour types 2 and 6 -- AND THAT IS NARROWER THAN IT SOUNDS.**
Anything else exits with `unsupported PNG: bitdepth=… colourtype=…`. JPEG is the obvious
exclusion and everyone expects it; the one that surprises is **greyscale**. On 2026-09-20 this
cost ticket 70 twelve of its twenty-six fixtures: eleven JPEGs, and `VerlaufGrau.png`, which is
colour type 0 and which the device renders perfectly well. So a fixture this tool cannot read is
not a fixture the firmware cannot read, and a coverage claim phrased as "N of M measured" has to
say which M. Convert first -- `.scratch/digital-frame/conv600.ps1` writes truecolour PNG at the
panel's long edge, losslessly, which is also what makes the conversion safe to measure through.

Standard library only; shells out to clang.
"""

import argparse
import pathlib
import struct
import subprocess
import sys
import zlib

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT_DIR = REPO_ROOT / ".scratch" / "renders"
BIN_DIR = REPO_ROOT / ".scratch" / "renders" / "_bin"

CLANG_CANDIDATES = [
    pathlib.Path(r"C:\Program Files\LLVM\bin\clang.exe"),
    pathlib.Path(r"C:\Program Files (x86)\LLVM\bin\clang.exe"),
    pathlib.Path("clang"),
]

# Must match EPD_PALETTE in src/core/epd_dither.c. Only used to turn indices back into
# something viewable; the firmware never needs this direction.
#
# These are *device* colours -- what gets sent to the panel -- and a preview drawn with
# them flatters the result badly, because the panel cannot produce any of them. See
# SIMULATED below.
PALETTE_DEVICE = {
    0x0: (0, 0, 0),
    0x1: (255, 255, 255),
    0x2: (255, 243, 56),
    0x3: (191, 0, 0),
    0x5: (100, 64, 255),
    0x6: (67, 138, 28),
}

# What the ink actually looks like. Spectra 6 cannot reach white or black: its white sits
# around L* 66 and its red around L* 26, so a preview drawn in device colours is a
# picture of an image the display will never show.
#
# Values from paperlesspaper/epdoptimize's `spectra6` calibrated palette (Apache-2.0),
# https://github.com/paperlesspaper/epdoptimize -- an independent measurement, and the
# only complete six-colour set to hand.
#
# It agrees with this panel's own datasheet where the two overlap. The EL040EF1 module
# manual quotes optical characteristics in L*a*b* (they are the acceptance targets in
# tools/colour_check.py); converted to sRGB they are white (154,164,162) against
# epdoptimize's (185,199,201), and red (123,25,19) against (98,32,30). Both sources say
# the same structural thing that the device palette does not: the white is a mid grey and
# the red is dark and unsaturated.
#
# Still generic, not this unit. Measuring our own panel with the colorimeter is
# .scratch/digital-frame/issues/19.
# Superseded by a measurement of OUR panel -- ticket 20, six refresh cycles through the
# fixed camera, normalised against white and black patches inside each photograph. These
# are EPD_PALETTE_MEASURED in src/core/epd_dither.c and must be kept equal to it: the preview
# is only worth looking at if it draws what the firmware matched against.
PALETTE_SIMULATED = {
    0x0: (33, 29, 47),
    0x1: (154, 164, 162),
    0x2: (173, 170, 96),
    0x3: (104, 22, 33),
    0x5: (28, 67, 130),
    0x6: (43, 73, 77),
}

# epdoptimize's five Spectra 6 calibrations, which are what the `epdopt` modes quantise
# against. Must equal the EPD_PALETTE_EPDOPT_* tables in src/core/epd_dither.c -- with
# --simulate these ARE the colours the quantiser matched against, so a preview drawn with
# them is the closest thing this tool can show to the glass.
PALETTE_EPDOPT = {
    "epdopt-aitjcize": {
        0x0: (2, 2, 2), 0x1: (190, 200, 200), 0x5: (5, 64, 158),
        0x6: (39, 102, 60), 0x3: (135, 19, 0), 0x2: (205, 202, 0),
    },
    "epdopt-spectra6": {
        0x0: (31, 34, 38), 0x1: (185, 199, 201), 0x5: (35, 63, 142),
        0x6: (53, 86, 58), 0x3: (98, 32, 30), 0x2: (193, 187, 30),
    },
    "epdopt-legacy": {
        0x0: (25, 30, 33), 0x1: (232, 232, 232), 0x5: (33, 87, 186),
        0x6: (18, 95, 32), 0x3: (178, 19, 24), 0x2: (239, 222, 68),
    },
    "epdopt-boeber": {
        0x0: (31, 34, 38), 0x1: (214, 214, 214), 0x5: (65, 108, 225),
        0x6: (6, 116, 6), 0x3: (234, 72, 67), 0x2: (219, 213, 41),
    },
    "epdopt-original": {
        0x0: (0, 0, 0), 0x1: (255, 255, 255), 0x5: (0, 0, 255),
        0x6: (0, 255, 0), 0x3: (255, 0, 0), 0x2: (255, 255, 0),
    },
}

ROW_MODES = ("quality", "none", "auto")
EPDOPT_MODES = ("epdopt", "epdopt-nearest")
ROW_PALETTES = ("stock", "measured-none", "measured-half", "measured-full", "manual")


def find_clang():
    for c in CLANG_CANDIDATES:
        if c.name == "clang" or c.is_file():
            return str(c)
    sys.exit("clang not found; see tools/native_toolchain.py for where it lives")


def build_renderer():
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    exe = BIN_DIR / "render_preview.exe"
    src = REPO_ROOT / "tools" / "render_preview.c"
    sources = [src] + [REPO_ROOT / "src" / "core" / name for name in
                       ("epd_dither.c", "epd_canvas.c", "epd_colour.c", "epd_epdopt.c",
                        "epd_adjust.c", "epd_auto.c", "epd_classify.c", "epd_diffuse.c",
                        # epd_flow.c since 2026-09-21: render_preview.c calls epd_flow_apply()
                        # instead of writing the six stages out itself. This list is hardcoded, so
                        # a new src/core/ dependency has to be added here or the link fails.
                        "epd_flow.c")]

    # Rebuild whenever any source is newer than the binary, so an edit to the quantiser
    # cannot be previewed with a stale one.
    newest = max(s.stat().st_mtime for s in sources)
    if exe.exists() and exe.stat().st_mtime > newest:
        return exe

    cmd = [find_clang(), "-std=c11", "-O2", "-Wall", "-Wextra",
           f"-I{REPO_ROOT / 'src' / 'core'}", *[str(s) for s in sources], "-o", str(exe)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit(f"build failed:\n{proc.stderr}")
    return exe


def decode_png(path):
    """Minimal PNG reader: 8-bit truecolour, with or without alpha, non-interlaced."""
    d = path.read_bytes()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(f"{path} is not a PNG")

    pos, idat, w, h, ct = 8, b"", None, None, None
    while pos < len(d):
        (ln,) = struct.unpack(">I", d[pos:pos + 4])
        typ = d[pos + 4:pos + 8]
        body = d[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b"IHDR":
            w, h, bd, ct, _, _, il = struct.unpack(">IIBBBBB", body)
            if bd != 8 or ct not in (2, 6) or il != 0:
                sys.exit(f"unsupported PNG: bitdepth={bd} colourtype={ct} interlace={il}")
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break

    bpp = 3 if ct == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if f:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                if f == 1:
                    line[x] = (line[x] + a) & 0xFF
                elif f == 2:
                    line[x] = (line[x] + b) & 0xFF
                elif f == 3:
                    line[x] = (line[x] + ((a + b) >> 1)) & 0xFF
                elif f == 4:
                    pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                    pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                    line[x] = (line[x] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line

    if bpp == 3:
        return w, h, bytes(out)

    # Composite alpha against white, matching the white matte the frame draws behind
    # every image (FR-5.3).
    rgb = bytearray(w * h * 3)
    for i in range(w * h):
        r, g, b, a = out[i * 4:i * 4 + 4]
        rgb[i * 3 + 0] = (r * a + 255 * (255 - a)) // 255
        rgb[i * 3 + 1] = (g * a + 255 * (255 - a)) // 255
        rgb[i * 3 + 2] = (b * a + 255 * (255 - a)) // 255
    return w, h, bytes(rgb)


def encode_png(path, w, h, rgb):
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)  # filter: none
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(typ, body):
        return (struct.pack(">I", len(body)) + typ + body +
                struct.pack(">I", zlib.crc32(typ + body) & 0xFFFFFFFF))

    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image", type=pathlib.Path)
    ap.add_argument("--mode", choices=[*EPDOPT_MODES, *ROW_MODES], default="quality",
                    help="quality and none are M5GFX's row-wise paths and are what the "
                         "frame renders with -- FR-3.3's two colour-reduction modes; "
                         "auto adds epdoptimize's own per-image auto flow in front of "
                         "them, which is what the frame runs when auto_adjust is on and "
                         "what its demo site does by default -- it prints the plan it "
                         "chose to stderr; epdopt is the ported epdoptimize pipeline, "
                         "which the device does NOT run (24 s a photograph), and "
                         "epdopt-nearest is the same without the error diffusion")
    ap.add_argument("--out-dir", type=pathlib.Path, default=OUT_DIR)
    ap.add_argument("--palette",
                    choices=[*PALETTE_EPDOPT.keys(), *ROW_PALETTES],
                    default=None,
                    help="what the quantiser matches against. The epdopt-* options are "
                         "epdoptimize's five calibrations of this panel and default to "
                         "epdopt-aitjcize, the one its demo selects. The others go with "
                         "--mode quality/none: 'stock' is the device colours the firmware "
                         "always used, and the 'measured' three are this panel's own "
                         "appearance with no, half and full tone compression (ticket 19)")
    ap.add_argument("--accurate", action="store_true",
                    help="use epdoptimize's L*a*b* range compressor -- its own default -- "
                         "instead of the fast one the frame ships. Host only: on the device "
                         "it ran over 90 s on one photograph and tripped the task watchdog "
                         "(2026-09-05). This tool is the only place it can be looked at")
    ap.add_argument("--serpentine", action="store_true",
                    help="reverse every other row of the error diffusion (epdopt modes "
                         "only). Plain Floyd-Steinberg leaves diagonal banding in a smooth "
                         "gradient -- a sky is where it shows -- and this is the standard "
                         "remedy")
    ap.add_argument("--diffuse", action="store_true",
                    help="quantise with Floyd-Steinberg error diffusion (src/core/epd_diffuse.c) "
                         "instead of M5GFX's row-wise pair search -- the frame's "
                         "`dither_diffuse` setting, and what epdoptimize's demo does. Applies "
                         "to --mode quality and --mode auto; it has no meaning on `none`, "
                         "because nearest is nearest on either quantiser. Combine with "
                         "--serpentine, which --mode auto turns on by itself")
    ap.add_argument("--simulate", action="store_true",
                    help="draw the preview in the ink's measured appearance rather than "
                         "in device colours -- much closer to what the glass shows")
    args = ap.parse_args()

    # The two mode families take different palettes, and crossing them is a mistake the
    # renderer rejects. Resolve the default here so neither has to be spelled out.
    epdopt = args.mode in EPDOPT_MODES
    if args.palette is None:
        # The frame's own default, so a bare invocation previews what ships.
        args.palette = "epdopt-aitjcize"
    # An epdopt palette is allowed with a row mode -- that crossing separates the library's
    # calibration from its algorithm, and the algorithm is the expensive half (24 s against
    # 1.6 s on the device). The reverse is not allowed: the epdopt modes carry their own tone
    # and range handling and have nowhere to put an EPD_RENDER_* config.
    if not epdopt and args.palette in PALETTE_EPDOPT:
        pass
    elif epdopt != (args.palette in PALETTE_EPDOPT):
        sys.exit(f"--mode {args.mode} does not take --palette {args.palette}")
    if args.accurate and not epdopt:
        sys.exit("--accurate applies to the epdopt modes only")
    if args.serpentine and not (epdopt or args.diffuse):
        sys.exit("--serpentine needs the epdopt modes or --diffuse")
    if args.diffuse and args.mode not in ("quality", "auto"):
        sys.exit("--diffuse applies to --mode quality and --mode auto")

    exe = build_renderer()
    w, h, rgb = decode_png(args.image)

    cmd = [str(exe), args.mode, str(w), str(h), args.palette]
    if args.serpentine:
        cmd.append("serpentine")
    if args.accurate:
        cmd.append("accurate")
    if args.diffuse:
        cmd.append("diffuse")
    proc = subprocess.run(cmd, input=rgb, capture_output=True)
    if proc.returncode != 0:
        sys.exit(proc.stderr.decode("utf-8", "replace"))
    # --mode auto reports what it decided on stderr, and that is half the point of the mode:
    # a colour question wants to know which kind the classifier picked, not only what came out.
    if proc.stderr:
        sys.stderr.write(proc.stderr.decode("utf-8", "replace"))

    if not args.simulate:
        palette = PALETTE_DEVICE
    elif args.palette in PALETTE_EPDOPT:
        # With an epdopt palette there is no separate "measured appearance" table to
        # simulate: the palette's own colours are that measurement, so --simulate draws
        # exactly what the quantiser matched against.
        palette = PALETTE_EPDOPT[args.palette]
    else:
        palette = PALETTE_SIMULATED
    packed = proc.stdout
    row_packed = (w + 1) // 2
    out_rgb = bytearray(w * h * 3)
    unrenderable = 0
    for y in range(h):
        base = y * row_packed
        for x in range(w):
            byte = packed[base + (x >> 1)]
            idx = (byte >> 4) if (x & 1) == 0 else (byte & 0x0F)
            colour = palette.get(idx)
            if colour is None:
                unrenderable += 1
                colour = (255, 0, 255)  # magenta: impossible on this panel, so visible
            i = (y * w + x) * 3
            out_rgb[i:i + 3] = bytes(colour)

    args.out_dir.mkdir(parents=True, exist_ok=True)
    suffix = (("-accurate" if args.accurate else "")
              + ("-diffuse" if args.diffuse else "")
              + ("-serpentine" if args.serpentine else "")
              + ("-simulated" if args.simulate else ""))
    out_path = args.out_dir / f"{args.image.stem}-{args.mode}-{args.palette}{suffix}.png"
    encode_png(out_path, w, h, bytes(out_rgb))

    print(out_path)
    if unrenderable:
        print(f"!! {unrenderable} pixels carried an index this panel cannot render",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
