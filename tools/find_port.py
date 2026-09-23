#!/usr/bin/env python3
"""Resolve which COM port a board is on, right now, instead of trusting a number.

    python tools/find_port.py m5papercolor     -> COM11
    python tools/find_port.py e1002            -> COM14
    python tools/find_port.py --list           -> every candidate, with its hwid

The reTerminal E1002's CH340 takes whatever number Windows gives the socket it is
plugged into -- COM12 on 2026-09-17, COM14 on 09-18 and 09-19, same board, same cable.
A hard-coded number fails with FileNotFoundError and reads exactly like a board that is
switched off, so every invocation resolves the port instead of naming it:

    python tools/bringup_capture.py --board e1002 ...
    pio.exe run -e frame_e1002 -t upload --upload-port "$(python tools/find_port.py e1002)"

Matching is on USB VID:PID, not on the description string: COM11's description is
Japanese on this PC and a cp932 console cannot even print it. The serial number is
reported where the device carries one (USB-Serial-JTAG puts the MAC there, which is why
COM11 follows its board) and is only used to tell two same-VID devices apart.

Exit codes: 0 with the port on stdout, 2 when no port or more than one matches, and the
reason on stderr either way. Nothing is printed on stdout but the port, so the command
substitutes into a shell.
"""

import argparse
import sys

from serial.tools import list_ports

BOARDS = {
    # vid/pid, and how to say which board it is in a message.
    "m5papercolor": (0x303A, 0x1001, "M5Paper Color, native USB-Serial-JTAG"),
    "e1002": (0x1A86, 0x7523, "reTerminal E1002, CH340C on UART0"),
}


def candidates(board):
    vid, pid, _ = BOARDS[board]
    return [p for p in list_ports.comports() if p.vid == vid and p.pid == pid]


def describe(p):
    """hwid only -- it is ASCII, and p.description is not on a Japanese Windows."""
    return f"{p.device} {p.hwid}"


def resolve(board, serial=None):
    """The one port `board` is on. Raises LookupError with the reason if that is not one."""
    found = candidates(board)
    if serial:
        found = [p for p in found if p.serial_number == serial]
    if not found:
        vid, pid, what = BOARDS[board]
        raise LookupError(
            f"no {board} port ({what}, USB {vid:04X}:{pid:04X}"
            + (f", SER={serial}" if serial else "")
            + "). The board is unplugged, or switched off"
        )
    if len(found) > 1:
        listing = "\n".join("  " + describe(p) for p in found)
        how = ("pass --serial to pick one" if all(p.serial_number for p in found)
               else "these carry no serial number, so unplug one")
        raise LookupError(f"{len(found)} ports match {board}; {how}:\n{listing}")
    return found[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("board", nargs="?", choices=sorted(BOARDS))
    ap.add_argument("--serial", help="serial number, to tell two same-VID devices apart")
    ap.add_argument("--list", action="store_true",
                    help="every port either board could be on, and exit")
    args = ap.parse_args()

    if args.list:
        for board in sorted(BOARDS):
            for p in candidates(board):
                print(f"{board:13s} {describe(p)}")
        return 0
    if not args.board:
        ap.error("give a board, or --list")

    try:
        port = resolve(args.board, args.serial)
    except LookupError as e:
        sys.stderr.write(f"!! {e}\n")
        return 2
    sys.stderr.write(f"# {args.board} on {describe(port)}\n")
    print(port.device)
    return 0


if __name__ == "__main__":
    sys.exit(main())
