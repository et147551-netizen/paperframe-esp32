"""Path-traversal sweep against a running frame's HTTP API (ticket 79).

Every route that turns request data into a path under /data is asked for a name that tries to
leave it. The guard is app_server.c's name_is_safe(), and the question is whether the whole
chain -- esp_http_server's URI handling, then url_decode(), then that guard -- rejects the
attempt on a real device. Ticket 29 ("four guards that were not guarding") is why this is asked
of a device rather than of the source.

TWO THINGS THAT MAKE THIS A MEASUREMENT RATHER THAN A REASSURANCE:

  * The request is built with the path VERBATIM. `curl` collapses `..` in a path before sending
    unless you pass --path-as-is, and a sweep that forgets is measuring its own client's URI
    normaliser; here the socket is written by hand for the same reason. A payload that the
    client rewrites would pass the sweep and prove nothing.
  * A control row that MUST succeed. Without it, a sweep where every request fails for an
    unrelated reason -- a wrong token, a stopped httpd, a mistyped host -- reports a clean pass.

Every traversal payload names a leaf that exists nowhere, so even a guard that failed outright
could not disclose or delete anything: the worst case resolves to a missing file. That is
deliberate, and it is what makes including DELETE acceptable.

Usage:
    python tools/api_path_sweep.py --host 192.168.1.20 --token <t> [--control imaged001.png]
    python tools/api_path_sweep.py --host 192.168.1.30 --token <t> --no-delete

The verdict is computed from the printed table, not inside the loop: rows are raw
(route, payload, status, note) and the summary counts them at the end.
"""

import argparse
import json
import socket
import sys

# (label, payload as it appears in the URL path / the JSON name)
PAYLOADS = [
    ("raw-dotdot-slash", "../zzz-does-not-exist.jpg"),
    ("enc-dotdot-slash", "%2e%2e%2fzzz-does-not-exist.jpg"),
    ("dotdot-encslash", "..%2fzzz-does-not-exist.jpg"),
    ("encdot-rawslash", "%2e%2e/zzz-does-not-exist.jpg"),
    ("fourdots-slashes", "....//zzz-does-not-exist.jpg"),
    ("dotdot-backslash", "..%5czzz-does-not-exist.jpg"),
    ("raw-backslash", "..\\zzz-does-not-exist.jpg"),
    ("leading-slash", "%2fdata%2fzzz-does-not-exist.jpg"),
    ("double-encoded", "%252e%252e%252fzzz-does-not-exist.jpg"),
    ("deep-traversal", "%2e%2e%2f%2e%2e%2f%2e%2e%2fnvs"),
    ("encoded-nul", "zzz%00%2e%2e%2fzzz.jpg"),
    ("bare-dot", "."),
    ("bare-dotdot", ".."),
    ("long-name", "a" * 200 + ".jpg"),
    ("empty", ""),
    ("bad-escape", "%zz.jpg"),
    ("trailing-percent", "abc%"),
]


