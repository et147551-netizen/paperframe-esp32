"""Build the listing-truncation fixtures on the SMB share.

arm1: 200 .txt named a000..a199 plus 10 .jpg named z000..z009. With the classifier
      behind the 200-entry listing cap the mirror sees only the .txt files and copies
      nothing, while reporting success.
arm2: 205 .jpg named b001..b205, for the wrongful-deletion arm; the a001..a010 that
      displace b191..b200 are added later by --shift.
"""

import base64
import pathlib
import shutil
import sys

# A minimal valid baseline JPEG, 1x1. Small on purpose: these arms are about how many
# files are listed, not about transfer time.
JPEG = base64.b64decode(
    "/9j/4AAQSkZJRgABAQEASABIAAD/2wBDAP//////////////////////////////////"
    "////////////////////////////////////////////////////////wAALCAABAAEB"
    "AREA/8QAFAABAAAAAAAAAAAAAAAAAAAACf/EABQQAQAAAAAAAAAAAAAAAAAAAAD/2gAI"
    "AQEAAD8AKp//2Q=="
)

ROOT = pathlib.Path(r"\\192.168.1.10\photos")


def build_arm1(d):
    # Order matters and was measured, not assumed: the share lists entries NEWEST FIRST
    # (2026-09-05, the z009..z006 fetch order of the first attempt). So the photographs
    # are written first, which puts them at the far end of the listing, and 250 non-photos
    # written after them more than fill the 200-entry window on their own.
    for i in range(10):
        (d / f"z{i:03d}.jpg").write_bytes(JPEG)
    for i in range(250):
        (d / f"a{i:03d}.txt").write_bytes(b"not a photograph\n")


def build_arm2(d):
    for i in range(1, 206):
        (d / f"b{i:03d}.jpg").write_bytes(JPEG)


def shift_arm2(d):
    for i in range(1, 11):
        (d / f"a{i:03d}.jpg").write_bytes(JPEG)


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "arm1"
    d = ROOT / ("smbtrunc" if what.startswith("arm1") else "smbtrunc2")

    if what == "clean":
        for name in ("smbtrunc", "smbtrunc2"):
            p = ROOT / name
            if p.exists():
                shutil.rmtree(p)
                print(f"removed {p}")
        return

    if what == "shift":
        shift_arm2(ROOT / "smbtrunc2")
    else:
        if d.exists():
            shutil.rmtree(d)
        d.mkdir(parents=True)
        (build_arm1 if what == "arm1" else build_arm2)(d)

    d = ROOT / ("smbtrunc" if what.startswith("arm1") else "smbtrunc2")
    names = sorted(p.name for p in d.iterdir())
    print(f"{d}: {len(names)} files")
    print("first 3:", names[:3], " last 3:", names[-3:])


if __name__ == "__main__":
    main()
