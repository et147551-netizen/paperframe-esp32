#!/usr/bin/env python3
"""Photograph the panel with the fixed USB webcam.

The panel is reflective and does not emit light, so what the camera records is
the room as much as it is the display. This script exists to make the *capture*
repeatable; it does not make the result colorimetric. For colour acceptance use
a colorimeter and tools/colour_check.py -- a webcam cannot settle whether a
patch is within dE2000 6 of the target, and should not be asked to.

What it is good for: "did the thing I asked for actually appear on the glass".
That question is unanswerable from the serial log alone, because a refresh can
report success while the panel shows nothing.

    python tools/capture_panel.py stock-firmware
    python tools/capture_panel.py colour-white --device "UGREEN Camera"
    python tools/capture_panel.py --list

Standard library only; shells out to ffmpeg.
"""

import argparse
import datetime
import pathlib
import subprocess
import sys

DEFAULT_DEVICE = "UGREEN Camera"
DEFAULT_SIZE = "1920x1080"

# Frames to throw away before keeping one. A webcam's first frames are captured
# while auto-exposure and auto-white-balance are still converging: they come out
# dark and colour-shifted. Comparing such a frame against a later one shows a
# difference that is entirely the camera's. 20 frames is ~0.7 s at 30 fps.
WARMUP_FRAMES = 20

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
CAPTURE_DIR = REPO_ROOT / ".scratch" / "captures"


def list_devices():
    """Print the DirectShow devices ffmpeg can see."""
    proc = subprocess.run(
        ["ffmpeg", "-hide_banner", "-list_devices", "true", "-f", "dshow", "-i", "dummy"],
        capture_output=True,
        text=True,
    )
    # ffmpeg reports the device list on stderr and then exits non-zero, which is
    # its documented behaviour for this query and not an error worth reporting.
    sys.stdout.write(proc.stderr)
    return 0


def _run_ffmpeg(device, size, warmup, pixel_format, path):
    cmd = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel", "error",
        "-y",
        "-f", "dshow",
        "-video_size", size,
    ]
    if pixel_format:
        # Ahead of -i, so it is an input option: it asks the device for that format
        # rather than converting after the fact.
        cmd += ["-pixel_format", pixel_format]
    cmd += [
        "-i", f"video={device}",
        # Keep the first frame at or after `warmup`, then stop.
        "-vf", f"select=gte(n\\,{warmup})",
        "-frames:v", "1",
        "-update", "1",
        str(path),
    ]
    return subprocess.run(cmd, capture_output=True, text=True)


def capture(label, device, size, warmup, out_dir, shots=1, pixel_format=None):
    out_dir.mkdir(parents=True, exist_ok=True)

    for shot in range(1, shots + 1):
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        # The suffix only appears for a multi-shot capture, so single shots keep the
        # filenames every earlier run used.
        suffix = f"-s{shot}" if shots > 1 else ""
        path = out_dir / f"{label}{suffix}-{stamp}.png"

        proc = _run_ffmpeg(device, size, warmup, pixel_format, path)
        if pixel_format and (proc.returncode != 0 or not path.exists()):
            # Not every camera offers every format, and a run that dies here loses a
            # scheduled sample it cannot go back for. Say what happened and take the
            # picture the way that works.
            sys.stderr.write(f"pixel_format {pixel_format} refused; falling back\n")
            proc = _run_ffmpeg(device, size, warmup, None, path)

        if proc.returncode != 0 or not path.exists():
            sys.stderr.write(f"capture failed ({proc.returncode})\n{proc.stderr}\n")
            return 1

        print(path)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("label", nargs="?",
                    help="short slug describing what is on screen, e.g. colour-red")
    ap.add_argument("--device", default=DEFAULT_DEVICE)
    ap.add_argument("--size", default=DEFAULT_SIZE)
    ap.add_argument("--warmup", type=int, default=WARMUP_FRAMES)
    ap.add_argument("--out-dir", type=pathlib.Path, default=CAPTURE_DIR)
    ap.add_argument("--shots", type=int, default=1,
                    help="photographs to take of the same unchanged panel. Their "
                         "spread is the camera-and-room noise floor, which is what "
                         "any claim about the panel changing has to beat")
    ap.add_argument("--pixel-format",
                    help="ask the device for this format, e.g. yuyv422, so patch "
                         "colour is not read through MJPEG chroma subsampling. Falls "
                         "back to the default if the camera refuses")
    ap.add_argument("--list", action="store_true", help="list capture devices and exit")
    args = ap.parse_args()

    if args.list:
        return list_devices()
    if not args.label:
        ap.error("label is required (or pass --list)")
    if args.shots < 1:
        ap.error("--shots must be at least 1")

    return capture(args.label, args.device, args.size, args.warmup, args.out_dir,
                   args.shots, args.pixel_format)


if __name__ == "__main__":
    sys.exit(main())