def request(host, method, path, body=None, timeout=15.0):
    """One HTTP/1.1 request with the request-target written verbatim.

    Returns (status:int|None, first 120 bytes of the body, note:str).
    """
    head = "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n" % (method, path, host)
    if body is not None:
        head += "Content-Type: application/json\r\nContent-Length: %d\r\n" % len(body)
    head += "\r\n"
    raw = head.encode("utf-8", "surrogateescape")
    if body is not None:
        raw += body.encode("utf-8")
    # STOP AT THE HEADERS PLUS A LOOK AT THE BODY -- do NOT read to EOF. `send_file()` answers
    # with httpd_resp_send_chunk(), and this server keeps that connection alive whatever the
    # request's `Connection: close` says, so there is no EOF to wait for: a client that waits
    # for one reports a timeout on a response that arrived in 200 ms. That is what the first two
    # runs of this sweep did to their own controls -- the /data/ row survived run 1 only because
    # its 14 KB body tripped an unrelated size cap, and when the cap went the row "failed" too.
    # The server was answering in 0.2 s throughout, checked with curl immediately afterwards.
    #
    # The status line is what this sweep is for; BODY_PEEK bytes of body are kept so a 200 can be
    # shown to be a real file and not an error page. Closing early is safe and is what curl does.
    BODY_PEEK = 256
    try:
        with socket.create_connection((host, 80), timeout=timeout) as s:
            s.settimeout(timeout)
            s.sendall(raw)
            data = b""
            while True:
                b = s.recv(1024)
                if not b:
                    break
                data += b
                sep = data.find(b"\r\n\r\n")
                if sep >= 0 and len(data) - (sep + 4) >= BODY_PEEK:
                    break
                if len(data) > 16384:
                    break
    except OSError as e:
        return None, b"", "socket: %s" % e
    if not data:
        return None, b"", "empty response"
    try:
        status = int(data.split(b" ", 2)[1])
    except (IndexError, ValueError):
        return None, data[:120], "unparseable status line"
    sep = data.find(b"\r\n\r\n")
    body_bytes = data[sep + 4:] if sep >= 0 else b""
    return status, body_bytes[:120], ""


def rows_for(host, token, control, do_delete, selftest=False):
    out = []
    payloads = list(PAYLOADS)
    if selftest:
        # A payload that MUST come back 200, so the detector below is shown to be able to
        # report a failure. Without this, "0 payload rows in 2xx" is a sentence that a broken
        # sweep also prints. It is a GET row only: nothing here should draw on the panel.
        payloads.append(("SELFTEST-should-fail", control))
    for label, payload in payloads:
        out.append(("GET /data/", label, "/data/%s?t=%s" % (payload, token), "GET", None))
        if label.startswith("SELFTEST"):
            continue
        out.append(("GET /thumb/", label, "/thumb/%s?t=%s" % (payload, token), "GET", None))
        if do_delete:
            out.append(("DELETE delete", label,
                        "/api/photos/delete?t=%s&name=%s" % (token, payload), "DELETE", None))
        out.append(("POST display", label, "/api/photos/display?t=%s" % token, "POST",
                    json.dumps({"name": payload})))
    # The controls, last so a server that died mid-sweep is visible as a failed control.
    out.append(("GET /data/", "CONTROL", "/data/%s?t=%s" % (control, token), "GET", None))
    out.append(("GET /thumb/", "CONTROL", "/thumb/%s?t=%s" % (control, token), "GET", None))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--token", required=True)
    ap.add_argument("--control", default="imaged001.png",
                    help="a name that really is on /data; the row that must succeed")
    ap.add_argument("--no-delete", action="store_true",
                    help="skip DELETE /api/photos/delete rows")
    ap.add_argument("--selftest", action="store_true",
                    help="add one payload row that must be reported as a failure, to show that "
                         "the detector can fail; the run then EXPECTS a non-zero exit")
    args = ap.parse_args()

    rows = rows_for(args.host, args.token, args.control, not args.no_delete, args.selftest)
    results = []
    for route, label, path, method, body in rows:
        status, head, note = request(args.host, method, path, body)
        results.append((route, label, status, head, note))
        print("%-14s %-18s %-5s %-40r %s" % (route, label, status, head[:40], note))

    print("")
    bad = [r for r in results
           if r[1] != "CONTROL" and r[2] is not None and 200 <= r[2] < 300]
    controls = [r for r in results if r[1] == "CONTROL"]
    nostatus = [r for r in results if r[2] is None]
    print("rows=%d  2xx-on-a-payload=%d  no-status=%d" % (len(results), len(bad), len(nostatus)))
    for r in controls:
        print("control %-12s -> %s (%d bytes shown)" % (r[0], r[2], len(r[3])))
    codes = {}
    for r in results:
        if r[1] != "CONTROL":
            codes[r[2]] = codes.get(r[2], 0) + 1
    print("payload status histogram: %s" % sorted(codes.items(), key=lambda kv: str(kv[0])))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
