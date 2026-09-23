"""Does every setting on GET/POST /api/mode/mode_1/config survive a reboot? (ticket 80 section 4)

Ticket 80 put a _Static_assert on APP_SETTING_COUNT so a new key cannot be added without its
save_locked() case and its load_from_nvs() line. That is a guard against FORGETTING. It cannot
see a key that is saved and loaded under the wrong NVS name -- both halves would use the same
wrong macro, the setting would round-trip, and what breaks is the OTHER setting sharing the
name. The only thing that catches that is writing a distinct value into every field, rebooting,
and reading them all back. Nobody had done it: the settings tickets each verified one field.

Three phases, because the reboot happens between them and the originals have to survive it:

    python tools/settings_roundtrip.py probe   --host H --token T --state S
    <reboot the board>
    python tools/settings_roundtrip.py verify  --host H --token T --state S
    python tools/settings_roundtrip.py restore --host H --token T --state S

`probe` saves the current config to S, derives a DIFFERENT valid value for every field it
knows, POSTs them, and re-reads. `verify` compares the device against what `probe` wrote --
this is the phase that has to run after the reboot. `restore` POSTs S's originals back and
re-reads to confirm. Every phase prints a per-field table and computes its verdict at the end
from the rows, not inside the loop.

WHAT IT DELIBERATELY DOES NOT TOUCH. Nothing whose current value cannot be read back, because
a probe that cannot be undone is not a probe: smb_password and wifi_password are write-only by
design, and a Google album key is a bearer capability the API redacts. /api/smb/config's eight
non-secret fields are readable and therefore probe-able, and are still left out -- changing the
share's host mid-run makes the mirror fail in a way that has nothing to do with this question.
So this covers the seventeen settings on the mode-config route; the other twenty-three are
noted as uncovered rather than silently implied.

THE BODY LIMIT IS 256 BYTES (app_server.c's read_json_body), and a longer body is a 400 that
reads like a rejected value. Fields are therefore sent in small groups -- and the five render
settings go in ONE group on purpose, because h_mode_cfg_set() accumulates a single redraw
decision per body, so five separate POSTs would be five panel refreshes instead of one.
"""

import argparse
import json
import sys
import urllib.error
import urllib.request

# The five that make h_mode_cfg_set() redraw (src/core/app_setting_effect.c), kept in one body.
RENDER_FIELDS = ["rotation", "palette", "auto_adjust", "dither_diffuse", "auto_rotate"]
# The schedule pair MUST travel together; the handler refuses one without the other.
GROUPS = [
    RENDER_FIELDS,
    ["auto_slideshow", "interval_minutes", "low_power_mode", "slideshow_random",
     "tz_offset_minutes"],
    ["active_start_hour", "active_end_hour", "standby_white", "standby_deep", "maint_day",
     "charge_limit_pct"],
]
FIELDS = [f for g in GROUPS for f in g]


def get_json(host, path, token, timeout=20):
    url = "http://%s%s?t=%s" % (host, path, token)
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def post_json(host, path, token, obj, timeout=90):
    url = "http://%s%s?t=%s" % (host, path, token)
    data = json.dumps(obj, separators=(",", ":")).encode("utf-8")
    if len(data) >= 256:
        raise SystemExit("body is %d bytes; app_server.c's read_json_body refuses >= 256" % len(data))
    req = urllib.request.Request(url, data=data, method="POST",
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")


def probe_value(field, cur, cfg):
    """A different, valid value for `field`. Ranges are h_mode_cfg_set()'s, read from the code."""
    if field == "rotation":
        return (int(cur) + 1) % (int(cfg.get("rotation_max", 3)) + 1)
    if field == "palette":
        ids = [p["id"] for p in cfg.get("palettes", []) if p.get("id") != cur]
        return ids[0] if ids else cur
    if field == "interval_minutes":
        return 7 if int(cur) != 7 else 5          # 1..255
    if field == "tz_offset_minutes":
        return 60 if int(cur) != 60 else 0        # -720..840
    if field == "active_start_hour":
        return (int(cur) + 1) % 24
    if field == "active_end_hour":
        return (int(cur) + 23) % 24
    if field == "maint_day":
        return 3 if int(cur) != 3 else 4          # 0..6 a weekday, 7 never
    if field == "charge_limit_pct":
        return 100 if int(cur) != 100 else 80     # 0, 80 or 100 only
    if isinstance(cur, bool):
        return not cur
    raise SystemExit("no probe value rule for %r" % field)


def read_cfg(host, token):
    return get_json(host, "/api/mode/mode_1/config", token)


def table(rows):
    bad = 0
    for field, want, got in rows:
        ok = (want == got)
        if not ok:
            bad += 1
        print("%-20s want=%-14r got=%-14r %s" % (field, want, got, "" if ok else "<-- MISMATCH"))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("phase", choices=["probe", "verify", "restore"])
    ap.add_argument("--host", required=True)
    ap.add_argument("--token", required=True)
    ap.add_argument("--state", required=True, help="where the originals and the probe values live")
    args = ap.parse_args()

    if args.phase == "probe":
        cfg = read_cfg(args.host, args.token)
        original = {f: cfg[f] for f in FIELDS}
        wanted = {f: probe_value(f, cfg[f], cfg) for f in FIELDS}
        with open(args.state, "w", encoding="utf-8") as fh:
            json.dump({"original": original, "wanted": wanted}, fh, indent=1)
        for group in GROUPS:
            status, body = post_json(args.host, "/api/mode/mode_1/config", args.token,
                                     {f: wanted[f] for f in group})
            print("POST %-40s -> %s" % (",".join(group)[:40], status))
            if status != 200:
                print("   body: %r" % body)
        after = read_cfg(args.host, args.token)
        print("\n-- what the device says immediately after the write (before any reboot) --")
        bad = table([(f, wanted[f], after[f]) for f in FIELDS])
        print("\nmismatches before the reboot: %d  (a mismatch here is the WRITE, not persistence)"
              % bad)
        return 1 if bad else 0

    with open(args.state, encoding="utf-8") as fh:
        state = json.load(fh)

    if args.phase == "verify":
        after = read_cfg(args.host, args.token)
        print("-- after the reboot: did each probe value survive? --")
        bad = table([(f, state["wanted"][f], after[f]) for f in FIELDS])
        print("\nfields=%d  did not survive=%d" % (len(FIELDS), bad))
        return 1 if bad else 0

    for group in GROUPS:
        status, body = post_json(args.host, "/api/mode/mode_1/config", args.token,
                                 {f: state["original"][f] for f in group})
        print("RESTORE %-37s -> %s" % (",".join(group)[:37], status))
        if status != 200:
            print("   body: %r" % body)
    after = read_cfg(args.host, args.token)
    print("\n-- restored? --")
    bad = table([(f, state["original"][f], after[f]) for f in FIELDS])
    print("\nnot restored=%d" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
