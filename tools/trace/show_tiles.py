"""Shows VRAM tiles or the background map in the terminal, from `pce_trace --dump-state`.

Usage:
  python tools\\trace\\show_tiles.py STATE tiles "4B6,4B8;4B7,4B9"
      tiles side by side (separated by commas), rows of tiles separated by ';'; each pixel is shown
      as the color index (1-F) or '.' for color 0
  python tools\\trace\\show_tiles.py STATE map [ROWS] [COLUMNS]
      the background map (BAT): each entry as PNNN (P = palette, NNN = tile); default 32 x 32
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vdc_state import load, tile_pixels  # noqa: E402


def main():
    if sys.argv[1:2] in (["-h"], ["--help"]):
        print(__doc__)
        return 0
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    s = load(sys.argv[1])
    if sys.argv[2] == "tiles" and len(sys.argv) == 4:
        for line in sys.argv[3].split(";"):
            tiles = [tile_pixels(s["vram"], int(t, 16)) for t in line.split(",")]
            for y in range(8):
                print("".join("".join("0123456789ABCDEF"[v] if v else "." for v in t[y]) for t in tiles))
        return 0
    if sys.argv[2] == "map":
        mwr = s["reg"][9]
        width = [32, 64, 128, 128][(mwr >> 4) & 3]
        height = [32, 64][(mwr >> 6) & 1]
        rows = int(sys.argv[3]) if len(sys.argv) > 3 else 32
        cols = int(sys.argv[4]) if len(sys.argv) > 4 else 32
        print(f"map {width}x{height}, scroll X {s['reg'][7]} Y {s['reg'][8]}")
        for y in range(min(rows, height)):
            entries = (s["vram"][y * width + x] for x in range(min(cols, width)))
            print(f"{y:2d} " + " ".join(f"{e >> 12:X}{e & 0xFFF:03X}" for e in entries))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
