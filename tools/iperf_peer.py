#!/usr/bin/env python3
"""The bench-PC peer for env:iperf (ticket 27).

Not a wrapper around iperf. There is no iperf binary of any version on this PC, and
`espressif/iperf` could not talk to iperf3 if there were: its TCP server performs no
handshake at all -- it accepts a connection and counts the bytes `recvfrom()` returns --
whereas iperf3 requires a control channel and a cookie exchange. Against that
implementation a plain socket is not an approximation of the peer, it is the same thing,
and it needs nothing installed.

Two roles, held at once in two threads, because the board runs two arms in one capture:

  listener  on --listen-port, draining          -- the board's CLIENT arm (board -> PC)
  connector to <board>:--board-port, blasting   -- the board's SERVER arm (board <- PC)

The board's IP does not have to be supplied. The listener learns it from the first
connection the board makes, and the connector then uses it; --board-ip skips the wait.

WHICH SIDE'S NUMBER COUNTS. Each direction has exactly one honest counter, and it is the
receiver's. A sender's `sendall()` returns when the kernel accepted the bytes, not when the
peer did, so its total includes whatever was still in flight when the connection closed.
So: for the board's client arm read THIS script's byte count; for the board's server arm
read the BOARD's `@@IPERF ... total_bytes=`. The other side of each pair is a cross-check,
and the two disagreeing by more than a window is itself information.

Run it before the board's capture starts and leave it running -- the board's server arm
calls accept() once per sample with no surrounding loop, so it needs a peer that keeps
coming back. Both roles retry forever; Ctrl-C prints the medians.

  python tools/iperf_peer.py --listen-port 5002 --board-port 5001
"""

import argparse
import socket
import statistics
import sys
import threading
import time

# 64 KB. Large enough that the send loop is not syscall-bound on this PC, small enough that
# a single sendall() cannot sit for long in a socket buffer at the rates in question.
CHUNK = b"\xa5" * 65536

# Sender-side cap per sample. The board's instance ends on its own timer and closes, which
# is the normal way a blast finishes; this only bounds the case where it does not.
BLAST_MAX_S = 40.0


