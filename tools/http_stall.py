#!/usr/bin/env python3
"""Drive a multipart upload that stops sending, and time what the frame does about it.

Two arms out of one script:

    --hold 90                 headers plus one chunk, then silence. The defect arm.
    --interval 10 --chunks 6  a chunk every ten seconds. The refutation arm: a client
                              that is slow but progressing must NOT be cut off, and this
                              is what catches a total-time budget dressed up as an idle
                              deadline.

Everything is raw sockets on purpose. requests and curl both insist on sending the body
they promised in Content-Length, and the whole point here is a client that does not.
"""

import argparse
import socket
import sys
import time

BOUNDARY = "----PaperFrameStallProbe"


def build_head(filename, content_len):
    """The request line, headers, and the first part's own header block."""
    part = (
        f"--{BOUNDARY}\r\n"
        f'Content-Disposition: form-data; name="file"; filename="{filename}"\r\n'
        f"Content-Type: image/jpeg\r\n"
        f"\r\n"
    )
    head = (
        f"POST /api/photos/upload HTTP/1.1\r\n"
        f"Host: {{host}}\r\n"
        f"Content-Type: multipart/form-data; boundary={BOUNDARY}\r\n"
        f"Content-Length: {content_len}\r\n"
        f"Connection: close\r\n"
        f"\r\n"
    )
    return head, part


def build_tail(algorithm="dither", action="upload_only"):
    """The trailing fields the handler needs, then the closing boundary."""
    out = ""
    for name, value in (("algorithm", algorithm), ("action", action)):
        out += (
            f"\r\n--{BOUNDARY}\r\n"
            f'Content-Disposition: form-data; name="{name}"\r\n'
            f"\r\n"
            f"{value}"
        )
    out += f"\r\n--{BOUNDARY}--\r\n"
    return out


def jpeg_bytes(n):
    """n bytes that begin like a JPEG. Only the first two matter to the decoder, and in
    the arms that never finish nothing decodes them at all."""
    return b"\xff\xd8\xff\xe0" + bytes(((i * 7) & 0xFF) for i in range(n - 4))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.1.20")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--filename", default="stall.jpg")
    ap.add_argument("--chunk", type=int, default=1024, help="bytes per body chunk")
    ap.add_argument("--chunks", type=int, default=1, help="how many chunks to send")
    ap.add_argument("--interval", type=float, default=0.0,
                    help="seconds between chunks (0 = send them back to back)")
    ap.add_argument("--hold", type=float, default=0.0,
                    help="seconds to hold the socket open after the last chunk, sending "
                         "nothing. The stall.")
    ap.add_argument("--finish", action="store_true",
                    help="after the chunks, send the closing boundary so the upload can "
                         "actually complete")
    ap.add_argument("--timeout", type=float, default=180.0,
                    help="how long to wait for a response once we stop writing")
    args = ap.parse_args()

    body_len = args.chunk * args.chunks
    tail = build_tail()
    head, part = build_head(args.filename, 0)
    # Content-Length is a promise the stall arms deliberately break: it claims the whole
    # body, including a tail that --finish may never send. That is exactly the client
    # this is meant to imitate.
    declared = len(part) + body_len + len(tail)
    head, part = build_head(args.filename, declared)
    head = head.format(host=args.host)

    t0 = time.monotonic()

    def stamp(msg):
        print(f"[{time.monotonic() - t0:7.2f}s] {msg}", flush=True)

    s = socket.create_connection((args.host, args.port), timeout=10.0)
    stamp(f"connected to {args.host}:{args.port}, declaring {declared} body bytes")
    s.sendall(head.encode() + part.encode())
    stamp("headers sent")

    payload = jpeg_bytes(args.chunk)
    for i in range(args.chunks):
        if i and args.interval:
            time.sleep(args.interval)
        s.sendall(payload)
        stamp(f"chunk {i + 1}/{args.chunks} sent ({args.chunk} bytes)")

    if args.finish:
        s.sendall(tail.encode())
        stamp("closing boundary sent")

    if args.hold:
        stamp(f"holding the socket open, sending nothing, for {args.hold:g} s")
        # Read with a deadline rather than sleeping: the whole question is whether an
        # answer arrives while we are silent, and when.
        s.settimeout(args.hold)
        try:
            early = s.recv(4096)
            if early:
                stamp(f"answered DURING the hold: {early.decode(errors='replace')[:200]!r}")
                s.close()
                return 0
            stamp("peer closed during the hold")
            s.close()
            return 0
        except socket.timeout:
            stamp("held to the end with no answer")

    stamp(f"waiting up to {args.timeout:g} s for a response")
    s.settimeout(args.timeout)
    chunks = []
    try:
        while True:
            b = s.recv(4096)
            if not b:
                break
            chunks.append(b)
    except socket.timeout:
        stamp("no response before the wait ran out")
        s.close()
        return 2

    resp = b"".join(chunks).decode(errors="replace")
    status = resp.split("\r\n", 1)[0] if resp else "(nothing)"
    stamp(f"response: {status}")
    body = resp.split("\r\n\r\n", 1)[1] if "\r\n\r\n" in resp else ""
    if body.strip():
        stamp(f"body: {body.strip()[:300]}")
    s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
