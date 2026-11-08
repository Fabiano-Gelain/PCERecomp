"""Reads the `pce_trace --dump-state` file (video and memory state at the end of the run).

Order in the file (16-bit little-endian words): VRAM (32768 words), SAT (256), VCE colors
(512), VDC registers (20), RAM (8 KB) and, with CD, the CD RAM (64 KB).

As a library: load(path) -> dict(vram, sat, col, reg, ram, cdram); tile_pixels(vram, n) -> 8x8
color indices (0-15) of tile n; rgb(color) -> (r, g, b) from 0 to 7.
As a program: python tools\\trace\\vdc_state.py STATE  -> registers, palette 0 and sprites in use.
"""
import struct
import sys


def load(path):
    d = open(path, "rb").read()
    o = 0
    vram = struct.unpack_from("<32768H", d, o)
    o += 65536
    sat = struct.unpack_from("<256H", d, o)
    o += 512
    col = struct.unpack_from("<512H", d, o)
    o += 1024
    reg = struct.unpack_from("<20H", d, o)
    o += 40
    ram = d[o:o + 8192]
    o += 8192
    cdram = d[o:o + 65536]
    return dict(vram=vram, sat=sat, col=col, reg=reg, ram=ram, cdram=cdram)


def rgb(c):
    """VCE color (9-bit GRB) -> (r, g, b), each from 0 to 7."""
    return ((c >> 3) & 7, (c >> 6) & 7, c & 7)


def tile_pixels(vram, t):
    """Background tile n (8x8, 4 planes): list of 8 rows with 8 color indices."""
    base = t * 16
    rows = []
    for y in range(8):
        w01 = vram[(base + y) & 0x7FFF]
        w23 = vram[(base + 8 + y) & 0x7FFF]
        row = []
        for x in range(8):
            b = 7 - x
            row.append(((w01 >> b) & 1) | (((w01 >> (8 + b)) & 1) << 1) | (((w23 >> b) & 1) << 2)
                       | (((w23 >> (8 + b)) & 1) << 3))
        rows.append(row)
    return rows


def main():
    if sys.argv[1:2] in (["-h"], ["--help"]):
        print(__doc__)
        return 0
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    s = load(sys.argv[1])
    print("VDC registers:", " ".join(f"{i:X}:{r:04X}" for i, r in enumerate(s["reg"])))
    print("palette 0:", [rgb(s["col"][i]) for i in range(16)])
    for i in range(64):
        y, x, pat, attr = s["sat"][i * 4:i * 4 + 4]
        if y or x:
            print(f"sprite {i}: x {x - 32} y {y - 64} pattern {pat:X} attributes {attr:X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
