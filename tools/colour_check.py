#!/usr/bin/env python3
"""Score colorimeter readings against the panel's own optical spec.

Turns "does the image still look OK at FRS 0x07" into a pass/fail measurement. The
targets are the EL040EF1 module manual's, p17, all at 25 C -- see research §9.2.

    python tools/colour_check.py measured.csv
    python tools/colour_check.py --targets          # emit a template to fill in
    python tools/colour_check.py --self-test        # no hardware needed

Input CSV:  state,L,a,b   with one row per patch, states as named in TARGETS.

Caveat that travels with every result this produces, from the same manual (Note 6-6):
the listed optical characteristics are guaranteed only under the controller and
waveform provided by E Ink. Changing FRS changes waveform playback speed, so at
0x07 these are acceptance targets we have chosen, not guarantees the vendor makes.

Standard library only.
"""

import argparse
import csv
import math
import sys

# state -> (L*, a*, b*, dE2000 limit). Green's limit is 8; everything else is 6.
TARGETS = {
    "white":  (66.5, -4.0, 0.0, 6.0),
    "dark":   (12.0, 7.0, -11.0, 6.0),
    "red":    (26.5, 41.0, 30.0, 6.0),
    "yellow": (62.0, -11.0, 65.0, 6.0),
    "blue":   (34.0, 3.5, -37.0, 6.0),
    "green":  (35.0, -22.0, 15.0, 8.0),
}

CONTRAST_MIN = 15.0        # typ 22
WHITE_REFLECTANCE_MIN = 30.0  # %, typ 34


def ciede2000(lab1, lab2):
    """CIEDE2000 colour difference, kL = kC = kH = 1.

    Verified against the Sharma / Wu / Dalal test data in self_test(); a wrong
    implementation here would mis-grade the panel silently, which is worse than not
    measuring at all.
    """
    l1, a1, b1 = lab1
    l2, a2, b2 = lab2

    c1 = math.hypot(a1, b1)
    c2 = math.hypot(a2, b2)
    c_bar = (c1 + c2) / 2.0
    c_bar7 = c_bar ** 7
    g = 0.5 * (1.0 - math.sqrt(c_bar7 / (c_bar7 + 25.0 ** 7)))

    a1p = (1.0 + g) * a1
    a2p = (1.0 + g) * a2
    c1p = math.hypot(a1p, b1)
    c2p = math.hypot(a2p, b2)

    def hue(ap, b):
        if ap == 0.0 and b == 0.0:
            return 0.0
        return math.degrees(math.atan2(b, ap)) % 360.0

    h1p = hue(a1p, b1)
    h2p = hue(a2p, b2)

    dlp = l2 - l1
    dcp = c2p - c1p

    if c1p * c2p == 0.0:
        dhp = 0.0
    elif abs(h2p - h1p) <= 180.0:
        dhp = h2p - h1p
    elif h2p - h1p > 180.0:
        dhp = h2p - h1p - 360.0
    else:
        dhp = h2p - h1p + 360.0
    dhp_cap = 2.0 * math.sqrt(c1p * c2p) * math.sin(math.radians(dhp) / 2.0)

    lbp = (l1 + l2) / 2.0
    cbp = (c1p + c2p) / 2.0

    if c1p * c2p == 0.0:
        hbp = h1p + h2p
    elif abs(h1p - h2p) <= 180.0:
        hbp = (h1p + h2p) / 2.0
    elif h1p + h2p < 360.0:
        hbp = (h1p + h2p + 360.0) / 2.0
    else:
        hbp = (h1p + h2p - 360.0) / 2.0

    t = (1.0
         - 0.17 * math.cos(math.radians(hbp - 30.0))
         + 0.24 * math.cos(math.radians(2.0 * hbp))
         + 0.32 * math.cos(math.radians(3.0 * hbp + 6.0))
         - 0.20 * math.cos(math.radians(4.0 * hbp - 63.0)))

    dtheta = 30.0 * math.exp(-(((hbp - 275.0) / 25.0) ** 2))
    cbp7 = cbp ** 7
    rc = 2.0 * math.sqrt(cbp7 / (cbp7 + 25.0 ** 7))

    sl = 1.0 + (0.015 * (lbp - 50.0) ** 2) / math.sqrt(20.0 + (lbp - 50.0) ** 2)
    sc = 1.0 + 0.045 * cbp
    sh = 1.0 + 0.015 * cbp * t
    rt = -math.sin(math.radians(2.0 * dtheta)) * rc

    return math.sqrt((dlp / sl) ** 2 + (dcp / sc) ** 2 + (dhp_cap / sh) ** 2
                     + rt * (dcp / sc) * (dhp_cap / sh))


