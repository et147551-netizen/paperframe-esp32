#!/usr/bin/env python3
"""Sign, check and try out the Google Photos scrape rules the frame reads.

Designed in the scrape-rules plan. The frame only uses a rule file whose signature verifies
against the public key compiled into it (src/app/gphotos_rules_pubkey.h), so a rule change needs this
script and the private key, and nothing else -- no rebuild, and nobody at the frame.

  keygen                 make the key pair: the private key OUTSIDE the repository, the public key
                         into src/app/gphotos_rules_pubkey.h. Refuses to overwrite either.
  sign RULES.json        write RULES.signed: line one the base64 DER signature over the SHA-256 of
                         every byte after that line, then RULES.json byte for byte.
  verify SIGNED          check a signed file against the header's public key and the frame's limits.
  check RULES.json       the frame's limits alone, before signing.
  scan RULES PAGE.html   run the rules' matcher over a saved album page and print what each pattern
                         found -- counts and sizes only, never a key: a key is a photograph's URL.

Back the private key up (a password manager will do). Losing it means a firmware with a new public
key, which means USB at the frame until OTA exists.

Needs the `cryptography` package.
"""

import argparse
import base64
import json
import re
import sys
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

REPO = Path(__file__).resolve().parent.parent
HEADER = REPO / "src" / "app" / "gphotos_rules_pubkey.h"
PRIVATE = Path.home() / ".m5paper-keys" / "gphotos_rules_private.pem"

# The firmware's limits, gphotos_rules.h. Keep them equal.
FILE_MAX = 4096
MAX_PATTERNS = 4
ANCHOR_MAX = 32
ID_RE = re.compile(r"^[a-z0-9]{1,8}$")
AFTER_MAX = 8
LIT_MAX = 8
URL_MAX = 159
UA_MAX = 191
KEY_MAX = 255
KEY_ALLOWED = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._~%/-")


class Refused(Exception):
    pass


def printable(s):
    return all(0x20 <= ord(c) <= 0x7E for c in s)


def parse_chars(spec):
    if not isinstance(spec, str) or not spec:
        raise Refused("key chars are empty")
    out = set()
    i = 0
    while i < len(spec):
        lo = hi = spec[i]
        if i + 2 < len(spec) and spec[i + 1] == "-":
            hi = spec[i + 2]
            i += 3
        else:
            i += 1
        if lo > hi:
            raise Refused("key chars hold a backwards range")
        for c in map(chr, range(ord(lo), ord(hi) + 1)):
            if c not in KEY_ALLOWED:
                raise Refused("key chars go beyond [A-Za-z0-9._~%/-]")
            out.add(c)
    return out


def parse_token(t):
    if t == "num":
        return ("num", None)
    if not isinstance(t, str) or not t.startswith("lit:"):
        raise Refused('an after token is neither "num" nor "lit:..."')
    lit = t[4:]
    if not 1 <= len(lit.encode()) <= LIT_MAX or not printable(lit):
        raise Refused("a literal token is not 1-8 printable bytes")
    return ("lit", lit)


def check_url(url):
    if not isinstance(url, str) or not url.startswith("https://"):
        raise Refused("url does not start with https://")
    if len(url) > URL_MAX:
        raise Refused("url is too long")
    rest = url[len("https://"):]
    host, slash, path = rest.partition("/")
    if not host or not slash:
        raise Refused("url has no host and path")
    if not re.fullmatch(r"[A-Za-z0-9.-]+", host):
        raise Refused("url host holds a character a host cannot")
    stripped = path.replace("{key}", "").replace("{edge}", "")
    if "{" in stripped or "}" in stripped or " " in path or not printable(path):
        raise Refused("url holds a stray brace, a space or an unprintable byte")
    if path.count("{key}") != 1:
        raise Refused("url must hold {key} exactly once")


