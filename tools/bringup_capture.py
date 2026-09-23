#!/usr/bin/env python3
"""Watch the serial console and photograph the panel when the firmware says to.

Timing a camera against a 12-second refresh by hand does not work, and a photograph
taken mid-waveform shows a half-drawn frame that looks like a driver bug. So the
firmware prints a marker once the frame is on the glass and settled, and this script
takes the picture. See src/bringup_main.c.

    python tools/bringup_capture.py --board m5papercolor

Markers understood, both emitted by the firmware:
    @@CAPTURE <label>   photograph now, filing the shot under <label>
    @@DONE              the run is over -- except from env:frame, see FRAME_BOOT_LINES

Everything else on the console is echoed through and saved to a log, because the
I2C scan and the per-refresh CSV rows are results in their own right.
"""

import argparse
import datetime
import pathlib
import subprocess
import sys
import time

import serial

import find_port

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
CAPTURE_SCRIPT = REPO_ROOT / "tools" / "capture_panel.py"
SCAN_SCRIPT = REPO_ROOT / "tools" / "scan_panel.ps1"
LOG_DIR = REPO_ROOT / ".scratch" / "captures"

# The panel's corner of the bed, in inches. From docs/agents/hardware-runs.md, which is
# where the WIA property-ID and extent-unit traps are written down.
SCAN_REGION = ("-XInch", "0", "-YInch", "0", "-WInch", "3.5", "-HInch", "4.8")

# env:frame prints @@DONE two minutes in and then keeps running (src/frame_main.c), so the default
# end marker stops a frame capture at t=120 s -- and closing the port resets the board. It happened
# twice for the pairing arm and once for ticket 60's collision arm, whose driver then ran with
# nothing recording, all with the trap already written down. So once one of these lines -- printed
# only by env:frame's boot -- has been seen, the DEFAULT marker is ignored and the capture says so.
# An explicit --until is honoured as given.
FRAME_BOOT_LINES = ("# smb mirror init:", "# http server:")

# AND A MARKER THAT IS NOT ON THE BOOT PATH, because the two above are, and that is how the trap came
# back on 2026-09-21 (ticket 76) after this guard had been in place since 2026-09-13. The sequence:
# a previous capture's close resets the board (see the note at the top), the next capture attaches
# five seconds into the new boot and therefore MISSES both lines above, and then @@DONE arrives at
# t=120 s and stops a capture that was asked for 580. Its own close resets the board again, so a
# series of these is a series of two-minute captures each costing a reboot -- which is what happened
# to an arm that was driving 20 renders over HTTP and recorded five of them.
#
# The frame's heartbeat carries `| display busy=` every 10 s for as long as it runs, so it identifies
# an env:frame capture whenever the capture was attached, not only when it saw the boot. It is not
# `startswith`-able, hence the separate substring test at the use site. Nothing else prints it:
# app_display's heartbeat exists only under BUILD_FRAME.
FRAME_LIVE_LINE = "| display busy="


def take_photo(label, shots=1, pixel_format=None):
    """Returns every path capture_panel.py wrote, one per shot."""
    cmd = [sys.executable, str(CAPTURE_SCRIPT), label]
    if shots > 1:
        cmd += ["--shots", str(shots)]
    if pixel_format:
        cmd += ["--pixel-format", pixel_format]

    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(f"!! capture failed for {label}: {proc.stderr}\n")
        return []
    return [line for line in proc.stdout.splitlines() if line.strip()]


def take_scan(label, dpi, region=SCAN_REGION):
    """The flatbed instead of the camera. Returns the one path it wrote, or []."""
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = LOG_DIR / f"{label}-{stamp}.bmp"
    cmd = ["powershell", "-NoProfile", "-File", str(SCAN_SCRIPT), "-Out", str(out),
           "-Dpi", str(dpi), *region]

    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0 or not out.exists():
        sys.stderr.write(f"!! scan failed for {label}: {proc.stderr}\n")
        return []
    return [str(out)]