def load(stream):
    out = {}
    for row in csv.DictReader(l for l in stream if not l.lstrip().startswith("#")):
        out[row["state"].strip().lower()] = (
            float(row["L"]), float(row["a"]), float(row["b"]))
    return out


def report(measured, out, contrast=None, reflectance=None):
    failures = []
    out.write("%-8s %8s %8s %8s   %7s %6s  %s\n"
              % ("state", "L*", "a*", "b*", "dE2000", "limit", "verdict"))

    for state, (tl, ta, tb, limit) in TARGETS.items():
        if state not in measured:
            out.write("%-8s %8s %8s %8s   %7s %6.1f  NOT MEASURED\n"
                      % (state, "-", "-", "-", "-", limit))
            failures.append("%s not measured" % state)
            continue
        lab = measured[state]
        de = ciede2000((tl, ta, tb), lab)
        ok = de <= limit
        out.write("%-8s %8.2f %8.2f %8.2f   %7.3f %6.1f  %s\n"
                  % (state, lab[0], lab[1], lab[2], de, limit,
                     "pass" if ok else "FAIL"))
        if not ok:
            failures.append("%s dE2000 %.3f > %.1f" % (state, de, limit))

    unknown = set(measured) - set(TARGETS)
    if unknown:
        out.write("\nignored unknown states: %s\n" % ", ".join(sorted(unknown)))

    if contrast is not None:
        ok = contrast >= CONTRAST_MIN
        out.write("\ncontrast ratio %.1f (min %.0f, typ 22)  %s\n"
                  % (contrast, CONTRAST_MIN, "pass" if ok else "FAIL"))
        if not ok:
            failures.append("contrast %.1f < %.0f" % (contrast, CONTRAST_MIN))

    if reflectance is not None:
        ok = reflectance >= WHITE_REFLECTANCE_MIN
        out.write("white reflectance %.1f%% (min %.0f%%, typ 34%%)  %s\n"
                  % (reflectance, WHITE_REFLECTANCE_MIN, "pass" if ok else "FAIL"))
        if not ok:
            failures.append("reflectance %.1f%% < %.0f%%"
                            % (reflectance, WHITE_REFLECTANCE_MIN))

    out.write("\n")
    if failures:
        out.write("RESULT: FAIL (%d)\n" % len(failures))
        for f in failures:
            out.write("  - %s\n" % f)
    else:
        out.write("RESULT: PASS -- all patches inside the manual's dE2000 limits\n")

    out.write("\nNote 6-6: these characteristics are guaranteed only under E Ink's own\n"
              "controller and waveform. Changing FRS changes playback speed, so this is\n"
              "an acceptance test we chose, not a vendor guarantee.\n")
    return len(failures)


