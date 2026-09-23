#!/usr/bin/env python3
"""Turn the harness CSV into medians, spread and H1/H2/H5 verdicts.

Thresholds come from docs/phase0-preregistration.md and are duplicated here as
constants. They were fixed before any hardware ran; changing one means amending that
document and saying why, not editing this file quietly.

    python tools/analyse.py run.csv
    python tools/analyse.py --self-test      # no hardware needed

Standard library only, so it runs under any Python on the machine.
"""

import argparse
import csv
import io
import statistics
import sys

# --- pre-registered thresholds -------------------------------------------------

H1_PON_FLOOR_MS = 80.0  # panel timing diagram: >80 ms minimum between PON and DRF
H2_XFER_PREDICTED_MS = 240.0  # 120,000 bytes at 4 MHz
H2_TOLERANCE = 0.15
H2_XFER_RATIO_DECISION = 0.02  # below this, DMA/clock work is not worth doing
H5_EXPECTED_C_MS = 918.0  # least-squares refit over 0x03-0x07, NOT the 550 ms in
                          # the research document, which anchored on two points
H5_AGREEMENT = 0.20

PHASES = ["t_reset_ms", "t_init_ms", "t_xfer_ms", "t_pon_ms", "t_drf_ms", "t_pof_ms",
          "t_total_ms"]


def frs_rate_hz(a):
    """The observed-table reading of the FRS encoding. See preregistration H4."""
    explicit = {0x39: 200.0, 0x3A: 100.0, 0x3C: 50.0}
    if a in explicit:
        return explicit[a]
    if a <= 0x07:
        return 12.5 * (a + 1)
    return 50.0


# The sentinel harness_main.c and bringup_main.c write when
# epd_read_temperature() fails. It is not a temperature and must never be
# summarised as one.
TEMP_UNAVAILABLE = -128


# Two headers, because issues/09 appended `seq` and `t_pon2drf_ms` to the end of a
# schema that ticket 08's and ticket 17's logs were written under. Both must load: a
# tool that can only read the logs written after its own last change is a tool that
# quietly retires the evidence base.
HEADER_V1 = "run,frs,temp_c," + ",".join(PHASES)
HEADER_V2 = HEADER_V1 + ",seq,t_pon2drf_ms"
HEADERS = (HEADER_V1, HEADER_V2)

# What a v1 row is. It was taken before the arm existed, and the arm it was taken
# under is the stock sequence -- that is a fact about those runs, not a default.
SEQ_DEFAULT = "stock"


def _data_lines(stream):
    """The CSV rows out of a raw capture log.

    The harness writes its CSV onto a console that also carries the IDF boot banner,
    '#' comments and @@CAPTURE markers, and tools/bringup_capture.py saves all of it.
    Extracting the rows by hand into a .csv first is a step that can quietly lose or
    duplicate a run, so this reads the log directly: nothing is counted until the
    pre-registered header line appears, and after it only lines whose first field is a
    run number are taken. A plain CSV file still works -- it is a log with no banner.
    """
    started = False
    for line in stream:
        line = line.strip()
        if not started:
            if line in HEADERS:
                started = True
                yield line
            continue
        if not line or line.startswith("#") or line.startswith("@@"):
            continue
        head = line.split(",", 1)[0]
        if head.isdigit():
            yield line


def load(stream):
    rows = []
    for raw in csv.DictReader(_data_lines(stream)):
        row = {"frs": int(raw["frs"], 16), "temp_c": int(raw["temp_c"]),
               "run": int(raw["run"]),
               "seq": raw.get("seq") or SEQ_DEFAULT}
        for p in PHASES:
            row[p] = float(raw[p])
        # nan, not 0.0: a v1 log did not measure this interval, and a zero would read
        # as a floor violation on every row of it.
        pon2drf = raw.get("t_pon2drf_ms")
        row["t_pon2drf_ms"] = float(pon2drf) if pon2drf else float("nan")
        rows.append(row)
    return rows


