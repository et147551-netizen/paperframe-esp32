"""Is the Web UI's ink preview still what the device renders?

`assets/index.html`'s preview quantiser is the same algorithm as `src/core/epd_dither.c`'s row path,
and until 2026-09-06 it silently differed from it on 18 % of a photograph's pixels: a hardcoded
palette, no tone compression, dither strength 200 against the device's 140, and a clamp the
device does not apply. Nothing could have noticed, because both sides looked reasonable on
screen. This is what notices.

The C side is `tools/render_preview.c`, which links `src/core/epd_dither.c` itself, so this compares
the page against the firmware's quantiser rather than against a description of it. The JS side is
extracted from `assets/index.html` by `tools/ink_preview_quantise.js`, so it cannot drift either.
The palette tables, tone amounts and dither strength come from a **device** response, which means
this also checks that `GET /api/mode/mode_1/config` serves the numbers `epd_dither.c` holds:

    T=$(grep -o 'FRAME_API_TOKEN=.*' platformio_local.ini | head -1 | cut -d= -f2- | tr -d "\\"'\\r")
    curl.exe -s "http://192.168.1.20/api/mode/mode_1/config?t=$T" -o config.json
    python tools/ink_preview_parity.py config.json

`--self-check` then perturbs the served numbers four ways and requires each to be *caught*. Run
it whenever the preview or the row path changes: a parity test that cannot fail looks exactly
like one that passes, and this repository has been fooled by that shape before.
"""
import argparse
import json
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from render_preview import build_renderer, decode_png  # noqa: E402

# firmware palette id -> the name tools/render_preview.c knows it by
PALETTES = {
    "aitjcize": "epdopt-aitjcize",
    "original": "epdopt-original",
    "spectra6": "epdopt-spectra6",
    "legacy": "epdopt-legacy",
    "boeber": "epdopt-boeber",
    "manual": "manual",
    "stock": "stock",
}
FIXTURES = [
    ROOT / ".scratch" / "digital-frame" / "fixtures" / "imaged003.png",
    # 8.2 % pure white and a p99 of 255, so the ordered bias pushes past 255: this is the arm
    # where the preview's old clampByte() had to disagree with the device.
    ROOT / ".scratch" / "digital-frame" / "fixtures" / "imaged001.png",
]
# `diffuse` is the device's default path since `dither_diffuse` shipped on, and the preview's since
# 2026-09-22: tone compression, then serpentine Floyd-Steinberg (src/app/app_display.c's
# diffuse_and_pack(), which tools/render_preview.c repeats step for step).
MODES = ("quality", "none", "diffuse")


def c_indices(exe, mode, w, h, palette_name, rgb):
    args = [str(exe), mode, str(w), str(h), palette_name]
    if mode == "diffuse":
        args = [str(exe), "quality", str(w), str(h), palette_name, "diffuse", "serpentine"]
    proc = subprocess.run(args, input=rgb, capture_output=True)
    if proc.returncode != 0:
        sys.exit(proc.stderr.decode("utf-8", "replace"))
    packed = proc.stdout
    row = (w + 1) // 2
    out = bytearray(w * h)
    for y in range(h):
        base = y * row
        for x in range(w):
            byte = packed[base + (x >> 1)]
            out[y * w + x] = (byte >> 4) if (x & 1) == 0 else (byte & 0x0F)
    return bytes(out)


def js_indices(cfg_path, pid, mode, raw, w, h, out, clamp):
    env = None
    if clamp:
        import os
        env = dict(os.environ, INK_CLAMP="1")
    return subprocess.run(
        ["node", str(ROOT / "tools" / "ink_preview_quantise.js"), str(cfg_path), pid, mode,
         str(raw), str(w), str(h), str(out)],
        capture_output=True, text=True, env=env)


