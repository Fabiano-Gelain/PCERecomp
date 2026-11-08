"""Compares two PPM (P6) frames pixel by pixel and, optionally, saves PNGs.

Usage: python tools/trace/compare_frames.py <reference.ppm> <ours.ppm> [--png dir]

Exit code 0 = identical images, 1 = different. Uses only the Python standard library.
"""
import argparse
import os
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = []
    pos = 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        parts.append(data[start:pos])
    pos += 1
    if parts[0] != b"P6":
        raise ValueError(f"{path}: not a P6 PPM")
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[pos:pos + w * h * 3]


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(kind, payload):
        c = struct.pack(">I", len(payload)) + kind + payload
        return c + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference")
    ap.add_argument("ours")
    ap.add_argument("--png", help="directory where reference.png, ours.png and diff.png are saved")
    args = ap.parse_args()

    rw, rh, ref = read_ppm(args.reference)
    ow, oh, our = read_ppm(args.ours)

    if args.png:
        os.makedirs(args.png, exist_ok=True)
        write_png(os.path.join(args.png, "reference.png"), rw, rh, ref)
        write_png(os.path.join(args.png, "ours.png"), ow, oh, our)

    if (rw, rh) != (ow, oh):
        print(f"DIFFERENT: reference size {rw}x{rh}, ours {ow}x{oh}")
        return 1

    diff = bytearray(len(ref))
    count = 0
    first = None
    for i in range(0, len(ref), 3):
        if ref[i:i + 3] != our[i:i + 3]:
            count += 1
            diff[i:i + 3] = b"\xff\x00\xff"
            if first is None:
                first = (i // 3 % rw, i // 3 // rw)
        else:
            g = sum(ref[i:i + 3]) // 6  # darkened reference
            diff[i:i + 3] = bytes((g, g, g))
    if args.png:
        write_png(os.path.join(args.png, "diff.png"), rw, rh, bytes(diff))

    if count:
        print(f"DIFFERENT: {count} pixels of {rw * rh} (first at x={first[0]}, y={first[1]})")
        return 1
    print(f"EQUAL: {rw}x{rh}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
