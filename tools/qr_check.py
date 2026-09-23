#!/usr/bin/env python3
"""Scan the panel on the flatbed and decode whatever QR codes are on it.

Ticket 30 needs to know that the pairing screen the panel draws is actually readable and
carries the right text. A phone is the real test and needs a person; this is the part of
it that can run at the bench, because the device already lies face-down on the scanner
glass and `render -> scan -> render -> scan` runs with nothing moved (ticket 20).

**What a pass here does and does not mean.** A flatbed is an easier reader than a phone:
even illumination, dead flat, high resolution, no perspective. So decoding here is
NECESSARY but not SUFFICIENT -- it says the payload is right and the module grid survived
the panel's rendering, not that a camera will manage it across a room. Failing here,
though, is decisive: what a flatbed cannot read, no phone will.

Two things that would otherwise produce a false "unreadable":

  * **The scan may be mirrored.** The device lies face-down, and QR decoders correct for
    rotation but not for reflection. Both the image and its mirror are tried, and the run
    says which one worked -- that is a fact about the bench worth knowing, not a detail to
    hide inside a retry.
  * **Resolution.** At 300 dpi one panel pixel is about 2.6 scan pixels, so a module drawn
    at scale 6 lands as roughly 16 px. Scanning lower to save time is the first thing to
    suspect if a decode fails; --dpi is exposed for exactly that experiment.

Usage:
    python tools/qr_check.py --expect-join 'WIFI:S:PaperColor-A1B2C3;T:nopass;;'
    python tools/qr_check.py --dpi 600 --keep

Any --expect-* value given is compared against the decoded set and reported; the token in
the pairing URL is deliberately not something this tool needs to be told.
"""

import argparse
import pathlib
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
SCAN_SCRIPT = REPO_ROOT / "tools" / "scan_panel.ps1"
# The panel's corner of the bed, from tools/bringup_capture.py's SCAN_REGION.
SCAN_REGION = ("-XInch", "0", "-YInch", "0", "-WInch", "3.5", "-HInch", "4.8")


def scan(out_path, dpi):
    cmd = [
        "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", str(SCAN_SCRIPT), "-Out", str(out_path), "-Dpi", str(dpi), *SCAN_REGION,
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    sys.stdout.write(proc.stdout)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        raise SystemExit("scan failed with %d" % proc.returncode)


def decode_all(img):
    """Every QR in the image, as a list of strings. [] when there are none."""
    import cv2

    det = cv2.QRCodeDetector()
    ok, texts, _pts, _straight = det.detectAndDecodeMulti(img)
    if not ok or texts is None:
        return []
    return [t for t in texts if t]


# **This output is kept in .scratch/, which is not git-ignored, and both payloads on the pairing
# screen are secrets**: the join URI carries the access point's WPA2 password and the pairing URL
# carries the API token. The --expect-url-prefix check below was always careful not to print the
# token; the raw dump above it printed both whole, which is the same defect the firmware had at
# ESP_LOG_INFO (ticket 66, docs/agents/defect-log.md). So nothing from a decoded string reaches
# stdout except its shape.
_SECRET_AFTER = ("P:", "?t=", "&t=")


def redact(text):
    out = text
    for marker in _SECRET_AFTER:
        i = out.find(marker)
        while i >= 0:
            start = i + len(marker)
            end = len(out)
            for stop in (";", "&", " "):
                j = out.find(stop, start)
                if j >= 0:
                    end = min(end, j)
            out = out[:start] + ("<%d chars>" % (end - start)) + out[end:]
            i = out.find(marker, start + 1)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dpi", type=int, default=300)
    ap.add_argument("--expect-join", default=None,
                    help="the WIFI: URI the panel should be showing")
    ap.add_argument("--expect-join-prefix", default=None,
                    help="e.g. WIFI:S:PaperFrame-A1B2C3;T:WPA;P= -- checks the SSID and the "
                         "authmode without needing the password on a command line")
    ap.add_argument("--expect-url-prefix", default=None,
                    help="e.g. http://192.168.4.1/?t= -- the token itself is not needed")
    ap.add_argument("--image", default=None,
                    help="decode this file instead of scanning (for re-analysis)")
    ap.add_argument("--keep", action="store_true", help="keep the scan next to the repo")
    args = ap.parse_args()

    import cv2

    if args.image:
        path = pathlib.Path(args.image)
    elif args.keep:
        path = REPO_ROOT / ".scratch" / "captures" / ("qr-scan-%d.bmp" % args.dpi)
        path.parent.mkdir(parents=True, exist_ok=True)
        scan(path, args.dpi)
    else:
        path = pathlib.Path(tempfile.gettempdir()) / "qr-scan.bmp"
        scan(path, args.dpi)

    img = cv2.imread(str(path))
    if img is None:
        raise SystemExit("could not read %s" % path)
    print("image %dx%d from %s" % (img.shape[1], img.shape[0], path))

    # Raw results for both orientations, printed whichever wins -- the verdict is computed
    # below from these rather than the loop stopping at the first success and hiding the
    # other. A device face-down on the glass is the normal case here, so which of the two
    # decodes is itself a measurement about the bench.
    found = {"as-scanned": decode_all(img), "mirrored": decode_all(cv2.flip(img, 1))}
    for how, texts in found.items():
        shown = [redact(t) for t in texts]
        print("  %-12s decoded %d: %s" % (how, len(texts), shown if shown else "-"))

    texts = found["as-scanned"] or found["mirrored"]
    if not texts:
        print("\nNO QR DECODED. Before concluding the panel cannot render one: try a "
              "higher --dpi, and check the panel is actually showing the pairing screen.")
        return 2

    rc = 0
    if args.expect_join is not None:
        hit = args.expect_join in texts
        print("\njoin URI   expected: %s\n           %s" % (args.expect_join,
              "FOUND" if hit else "NOT FOUND"))
        rc |= 0 if hit else 1
    if args.expect_join_prefix is not None:
        hits = [t for t in texts if t.startswith(args.expect_join_prefix)]
        print("\njoin URI prefix: %s\n           %s" % (
            args.expect_join_prefix,
            ("FOUND, %d chars after the prefix" % (len(hits[0]) - len(args.expect_join_prefix)))
            if hits else "NOT FOUND"))
        rc |= 0 if hits else 1
    if args.expect_url_prefix is not None:
        hits = [t for t in texts if t.startswith(args.expect_url_prefix)]
        # The token is not printed: this output is kept in .scratch/.
        print("pairing URL prefix: %s\n           %s" % (
            args.expect_url_prefix,
            ("FOUND, %d chars after the prefix" % (len(hits[0]) - len(args.expect_url_prefix)))
            if hits else "NOT FOUND"))
        rc |= 0 if hits else 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