def spread(values):
    """Median plus the range that actually matters when judging a threshold."""
    s = sorted(values)
    n = len(s)
    return {
        "n": n,
        "median": statistics.median(s),
        "min": s[0],
        "max": s[-1],
        "iqr": (statistics.median(s[n // 2:]) - statistics.median(s[: (n + 1) // 2])
                if n >= 4 else float("nan")),
    }


def least_squares(xs, ys):
    """Fit y = c + n*x. Returns (c, n)."""
    k = len(xs)
    sx, sy = sum(xs), sum(ys)
    sxx = sum(x * x for x in xs)
    sxy = sum(x * y for x, y in zip(xs, ys))
    denom = k * sxx - sx * sx
    if k < 2 or denom == 0:
        return float("nan"), float("nan")
    n = (k * sxy - sx * sy) / denom
    return (sy - n * sx) / k, n


def summarise(rows):
    """Group by ARM -- (frs, sequencing) -- not by frs alone.

    Ticket 09 varies the host-side sequencing at a fixed frs, so grouping by frs would
    average the two arms together and report the difference as spread.
    """
    by_arm = {}
    for r in rows:
        by_arm.setdefault((r["frs"], r["seq"]), []).append(r)
    return by_arm


def report(rows, out):
    if not rows:
        out.write("no data rows\n")
        return

    by_arm = summarise(rows)
    # H5's fit is over FRS, and it is a fit of the FIXED overhead -- which is exactly
    # what the sequencing arm changes. Fit it on the stock arm only, so its intercept
    # stays comparable with the 1134.8 / 1157.6 ms in issues/08.
    by_frs = {}
    for (frs, seq), group in by_arm.items():
        if seq == SEQ_DEFAULT:
            by_frs.setdefault(frs, []).extend(group)
    if not by_frs:  # a busy-only log: fit what there is and say so
        for (frs, _seq), group in by_arm.items():
            by_frs.setdefault(frs, []).extend(group)

    out.write("=== per-phase medians (ms) ===\n")
    for frs, seq in sorted(by_arm):
        group = by_arm[(frs, seq)]
        # -128 is the firmware's "the panel did not give me one". Printing a
        # range over sentinels states the condition a run was taken under using
        # a number the panel never supplied, which is the whole of ticket 18.
        temps = [r["temp_c"] for r in group if r["temp_c"] != TEMP_UNAVAILABLE]
        if temps:
            label = "temp %d..%d C" % (min(temps), max(temps))
            if len(temps) != len(group):
                label += " (%d of %d runs)" % (len(temps), len(group))
        else:
            label = "temp n/a"
        out.write("\nFRS 0x%02X  seq=%s  n=%d  %s\n"
                  % (frs, seq, len(group), label))
        if len(group) < 10:
            out.write("  WARNING: fewer than 10 runs. A single reading is noise; the\n"
                      "  table this project is chasing is eight single-sample points.\n")
        out.write("  %-12s %9s %9s %9s %9s\n"
                  % ("phase", "median", "min", "max", "iqr"))
        for p in PHASES:
            s = spread([r[p] for r in group])
            out.write("  %-12s %9.3f %9.3f %9.3f %9.3f\n"
                      % (p, s["median"], s["min"], s["max"], s["iqr"]))

    _report_arms(by_arm, out)

    out.write("\n=== verdicts ===\n")
    _verdict_h1(by_frs, out)
    _verdict_h2(by_frs, out)
    _verdict_h5(by_frs, out)


def _report_arms(by_arm, out):
    """The issues/09 result: what BUSY-driven sequencing recovers, per FRS.

    Reported as a delta between arms measured in the same session rather than against
    a stored number, because refresh time moves with panel temperature and with the
    inter-refresh spacing. Two arms round-robined through one thermal history is the
    only comparison here that does not have to argue about conditions.
    """
    frs_values = sorted({frs for frs, _seq in by_arm})
    if not any(len({seq for f, seq in by_arm if f == frs}) > 1 for frs in frs_values):
        return  # single-arm log; there is no delta to report

    out.write("\n=== sequencing (issues/09) ===\n")
    for frs in frs_values:
        seqs = sorted(seq for f, seq in by_arm if f == frs)
        if len(seqs) < 2:
            continue
        totals = {seq: spread([r["t_total_ms"] for r in by_arm[(frs, seq)]])
                  for seq in seqs}
        out.write("FRS 0x%02X\n" % frs)
        for seq in seqs:
            s = totals[seq]
            out.write("  %-6s total median %9.1f ms  (n=%d, %.1f-%.1f)\n"
                      % (seq, s["median"], s["n"], s["min"], s["max"]))
        if SEQ_DEFAULT in totals and "busy" in totals:
            base = totals[SEQ_DEFAULT]["median"]
            busy = totals["busy"]["median"]
            out.write("  recovered: %.1f ms (%.2f%% of the stock refresh)\n"
                      % (base - busy, 100.0 * (base - busy) / base if base else 0.0))
            # The three stock sleeps are 600 ms of vTaskDelay at a 10 ms tick. A
            # recovery far from that is not the delays being removed and is worth
            # saying out loud rather than filing as a win.
            if not 400.0 <= (base - busy) <= 700.0:
                out.write("  NOTE: 400-700 ms was expected (three 200 ms sleeps).\n"
                          "        A figure outside that is measuring something else.\n")

    pon2drf = [r["t_pon2drf_ms"] for group in by_arm.values() for r in group
               if r["t_pon2drf_ms"] == r["t_pon2drf_ms"]]  # drop nan (v1 rows)
    if pon2drf:
        s = spread(pon2drf)
        ok = s["min"] >= H1_PON_FLOOR_MS
        out.write("PON->DRF floor: min %.3f ms against %.0f ms -- %s (n=%d)\n"
                  % (s["min"], H1_PON_FLOOR_MS, "held" if ok else "VIOLATED", s["n"]))
        if not ok:
            out.write("  -> the driver issued DRF inside the panel's stated minimum.\n"
                      "     Every image in this session is suspect, not just the fast\n"
                      "     arm's. Fix the enforcement before reading any timing.\n")


def _verdict_h1(by_frs, out):
    pon = [r["t_pon_ms"] for group in by_frs.values() for r in group]
    s = spread(pon)
    ok = s["median"] >= H1_PON_FLOOR_MS and s["min"] >= H1_PON_FLOOR_MS
    out.write("H1 BUSY covers PON->DRF: %s\n" % ("CONFIRMED" if ok else "REFUTED"))
    out.write("   median %.3f ms, min %.3f ms, floor %.0f ms (n=%d)\n"
              % (s["median"], s["min"], H1_PON_FLOOR_MS, s["n"]))
    if not ok:
        out.write("   -> the fixed delays are load-bearing. issues/09 STOPS and is\n"
                  "      reassessed rather than removing them anyway.\n")


def _verdict_h2(by_frs, out):
    xfer = [r["t_xfer_ms"] for group in by_frs.values() for r in group]
    total = [r["t_total_ms"] for group in by_frs.values() for r in group]
    s = spread(xfer)
    lo = H2_XFER_PREDICTED_MS * (1 - H2_TOLERANCE)
    hi = H2_XFER_PREDICTED_MS * (1 + H2_TOLERANCE)
    ok = lo <= s["median"] <= hi
    out.write("H2 transfer ~%.0f ms: %s\n"
              % (H2_XFER_PREDICTED_MS, "CONFIRMED" if ok else "REFUTED"))
    out.write("   median %.3f ms, accept %.0f-%.0f ms\n" % (s["median"], lo, hi))

    ratio = s["median"] / statistics.median(total)
    out.write("   transfer is %.2f%% of total refresh\n" % (100 * ratio))
    if ratio < H2_XFER_RATIO_DECISION:
        out.write("   -> below %.0f%%: DMA and clock-rate work is NOT worth doing.\n"
                  % (100 * H2_XFER_RATIO_DECISION))


def _verdict_h5(by_frs, out):
    direct = [r["t_total_ms"] - r["t_drf_ms"]
              for group in by_frs.values() for r in group]
    c_direct = statistics.median(direct)
    out.write("H5 fixed overhead:\n")
    out.write("   method 1, direct (total - drf): %.1f ms\n" % c_direct)

    # Method 2 needs the linear range swept, and only 0x03-0x07 is in the model's
    # domain -- the published data already refutes it below 0x03.
    pts = [(1.0 / frs_rate_hz(f), statistics.median([r["t_total_ms"] for r in g]))
           for f, g in by_frs.items() if 0x03 <= f <= 0x07]
    if len(pts) < 3:
        out.write("   method 2, fit intercept: needs >=3 values in 0x03-0x07 "
                  "(have %d); run the sweep\n" % len(pts))
        out.write("   INCONCLUSIVE until the sweep is done\n")
        return

    c_fit, n_fit = least_squares([p[0] for p in pts], [p[1] for p in pts])
    out.write("   method 2, fit intercept: %.1f ms (N=%.0f frame-seconds, %d points)\n"
              % (c_fit, n_fit, len(pts)))
    out.write("   registered expectation: %.0f ms\n" % H5_EXPECTED_C_MS)

    denom = max(abs(c_direct), 1e-9)
    if abs(c_fit - c_direct) / denom <= H5_AGREEMENT:
        out.write("   CONFIRMED: the two methods agree within %.0f%%\n"
                  % (100 * H5_AGREEMENT))
    else:
        out.write("   DISAGREE by %.0f%%\n" % (100 * abs(c_fit - c_direct) / denom))
        if c_fit > c_direct:
            out.write("   -> a fixed component lives INSIDE the DRF phase. This lowers\n"
                      "      the ceiling on what any FRS change can achieve.\n")
    out.write("   floor at 200 Hz: %.0f ms -- no frame-rate work goes below this\n"
              % (c_fit + n_fit / 200.0))


# --- self-test -----------------------------------------------------------------

def _synth(frs_values, pon_ms, xfer_ms, drf_scale=1.0, runs=10):
    """Build a v1 CSV with planted values. Deterministic: no RNG, so an assertion that
    fails is a real failure and not a seed.

    v1 on purpose -- this is the schema ticket 08's and ticket 17's logs are in, so
    every test built on it is also a test that those logs still load.
    """
    out = [HEADER_V1]
    for frs in frs_values:
        drf = (896751.0 / frs_rate_hz(frs)) * drf_scale
        for i in range(runs):
            # Symmetric about zero for any n, so the planted median is exact.
            jitter = (i - (runs - 1) / 2.0) * 0.1
            fixed = 202.0 + 3.5 + xfer_ms + pon_ms + 90.0
            out.append("%d,0x%02X,25,202.000,3.500,%.3f,%.3f,%.3f,90.000,%.3f"
                       % (i + 1, frs, xfer_ms + jitter, pon_ms + jitter, drf,
                          fixed + drf + 620.0))
    return "\n".join(out) + "\n"


def _synth_arms(frs, recovered_ms, pon2drf_ms=130.9, runs=10):
    """A v2 two-arm CSV with a planted stock-minus-busy delta."""
    out = [HEADER_V2]
    drf = 896751.0 / frs_rate_hz(frs)
    run = 0
    for seq, extra in (("stock", 600.0), ("busy", 600.0 - recovered_ms)):
        for i in range(runs):
            run += 1
            fixed = 202.0 + 3.5 + 240.0 + 130.9 + 90.0
            out.append("%d,0x%02X,25,202.000,3.500,240.000,130.900,%.3f,90.000,"
                       "%.3f,%s,%.3f"
                       % (run, frs, drf, fixed + drf + extra, seq, pon2drf_ms))
    return "\n".join(out) + "\n"


def self_test():
    failures = []

    def check(name, condition, detail=""):
        if condition:
            print("  PASS  %s" % name)
        else:
            print("  FAIL  %s %s" % (name, detail))
            failures.append(name)

    print("a raw capture log loads as the CSV inside it:")
    synth = _synth([0x08], pon_ms=85.0, xfer_ms=240.0)
    noisy = ("I (828) esp_psram: SPI SRAM memory test OK\n"
             "# M5Paper Color EPD refresh harness\n"
             "# frs sweep: 0x08\n"
             + synth.replace("\n", "\n", 1)
             + "@@CAPTURE frs-0x08\n"
             "# done\n")
    check("banner and markers are skipped",
          len(load(io.StringIO(noisy))) == len(load(io.StringIO(synth))))
    check("nothing before the header is read",
          load(io.StringIO("1,0x08,25,1,1,1,1,1,1,1\n")) == [])

    print("planted medians are recovered exactly:")
    rows = load(io.StringIO(synth))
    check("n = 10", len(rows) == 10)
    s = spread([r["t_pon_ms"] for r in rows])
    check("median t_pon == 85.000", abs(s["median"] - 85.0) < 1e-6,
          "got %.6f" % s["median"])
    s = spread([r["t_xfer_ms"] for r in rows])
    check("median t_xfer == 240.000", abs(s["median"] - 240.0) < 1e-6,
          "got %.6f" % s["median"])

    print("\nH1 passes when PON clears 80 ms:")
    buf = io.StringIO()
    report(load(io.StringIO(_synth([0x08], pon_ms=85.0, xfer_ms=240.0))), buf)
    check("verdict CONFIRMED", "H1 BUSY covers PON->DRF: CONFIRMED" in buf.getvalue())

    print("\nH1 fails on a single early release, not just a bad median:")
    csv_text = _synth([0x08], pon_ms=85.0, xfer_ms=240.0)
    lines = csv_text.splitlines()
    # One run releases at 79 ms. Median stays healthy; min does not.
    lines[1] = lines[1].replace(",85.000,", ",79.000,").replace(
        ",84.500,", ",79.000,")
    parts = lines[1].split(",")
    parts[6] = "79.000"
    lines[1] = ",".join(parts)
    buf = io.StringIO()
    rows = load(io.StringIO("\n".join(lines) + "\n"))
    report(rows, buf)
    s = spread([r["t_pon_ms"] for r in rows])
    check("median still >= 80", s["median"] >= 80.0, "got %.3f" % s["median"])
    check("verdict REFUTED anyway", "H1 BUSY covers PON->DRF: REFUTED" in buf.getvalue())
    check("issues/09 stop instruction printed", "issues/09 STOPS" in buf.getvalue())

    print("\nH2 confirms at 240 ms and refutes at 900 ms:")
    buf = io.StringIO()
    report(load(io.StringIO(_synth([0x08], pon_ms=85.0, xfer_ms=240.0))), buf)
    check("CONFIRMED at 240", "H2 transfer ~240 ms: CONFIRMED" in buf.getvalue())
    check("ratio decision fires", "NOT worth doing" in buf.getvalue())
    buf = io.StringIO()
    report(load(io.StringIO(_synth([0x08], pon_ms=85.0, xfer_ms=900.0))), buf)
    check("REFUTED at 900", "H2 transfer ~240 ms: REFUTED" in buf.getvalue())

    print("\nH5 recovers a planted intercept from a swept dataset:")
    buf = io.StringIO()
    report(load(io.StringIO(_synth([0x03, 0x04, 0x05, 0x06, 0x07],
                                   pon_ms=85.0, xfer_ms=240.0))), buf)
    text = buf.getvalue()
    check("fit ran", "method 2, fit intercept" in text)
    # The generator's non-DRF total is 202+3.5+240+85+90+620 = 1240.5 ms, and it is
    # frame-rate independent by construction, so both methods must land on it.
    check("intercept ~1240.5 ms", "1240." in text or "1241." in text,
          "\n".join(l for l in text.splitlines() if "intercept" in l))
    check("methods agree", "CONFIRMED: the two methods agree" in text)

    print("\nH5 is inconclusive without a sweep:")
    buf = io.StringIO()
    report(load(io.StringIO(_synth([0x08], pon_ms=85.0, xfer_ms=240.0))), buf)
    check("INCONCLUSIVE", "INCONCLUSIVE until the sweep is done" in buf.getvalue())

    print("\nthe two-arm schema loads and the planted delta is recovered:")
    rows = load(io.StringIO(_synth_arms(0x08, recovered_ms=592.0)))
    check("n = 20 across both arms", len(rows) == 20)
    check("arms are grouped separately", len(summarise(rows)) == 2)
    buf = io.StringIO()
    report(rows, buf)
    text = buf.getvalue()
    check("delta reported as 592.0 ms", "recovered: 592.0 ms" in text,
          "\n".join(l for l in text.splitlines() if "recovered" in l))
    check("no out-of-range note", "measuring something else" not in text)
    check("floor reported as held", "-- held" in text)

    print("\na delta nowhere near the three sleeps is flagged:")
    buf = io.StringIO()
    report(load(io.StringIO(_synth_arms(0x08, recovered_ms=40.0))), buf)
    check("note fires", "measuring something else" in buf.getvalue())

    print("\na PON->DRF interval under the floor is called a violation:")
    buf = io.StringIO()
    report(load(io.StringIO(_synth_arms(0x08, recovered_ms=592.0,
                                        pon2drf_ms=79.0))), buf)
    check("VIOLATED", "VIOLATED" in buf.getvalue())
    check("says every image is suspect", "Every image in this session" in
          buf.getvalue())

    print("\nv1 logs still load, and are labelled as the stock arm:")
    rows = load(io.StringIO(_synth([0x08], pon_ms=85.0, xfer_ms=240.0)))
    check("seq defaults to stock", all(r["seq"] == "stock" for r in rows))
    check("t_pon2drf is nan, not zero",
          all(r["t_pon2drf_ms"] != r["t_pon2drf_ms"] for r in rows))
    buf = io.StringIO()
    report(rows, buf)
    check("no sequencing section for one arm",
          "=== sequencing" not in buf.getvalue())
    check("no floor line invented from missing data", "PON->DRF floor" not in
          buf.getvalue())

    print("\nunder-powered runs are called out:")
    buf = io.StringIO()
    report(load(io.StringIO(_synth([0x08], pon_ms=85.0, xfer_ms=240.0, runs=3))), buf)
    check("warns below 10 runs", "fewer than 10 runs" in buf.getvalue())

    print()
    if failures:
        print("%d self-test failure(s): %s" % (len(failures), ", ".join(failures)))
        return 1
    print("all self-tests passed")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="?", help="harness CSV; '-' for stdin")
    ap.add_argument("--self-test", action="store_true",
                    help="run against synthetic data with known answers")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if not args.csv:
        ap.error("give a CSV path, or --self-test")

    stream = sys.stdin if args.csv == "-" else open(args.csv)
    with stream:
        report(load(stream), sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