def check_rules(doc):
    if not isinstance(doc, dict):
        raise Refused("the rules are not a JSON object")
    if doc.get("v") != 1:
        raise Refused("v is not 1")
    seq = doc.get("seq")
    if not isinstance(seq, int) or isinstance(seq, bool) or not 1 <= seq <= 0xFFFFFFFF:
        raise Refused("seq is not a whole number from 1")
    ua = doc.get("ua")
    if not isinstance(ua, str) or not 1 <= len(ua) <= UA_MAX or not printable(ua):
        raise Refused("ua is not 1-191 printable characters")
    pats = doc.get("patterns")
    if not isinstance(pats, list) or not 1 <= len(pats) <= MAX_PATTERNS:
        raise Refused("patterns is not an array of 1-4")
    out = []
    for p in pats:
        pid = p.get("id")
        if not isinstance(pid, str) or not ID_RE.match(pid):
            raise Refused("a pattern id is not [a-z0-9]{1,8}")
        anchor = p.get("anchor")
        if not isinstance(anchor, str) or not 1 <= len(anchor.encode()) <= ANCHOR_MAX \
                or not printable(anchor):
            raise Refused("an anchor is not 1-32 printable bytes")
        key = p.get("key") or {}
        chars = parse_chars(key.get("chars"))
        kmax = key.get("max")
        if not isinstance(kmax, int) or not 1 <= kmax <= KEY_MAX:
            raise Refused("key max is not 1-255")
        after = p.get("after", [])
        if not isinstance(after, list) or len(after) > AFTER_MAX:
            raise Refused("after is not an array of at most 8 tokens")
        toks = [parse_token(t) for t in after]
        check_url(p.get("url"))
        out.append({"id": pid, "anchor": anchor, "chars": chars, "max": kmax, "after": toks})
    ids = [p["id"] for p in out]
    if len(set(ids)) != len(ids):
        raise Refused("two patterns share an id")
    return out


def load_header_key():
    text = HEADER.read_text(encoding="utf-8")
    pem = "\n".join(re.findall(r'"([^"\\]*)\\n"', text)) + "\n"
    if "BEGIN PUBLIC KEY" not in pem:
        raise SystemExit(f"no public key in {HEADER}")
    return serialization.load_pem_public_key(pem.encode())


def split_signed(data):
    nl = data.find(b"\n")
    if nl < 0:
        raise Refused("no signature line")
    sig_line = data[:nl].rstrip(b"\r")
    try:
        sig = base64.b64decode(sig_line, validate=True)
    except ValueError:
        raise Refused("the signature line is not base64")
    return sig, data[nl + 1:]


def rules_from_path(path):
    data = Path(path).read_bytes()
    if data.lstrip()[:1] == b"{":
        return data
    return split_signed(data)[1]


# ---------------------------------------------------------------------------- matcher
# A transcription of src/core/gphotos_parse.c's per-pattern state machine, so a rule can be tried against a
# saved page before it is signed. The C is the authority; test/test_gphotos_rules pins it.

def scan_text(pats, text):
    found = [[] for _ in pats]
    for i, p in enumerate(pats):
        anchor, n = p["anchor"], len(p["anchor"])
        fail = [0] * n
        k = 0
        for j in range(1, n):
            while k and anchor[j] != anchor[k]:
                k = fail[k - 1]
            if anchor[j] == anchor[k]:
                k += 1
            fail[j] = k
        phase, lit, key, tok, pos, num, digits, nums = "lit", 0, [], 0, 0, 0, 0, []

        def emit():
            found[i].append(("".join(key), nums[:2]))

        for c in text + "\0":
            end = c == "\0"
            while True:
                if end:
                    if phase == "tok" and tok + 1 == len(p["after"]) \
                            and p["after"][tok][0] == "num" and digits >= 2:
                        nums.append(num)
                        emit()
                    elif phase == "key" and not p["after"] and key:
                        emit()
                    break
                if phase == "lit":
                    while lit and c != anchor[lit]:
                        lit = fail[lit - 1]
                    if c == anchor[lit]:
                        lit += 1
                    if lit == n:
                        phase, key, nums, lit = "key", [], [], 0
                    break
                if phase == "key":
                    if c in p["chars"]:
                        if len(key) < p["max"]:
                            key.append(c)
                            break
                    elif key:
                        if not p["after"]:
                            emit()
                            phase, lit = "lit", 0
                        else:
                            phase, tok, pos, num, digits = "tok", 0, 0, 0, 0
                        continue
                    phase, lit = "lit", 0
                    continue
                kind, val = p["after"][tok]
                if kind == "lit":
                    if c != val[pos]:
                        phase, lit = "lit", 0
                        continue
                    pos += 1
                    if pos == len(val):
                        if tok + 1 == len(p["after"]):
                            emit()
                            phase, lit = "lit", 0
                        else:
                            tok, pos, num, digits = tok + 1, 0, 0, 0
                    break
                if c.isdigit() and c.isascii() and digits < 5:
                    num, digits = num * 10 + int(c), digits + 1
                    break
                if digits < 2:
                    phase, lit = "lit", 0
                    continue
                nums.append(num)
                if tok + 1 == len(p["after"]):
                    emit()
                    phase, lit = "lit", 0
                else:
                    tok, pos, num, digits = tok + 1, 0, 0, 0
                continue
    return found


# ---------------------------------------------------------------------------- commands