class Samples:
    """Per-role results. Locked because the printer runs on the main thread."""

    def __init__(self, label):
        self.label = label
        self.rows = []
        self.lock = threading.Lock()

    def add(self, nbytes, seconds):
        # n read under the lock: with a thread per connection, several drains can finish at
        # once and a count read outside it would mislabel the samples.
        with self.lock:
            self.rows.append((nbytes, seconds))
            n = len(self.rows)
        # "n/a" rather than 0.0 when the interval did not resolve. A zero rate against a
        # non-zero byte count is a number that looks like a measurement and is not; the
        # loopback self-test printed exactly that before perf_counter() replaced
        # monotonic() below, and a real 10 s sample cannot hit it.
        rate = f"{nbytes / seconds / 1024.0:9.1f} KB/s" if seconds > 0 else "      n/a"
        print(
            f"[{self.label}] sample {n:2d}  "
            f"{nbytes:>12,} bytes  {seconds:6.2f} s  {rate}",
            flush=True,
        )

    def report(self):
        with self.lock:
            rows = list(self.rows)
        if not rows:
            print(f"[{self.label}] no samples")
            return
        rates = sorted(n / s / 1024.0 for n, s in rows if s > 0)
        if not rates:
            print(f"[{self.label}] {len(rows)} samples, none with a measurable interval")
            return
        if len(rates) < 4:
            # Nearest-rank quartiles below need n >= 4. Report the values themselves
            # rather than a spread computed from too few of them.
            print(f"[{self.label}] n={len(rates)} rates={[f'{r:.1f}' for r in rates]}")
            return
        med = statistics.median(rates)
        # IQR by nearest-rank, which is what this project reports elsewhere. With n=10 the
        # quartiles are the 3rd and 8th values; no interpolation is claimed.
        q1 = rates[len(rates) // 4]
        q3 = rates[(3 * len(rates)) // 4 - (1 if len(rates) % 4 == 0 else 0)]
        print(
            f"[{self.label}] n={len(rates)} median={med:.1f} KB/s "
            f"IQR={q1:.1f}-{q3:.1f} min={rates[0]:.1f} max={rates[-1]:.1f}"
        )


class BoardAddr:
    """The board's IP, either given or learned from its first connection."""

    def __init__(self, given):
        self.event = threading.Event()
        self.ip = given
        if given:
            self.event.set()

    def learn(self, ip):
        if not self.ip:
            self.ip = ip
            print(f"[peer] board is {ip} (learned from its first connection)", flush=True)
            self.event.set()

    def wait(self):
        self.event.wait()
        return self.ip


def drain_one(conn, addr, samples):
    """One accepted connection, drained to EOF. Runs on its own thread -- see below."""
    total = 0
    # First byte, not accept(), starts the clock: the interval being measured is the
    # transfer, and on the board's side the connect completes before its timer starts.
    began = None
    with conn:
        conn.settimeout(30.0)
        try:
            while True:
                buf = conn.recv(65536)
                if not buf:
                    break
                if began is None:
                    began = time.perf_counter()
                total += len(buf)
        except (socket.timeout, OSError) as exc:
            print(f"[recv] connection from {addr[0]} ended: {exc}", flush=True)
    if total and began is not None:
        samples.add(total, time.perf_counter() - began)


def listener_role(port, samples, board, stop):
    """Drain whatever the board sends. Receiver side, so this count is authoritative.

    A THREAD PER CONNECTION, and that is not decoration. This loop used to accept and drain
    inline, so a connection still being drained held the next one in the accept backlog --
    and the board opens a fresh connection per sample. The run of 2026-09-05 at window 5760
    shows exactly what that costs: client run 2 sent 131,072 bytes in its first second (the
    PC's 128 KB receive buffer, filled and then blocked), nothing for three seconds, and
    then 688-950 KB/s once the listener finished with run 1 and got round to it. Runs 1-2 of
    that arm are not usable, the peer recorded 11 samples for 10 runs, and one of them spans
    36.86 s of a 10 s instance. The board was fine; the instrument was the bottleneck.
    """
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("", port))
    srv.listen(8)
    srv.settimeout(1.0)
    print(f"[recv] listening on :{port} for the board's client arm", flush=True)

    while not stop.is_set():
        try:
            conn, addr = srv.accept()
        except socket.timeout:
            continue
        except OSError as exc:
            print(f"[recv] accept failed: {exc}", flush=True)
            break
        board.learn(addr[0])
        threading.Thread(target=drain_one, args=(conn, addr, samples), daemon=True).start()


def connector_role(port, samples, board, stop):
    """Blast at the board's server arm. The board's own count is the honest one here."""
    ip = board.wait()
    if stop.is_set():
        return
    print(f"[send] will connect to {ip}:{port} for the board's server arm", flush=True)

    while not stop.is_set():
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(2.0)
        try:
            sock.connect((ip, port))
        except OSError:
            # Refused is the normal state between samples: the board only listens while an
            # instance is open. Retrying is how the two sides stay in step without a clock.
            sock.close()
            time.sleep(0.25)
            continue
        sent = 0
        began = time.perf_counter()
        try:
            sock.settimeout(10.0)
            while time.perf_counter() - began < BLAST_MAX_S:
                sock.sendall(CHUNK)
                sent += len(CHUNK)
        except OSError:
            # The board closing when its timer expires is the expected end of a sample.
            pass
        finally:
            elapsed = time.perf_counter() - began
            sock.close()
        if sent:
            samples.add(sent, elapsed)
        time.sleep(0.25)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--listen-port", type=int, default=5002,
                    help="where the board's client arm pushes to (default 5002)")
    ap.add_argument("--board-port", type=int, default=5001,
                    help="where the board's server arm listens (default 5001)")
    ap.add_argument("--board-ip", default=None,
                    help="skip learning it from the board's first connection")
    args = ap.parse_args()

    recv = Samples("recv board->pc")
    send = Samples("send pc->board")
    board = BoardAddr(args.board_ip)
    stop = threading.Event()

    threads = [
        threading.Thread(target=listener_role, args=(args.listen_port, recv, board, stop),
                         daemon=True),
        threading.Thread(target=connector_role, args=(args.board_port, send, board, stop),
                         daemon=True),
    ]
    for t in threads:
        t.start()

    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        stop.set()
        print("\n--- medians ---")
        recv.report()
        send.report()
        print("Authoritative: board->pc from [recv] above; pc->board from the board's own")
        print("@@IPERF server ... total_bytes=. Each direction's receiver is the counter.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