def run(cfg_path, palettes, tmp, exe, clamp=False, quiet=False):
    """Returns a {(fixture, palette, mode): differing_pixel_count} map."""
    result = {}
    for fixture in FIXTURES:
        w, h, rgb = decode_png(fixture)
        raw = tmp / (fixture.stem + ".rgb")
        raw.write_bytes(rgb)
        for pid in palettes:
            for mode in MODES:
                idx = tmp / "js.idx"
                r = js_indices(cfg_path, pid, mode, raw, w, h, idx, clamp)
                if r.returncode != 0:
                    sys.exit(f"{fixture.name} {pid} {mode}: {r.stderr.strip()}")
                js = idx.read_bytes()
                c = c_indices(exe, mode, w, h, PALETTES[pid], rgb)
                diff = sum(1 for a, b in zip(js, c) if a != b)
                result[(fixture.name, pid, mode)] = diff
                if not quiet:
                    if diff == 0:
                        print(f"ok   {fixture.name:16s} {pid:9s} {mode:8s} "
                              f"{len(js)} px identical   [{r.stdout.strip()}]")
                    else:
                        first = next(i for i, (a, b) in enumerate(zip(js, c)) if a != b)
                        print(f"FAIL {fixture.name:16s} {pid:9s} {mode:8s} "
                              f"{diff}/{len(js)} px differ "
                              f"({100.0 * diff / len(js):.3f} %), first at "
                              f"x={first % w} y={first // w}: js={js[first]} c={c[first]}")
    return result


def perturb(cfg, kind):
    d = json.loads(json.dumps(cfg))
    if kind == "strength":
        d["dither_strength"] = 200
        return d, "the shipping preview's dither strength", ("quality",)
    for p in d["palettes"]:
        if p["id"] != "aitjcize":
            continue
        if kind == "tone":
            p["tone"] = 0
        elif kind == "order":
            p["match"] = p["match"][4:8] + p["match"][0:4] + p["match"][8:]
    if kind == "tone":
        return d, "tone compression dropped", MODES
    return d, "the first two match entries swapped", MODES


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("config", type=pathlib.Path,
                    help="a GET /api/mode/mode_1/config response; see this file's docstring")
    ap.add_argument("--self-check", action="store_true",
                    help="also require four deliberate perturbations to be caught")
    args = ap.parse_args()

    cfg = json.loads(args.config.read_text(encoding="utf-8"))
    served = {p["id"] for p in cfg.get("palettes", [])}
    missing = served - set(PALETTES)
    if missing:
        sys.exit(f"the device serves palettes this script has no C name for: {sorted(missing)}\n"
                 "add them to PALETTES, or the check silently skips them")
    palettes = [p for p in PALETTES if p in served]

    exe = build_renderer()
    with tempfile.TemporaryDirectory() as td:
        tmp = pathlib.Path(td)
        print(f"-- as served, {len(palettes)} palettes x {len(FIXTURES)} fixtures x "
              f"{len(MODES)} modes")
        base = run(args.config, palettes, tmp, exe)
        failed = sum(1 for v in base.values() if v)

        if args.self_check:
            print("-- self-check: each of these must be CAUGHT")
            for kind in ("strength", "tone", "order"):
                d, what, expect_modes = perturb(cfg, kind)
                p = tmp / f"cfg_{kind}.json"
                p.write_text(json.dumps(d), encoding="utf-8")
                res = run(p, ["aitjcize"], tmp, exe, quiet=True)
                for (fx, _pid, mode), diff in sorted(res.items()):
                    want = mode in expect_modes
                    ok = (diff > 0) == want
                    print(f"{'ok  ' if ok else 'FAIL'} {what:38s} {fx:16s} {mode:8s} "
                          f"{diff} px differ, expected {'some' if want else 'none'}")
                    if not ok:
                        failed += 1
            res = run(args.config, ["aitjcize"], tmp, exe, clamp=True, quiet=True)
            for (fx, _pid, mode), diff in sorted(res.items()):
                want = mode == "quality"
                ok = (diff > 0) == want
                print(f"{'ok  ' if ok else 'FAIL'} {'clampByte() restored':38s} {fx:16s} "
                      f"{mode:8s} {diff} px differ, expected {'some' if want else 'none'}")
                if not ok:
                    failed += 1

    print("ALL OK" if failed == 0 else f"{failed} checks failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