def main():
    ap = argparse.ArgumentParser()
    # A port NUMBER is not a fact about a board -- the E1002's CH340 has been COM12 and COM14 on
    # the same cable, and a number that has moved fails in a way that reads like a board switched
    # off. So name the board and let tools/find_port.py say which port it is on today. --port is
    # still there for a third device, and is identified before it is opened.
    ap.add_argument("--board", choices=sorted(find_port.BOARDS), default="m5papercolor")
    ap.add_argument("--port", help="a literal port, when the device is not one of --board's")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=float, default=300.0,
                    help="give up if the run has not finished in this many seconds")
    ap.add_argument("--until", default="@@DONE",
                    help="line prefix that marks the end of the run; the harness "
                         "ends with '# done' rather than the bring-up's @@DONE")
    ap.add_argument("--label", default="bringup",
                    help="log filename stem")
    ap.add_argument("--shots", type=int, default=1,
                    help="photographs per @@CAPTURE marker; passed through to "
                         "capture_panel.py. More than one gives the noise floor")
    ap.add_argument("--pixel-format",
                    help="passed through to capture_panel.py, e.g. yuyv422")
    ap.add_argument("--scanner", action="store_true",
                    help="capture with the flatbed (tools/scan_panel.ps1) instead of "
                         "the webcam, which has been unplugged since 2026-09-02. The "
                         "panel lies face-down in the corner of the bed; --shots and "
                         "--pixel-format do not apply")
    ap.add_argument("--dpi", type=int, default=150,
                    help="scan resolution with --scanner. 150 answers 'did an image "
                         "appear and are the colours distinguishable'; 600 is what "
                         "resolves individual dither dots, and takes four times as long")
    # SCAN_REGION's default is the M5Paper Color's 3.5 x 4.8 inch corner. The reTerminal E1002's
    # panel is 7.3" landscape and does not fit inside that rectangle, so the region is where the
    # board actually sits rather than a constant. Inches, because WIA's extents are in 300 dpi
    # units whatever the scan resolution -- see docs/agents/hardware-runs.md.
    ap.add_argument("--scan-x", type=float, help="scan origin X in inches")
    ap.add_argument("--scan-y", type=float, help="scan origin Y in inches")
    ap.add_argument("--scan-w", type=float, help="scan width in inches")
    ap.add_argument("--scan-h", type=float, help="scan height in inches")
    ap.add_argument("--reset", action="store_true", default=True,
                    help="pulse RTS to restart the device so the log starts at boot")
    ap.add_argument("--no-reset", dest="reset", action="store_false",
                    help="attach to a device that is already running. Needed once the "
                         "firmware serves a network: a reset drops the softAP, and "
                         "whatever was talking to it has to find its way back")
    args = ap.parse_args()

    scan_region = SCAN_REGION
    if any(v is not None for v in (args.scan_x, args.scan_y, args.scan_w, args.scan_h)):
        # All four together or none: three of four would silently mix this board's origin with
        # another board's extent, and a scan of the wrong rectangle looks like a blank panel.
        if any(v is None for v in (args.scan_x, args.scan_y, args.scan_w, args.scan_h)):
            ap.error("--scan-x/-y/-w/-h must be given together")
        scan_region = ("-XInch", str(args.scan_x), "-YInch", str(args.scan_y),
                       "-WInch", str(args.scan_w), "-HInch", str(args.scan_h))

    LOG_DIR.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    log_path = LOG_DIR / f"{args.label}-{stamp}.log"

    # Check the port every run, before anything is opened: which board is actually there, and on
    # which number today. Printed and logged, because a capture whose board is in doubt afterwards
    # is a capture that has to be run again.
    if args.port:
        port = args.port
        seen = [p for b in find_port.BOARDS for p in find_port.candidates(b) if p.device == port]
        port_line = (f"# port {find_port.describe(seen[0])}" if seen
                     else f"# port {port} (not one of this project's boards)")
    else:
        try:
            resolved = find_port.resolve(args.board)
        except LookupError as e:
            sys.stderr.write(f"!! {e}\n")
            return 1
        port = resolved.device
        port_line = f"# port {args.board} on {find_port.describe(resolved)}"
    print(port_line)

    s = serial.Serial(port, args.baud, timeout=1)
    if args.reset:
        s.setDTR(False)
        s.setRTS(True)
        time.sleep(0.2)
        # Flush while the device is still held in reset, not after it is released. This used to
        # sleep 0.1 s after releasing RTS and flush then, which threw away whatever the device
        # printed in that window; flushing here discards the stale bytes from before the reset,
        # which is the only thing the flush was ever for.
        #
        # **It does not recover the bootloader's output, and that was the point of changing it.**
        # No capture in .scratch/captures/ has ever contained a `boot:` line and every one starts
        # partway into `cpu_start` -- 646 to 880 ms in, depending on the build. The flush order was
        # genuinely wrong and fixing it changed nothing, because the console here is
        # USB-Serial-JTAG: a reset drops the USB device and the host loses the port until it
        # re-enumerates, so the ROM banner and the whole second-stage bootloader are gone before
        # anything can read them. That is not fixable from this end.
        #
        # What to do instead, when the question is about something only the bootloader prints:
        # find the app-level equivalent. `boot: SPI Mode : QIO` is unreachable, but the
        # `spi_flash` component prints `flash io: qio` at INFO once the app is up, and that is
        # what verified the flash-mode arm on 2026-09-05.
        s.reset_input_buffer()
        s.setRTS(False)

    shots = []
    until_given = any(a == "--until" or a.startswith("--until=") for a in sys.argv[1:])
    is_frame = False
    deadline = time.time() + args.timeout
    buf = b""
    with log_path.open("w", encoding="utf-8") as log:
        log.write(port_line + "\n")
        log.flush()
        while time.time() < deadline:
            chunk = s.read(4096)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").rstrip("\r")
                # **The ECHO is what kills a run, not the decode.** The decode above already
                # replaces a bad byte with U+FFFD, but this console is cp932 and cannot ENCODE
                # U+FFFD, so `print(line)` raised UnicodeEncodeError and took the capture process
                # with it -- twice on 2026-09-19, both times at the exact moment the firmware first
                # printed the matte band's degree sign (byte 0xB0, ticket 64). The log file is
                # UTF-8 and was never the problem; a run died for the sake of its own echo.
                #
                # The log keeps the real text. The terminal gets whatever it can represent.
                try:
                    print(line)
                except UnicodeEncodeError:
                    enc = sys.stdout.encoding or "ascii"
                    print(line.encode(enc, "replace").decode(enc, "replace"))
                log.write(line + "\n")
                log.flush()

                if not is_frame and (line.startswith(FRAME_BOOT_LINES)
                                     or FRAME_LIVE_LINE in line):
                    is_frame = True

                if line.startswith("@@CAPTURE "):
                    # Prefixed with the run's label, because a second run of the same
                    # firmware emits exactly the same marker labels and its photographs
                    # would otherwise land in the same directory under the same names,
                    # silently merging two runs into one dataset.
                    label = f"{args.label}-" + line[len("@@CAPTURE "):].strip()
                    paths = (take_scan(label, args.dpi, scan_region) if args.scanner
                             else take_photo(label, args.shots, args.pixel_format))
                    for path in paths:
                        print(f"   -> {path}")
                        log.write(f"   -> {path}\n")
                        log.flush()
                        shots.append(path)
                elif line.startswith(args.until):
                    if is_frame and not until_given:
                        note = ("!! env:frame prints @@DONE at t=120 s and keeps running, so the "
                                "default end marker is ignored; --timeout ends this capture. "
                                "Pass --until to choose a marker (docs/agents/hardware-runs.md)")
                        print(note)
                        log.write(note + "\n")
                        log.flush()
                        continue
                    s.close()
                    print(f"\nlog: {log_path}")
                    print(f"{len(shots)} captures")
                    return 0
    s.close()
    sys.stderr.write(f"\ntimed out after {args.timeout}s; log: {log_path}\n")
    return 1


if __name__ == "__main__":
    sys.exit(main())