def self_test():
    failures = []

    def check(name, condition, detail=""):
        if condition:
            print("  PASS  %s" % name)
        else:
            print("  FAIL  %s %s" % (name, detail))
            failures.append(name)

    # Sharma, Wu & Dalal (2005), "The CIEDE2000 Color-Difference Formula", Table 1.
    # These pairs are the standard implementation check; several exist specifically
    # to catch the hue-rotation and mean-hue-wraparound branches.
    print("CIEDE2000 against the Sharma et al. reference pairs:")
    reference = [
        ((50.0000, 2.6772, -79.7751), (50.0000, 0.0000, -82.7485), 2.0425),
        ((50.0000, 3.1571, -77.2803), (50.0000, 0.0000, -82.7485), 2.8615),
        ((50.0000, -1.3802, -84.2814), (50.0000, 0.0000, -82.7485), 1.0000),
        ((50.0000, 0.0000, 0.0000), (50.0000, -1.0000, 2.0000), 2.3669),
        ((60.2574, -34.0099, 36.2677), (60.4626, -34.1751, 39.4387), 1.2644),
        ((22.7233, 20.0904, -46.6940), (23.0331, 14.9730, -42.5619), 2.0373),
        ((90.9257, -0.5406, -0.9208), (88.6381, -0.8985, -0.7239), 1.5381),
        ((2.0776, 0.0795, -1.1350), (0.9033, -0.0636, -0.5514), 0.9082),
    ]
    worst = 0.0
    for lab1, lab2, expected in reference:
        got = ciede2000(lab1, lab2)
        worst = max(worst, abs(got - expected))
    check("all %d pairs within 1e-4" % len(reference), worst < 1e-4,
          "worst error %.6f" % worst)

    print("\nthe manual's own targets score a perfect pass:")
    exact = {s: (v[0], v[1], v[2]) for s, v in TARGETS.items()}
    import io
    buf = io.StringIO()
    n = report(exact, buf)
    check("zero failures", n == 0, buf.getvalue())
    check("RESULT: PASS printed", "RESULT: PASS" in buf.getvalue())
    check("Note 6-6 caveat travels with the result", "Note 6-6" in buf.getvalue())

    print("\nnudging one patch across its limit fails it, and only it:")
    # Blue's limit is 6. Move L* far enough to clear it.
    nudged = dict(exact)
    nudged["blue"] = (34.0 + 12.0, 3.5, -37.0)
    buf = io.StringIO()
    n = report(nudged, buf)
    check("exactly one failure", n == 1, "got %d" % n)
    check("it is blue", "blue dE2000" in buf.getvalue())

    print("\ngreen's limit is 8 and is distinguishable from the other five at 6:")
    # A deviation that fails at a limit of 6 but passes at 8.
    de_probe = None
    for dl in [x * 0.1 for x in range(1, 300)]:
        d = ciede2000(TARGETS["green"][:3], (35.0 + dl, -22.0, 15.0))
        if 6.0 < d < 8.0:
            de_probe = (dl, d)
            break
    check("found a deviation between the two limits", de_probe is not None)
    if de_probe:
        dl, d = de_probe
        buf = io.StringIO()
        n = report({**exact, "green": (35.0 + dl, -22.0, 15.0)}, buf)
        check("green passes at dE2000 %.3f" % d, n == 0, buf.getvalue())
        # The same deviation applied to a limit-6 state must fail.
        buf = io.StringIO()
        base = TARGETS["blue"][:3]
        probe_blue = None
        for x in [v * 0.1 for v in range(1, 300)]:
            if 6.0 < ciede2000(base, (base[0] + x, base[1], base[2])) < 8.0:
                probe_blue = (base[0] + x, base[1], base[2])
                break
        check("a same-sized deviation on blue (limit 6) FAILS",
              probe_blue is not None
              and report({**exact, "blue": probe_blue}, io.StringIO()) == 1)

    print("\na missing patch is a failure, not a silent skip:")
    partial = dict(exact)
    del partial["yellow"]
    buf = io.StringIO()
    n = report(partial, buf)
    check("missing patch counted", n == 1, "got %d" % n)
    check("reported as NOT MEASURED", "NOT MEASURED" in buf.getvalue())

    print("\ncontrast and reflectance limits bite:")
    buf = io.StringIO()
    check("contrast 14 fails", report(exact, buf, contrast=14.0) == 1)
    buf = io.StringIO()
    check("contrast 22 passes", report(exact, buf, contrast=22.0) == 0)
    buf = io.StringIO()
    check("reflectance 28% fails", report(exact, buf, reflectance=28.0) == 1)

    print()
    if failures:
        print("%d self-test failure(s): %s" % (len(failures), ", ".join(failures)))
        return 1
    print("all self-tests passed")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="?", help="measured patches; '-' for stdin")
    ap.add_argument("--contrast", type=float, help="measured contrast ratio")
    ap.add_argument("--reflectance", type=float, help="measured white reflectance, %%")
    ap.add_argument("--targets", action="store_true",
                    help="print a template CSV holding the manual's target values")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if args.targets:
        print("state,L,a,b")
        for state, (l, a, b, _) in TARGETS.items():
            print("%s,%.1f,%.1f,%.1f" % (state, l, a, b))
        return 0
    if not args.csv:
        ap.error("give a CSV path, --targets, or --self-test")

    stream = sys.stdin if args.csv == "-" else open(args.csv)
    with stream:
        measured = load(stream)
    return 1 if report(measured, sys.stdout, args.contrast, args.reflectance) else 0


if __name__ == "__main__":
    sys.exit(main())