def cmd_keygen(args):
    priv_path = Path(args.private)
    if priv_path.exists():
        raise SystemExit(f"{priv_path} exists; not overwriting a signing key")
    if HEADER.exists():
        raise SystemExit(f"{HEADER} exists; delete it first if a new key is really meant")
    if REPO in priv_path.resolve().parents:
        raise SystemExit("the private key must live outside the repository")
    key = ec.generate_private_key(ec.SECP256R1())
    priv_path.parent.mkdir(parents=True, exist_ok=True)
    priv_path.write_bytes(key.private_bytes(serialization.Encoding.PEM,
                                            serialization.PrivateFormat.PKCS8,
                                            serialization.NoEncryption()))
    pub = key.public_key().public_bytes(serialization.Encoding.PEM,
                                        serialization.PublicFormat.SubjectPublicKeyInfo).decode()
    lines = "\n".join(f'    "{l}\\n"' for l in pub.strip().splitlines())
    HEADER.write_text(
        "// The public half of the key that signs Google Photos scrape rules.\n"
        "//\n"
        "// Generated by tools/gphotos_rules_sign.py keygen. The private half is not in this\n"
        "// repository. A rule file that does not verify against this key is never used\n"
        "// (src/app/app_gphotos_rules.h), so replacing it means a reflash of every frame.\n"
        "\n"
        "#ifndef GPHOTOS_RULES_PUBKEY_H\n#define GPHOTOS_RULES_PUBKEY_H\n\n"
        f"static const char GPHOTOS_RULES_PUBKEY_PEM[] =\n{lines};\n\n"
        "#endif // GPHOTOS_RULES_PUBKEY_H\n",
        encoding="utf-8", newline="\n")
    print(f"private key: {priv_path}  <- back this up")
    print(f"public key:  {HEADER}")


def cmd_check(args):
    doc = json.loads(Path(args.rules).read_bytes())
    pats = check_rules(doc)
    print(f"ok: seq {doc['seq']}, patterns {', '.join(p['id'] for p in pats)}")


def cmd_sign(args):
    payload = Path(args.rules).read_bytes()
    doc = json.loads(payload)
    check_rules(doc)
    key = serialization.load_pem_private_key(Path(args.private).read_bytes(), password=None)
    sig = key.sign(payload, ec.ECDSA(hashes.SHA256()))
    data = base64.b64encode(sig) + b"\n" + payload
    if len(data) > FILE_MAX:
        raise Refused(f"the signed file is {len(data)} bytes, over {FILE_MAX}")
    out = Path(args.out) if args.out else Path(args.rules).with_suffix(".signed")
    out.write_bytes(data)
    print(f"signed seq {doc['seq']}: {out} ({len(data)} B)")


def cmd_verify(args):
    data = Path(args.signed).read_bytes()
    if len(data) > FILE_MAX:
        raise Refused(f"the file is {len(data)} bytes, over {FILE_MAX}")
    sig, payload = split_signed(data)
    try:
        load_header_key().verify(sig, payload, ec.ECDSA(hashes.SHA256()))
    except InvalidSignature:
        raise Refused("the signature does not verify")
    doc = json.loads(payload)
    pats = check_rules(doc)
    print(f"ok: seq {doc['seq']}, patterns {', '.join(p['id'] for p in pats)}, {len(data)} B")


def cmd_scan(args):
    doc = json.loads(rules_from_path(args.rules))
    pats = check_rules(doc)
    text = Path(args.page).read_bytes().decode("utf-8", errors="replace")
    total = set()
    for p, items in zip(pats, scan_text(pats, text)):
        keys = {k for k, _ in items}
        total |= keys
        lens = sorted({len(k) for k in keys})
        dims = sum(1 for _, n in items if len(n) >= 2)
        print(f"{p['id']}: {len(items)} matches, {len(keys)} distinct keys, key lengths {lens}, "
              f"{dims} with two numbers")
    print(f"all patterns: {len(total)} distinct keys (the frame keeps the first of a repeated key)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    k = sub.add_parser("keygen")
    k.add_argument("--private", default=str(PRIVATE))
    k.set_defaults(fn=cmd_keygen)
    s = sub.add_parser("sign")
    s.add_argument("rules")
    s.add_argument("--private", default=str(PRIVATE))
    s.add_argument("-o", "--out")
    s.set_defaults(fn=cmd_sign)
    v = sub.add_parser("verify")
    v.add_argument("signed")
    v.set_defaults(fn=cmd_verify)
    c = sub.add_parser("check")
    c.add_argument("rules")
    c.set_defaults(fn=cmd_check)
    sc = sub.add_parser("scan")
    sc.add_argument("rules")
    sc.add_argument("page")
    sc.set_defaults(fn=cmd_scan)
    args = ap.parse_args()
    try:
        args.fn(args)
    except Refused as e:
        raise SystemExit(f"refused: {e}")


if __name__ == "__main__":
    main()
