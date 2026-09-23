"""Is the firmware's battery curve still the page's battery curve?

`assets/index.html`'s `batteryPercentFromMV()` has been the web UI's readout since before the matte
band existed. `src/core/battery.c` is a PORT of it, because the band's icon needs the same number and
two implementations of one curve would have the page and the glass disagreeing about the same cell
with nothing to say which was right (ticket 64).

So the acceptance test is "identical", not "reasonable" -- the same rule the epdoptimize port follows,
and the same shape as `tools/ink_preview_parity.py`: the JavaScript is EXTRACTED from the page rather
than copied here, so it cannot drift, and the C is compiled from `src/core/battery.c` itself rather
than reimplemented.

    python tools/battery_parity.py
    python tools/battery_parity.py --self-check

`--self-check` perturbs the C side four ways and requires each to be caught. **A parity test that
cannot fail looks exactly like one that passes**, and this repository has been fooled by that shape
before.

Needs `node` for the JavaScript side and a C compiler; it finds the same clang
`tools/native_toolchain.py` does.
"""
import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
PAGE = ROOT / "assets" / "index.html"
C_SRC = ROOT / "src" / "core" / "battery.c"
MV_MAX = 5000

# The C driver. Prints one percentage per millivolt so the comparison is every value rather than a
# sample -- the curve is 5001 integers and checking all of them costs nothing.
DRIVER = r"""
#include <stdio.h>
#include "battery.h"
int main(void)
{
    for (unsigned mv = 0; mv <= %d; mv++) {
        printf("%%d\n", battery_percent_from_mv((unsigned short)mv));
    }
    return 0;
}
""" % MV_MAX


def extract_js():
    """The page's function, lifted verbatim. Brace-matched rather than regex-terminated: a regex
    that stopped at the first `}` would take half the function and still look plausible."""
    src = PAGE.read_text(encoding="utf-8")
    start = src.index("function batteryPercentFromMV")
    depth = 0
    for i in range(src.index("{", start), len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                body = src[start:i + 1]
                break
    else:
        raise SystemExit("could not brace-match batteryPercentFromMV in assets/index.html")
    if "Math.round" not in body:
        raise SystemExit("extracted text does not look like the curve: %r" % body[:80])
    return body


def run_js(js):
    script = js + "\nfor (let mv = 0; mv <= %d; mv++) console.log(batteryPercentFromMV(mv));\n" % MV_MAX
    with tempfile.TemporaryDirectory() as td:
        p = pathlib.Path(td) / "curve.mjs"
        p.write_text(script, encoding="utf-8")
        out = subprocess.run(["node", str(p)], capture_output=True, text=True, check=True)
    return [int(v) for v in out.stdout.split()]


# The same directories tools/native_toolchain.py searches: this machine has no gcc, and clang 22 is
# installed under Program Files but not on PATH. Kept in step with that file by hand -- there is no
# import path to it, because it is a SCons script that expects an injected `env`.
CLANG_DIRS = [
    pathlib.Path(r"C:\Program Files\LLVM\bin"),
    pathlib.Path(r"C:\Program Files (x86)\LLVM\bin"),
]


def find_cc():
    for d in CLANG_DIRS:
        exe = d / "clang.exe"
        if exe.is_file():
            return str(exe)
    for candidate in ("clang", "gcc", "cc"):
        try:
            subprocess.run([candidate, "--version"], capture_output=True, check=True)
            return candidate
        except (OSError, subprocess.CalledProcessError):
            continue
    raise SystemExit("no C compiler found (clang is installed off PATH; see tools/native_toolchain.py)")


def run_c(source_text):
    cc = find_cc()
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        (td / "battery.c").write_text(source_text, encoding="utf-8")
        (td / "battery.h").write_text((ROOT / "src" / "core" / "battery.h").read_text(encoding="utf-8"),
                                      encoding="utf-8")
        (td / "driver.c").write_text(DRIVER, encoding="utf-8")
        exe = td / ("curve.exe" if sys.platform == "win32" else "curve")
        subprocess.run([cc, "-std=c11", "-O1", "-I", str(td), str(td / "battery.c"),
                        str(td / "driver.c"), "-o", str(exe)], check=True,
                       capture_output=True, text=True)
        out = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
    return [int(v) for v in out.stdout.split()]


def compare(js, c, label):
    bad = [(mv, a, b) for mv, (a, b) in enumerate(zip(js, c)) if a != b]
    if len(js) != len(c):
        print("%-28s LENGTH MISMATCH js=%d c=%d" % (label, len(js), len(c)))
        return False
    if bad:
        print("%-28s %d of %d millivolts differ; first five:" % (label, len(bad), len(js)))
        for mv, a, b in bad[:5]:
            print("    %4d mV  page=%3d  firmware=%3d" % (mv, a, b))
        return False
    print("%-28s identical at all %d millivolts" % (label, len(js)))
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-check", action="store_true",
                    help="perturb the C four ways and require each to be caught")
    args = ap.parse_args()

    js_src = extract_js()
    js = run_js(js_src)
    c_src = C_SRC.read_text(encoding="utf-8")
    ok = compare(js, run_c(c_src), "page vs firmware")

    if args.self_check:
        # Each perturbation is a mistake somebody could actually make: a wrong base, a wrong span, a
        # shifted boundary, and the truncation instead of the rounding.
        perturbations = [
            ("base 85 -> 86", "js_round(85 +", "js_round(86 +"),
            ("span 35 -> 34", "(v - 3.8) / 0.2 * 35", "(v - 3.8) / 0.2 * 34"),
            ("boundary 3.6 -> 3.61", "v >= 3.6", "v >= 3.61"),
            # The one that caught the first implementation: Math.round is half-UP, and truncating
            # instead differs at 485 of 5001 millivolts.
            ("round -> truncate", "return (int)floor(x + 0.5);", "return (int)x;"),
        ]
        for label, old, new in perturbations:
            if old not in c_src:
                print("%-28s ANCHOR MISSING -- the self-check cannot run" % label)
                ok = False
                continue
            assert c_src.count(old) == 1, "anchor %r is not unique" % old
            caught = not compare(js, run_c(c_src.replace(old, new)), "  perturbed: " + label)
            if not caught:
                print("    NOT CAUGHT -- this check cannot fail and so proves nothing")
                ok = False

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
