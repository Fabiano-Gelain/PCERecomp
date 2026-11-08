"""Converts PPM images (those from pce_trace/gg_trace) to PNG, next to each one.

Usage: python tools\\trace\\ppm2png.py FILE.ppm [...]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compare_frames import read_ppm, write_png  # noqa: E402


def main():
    if sys.argv[1:2] in (["-h"], ["--help"]):
        print(__doc__)
        return 0
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    for path in sys.argv[1:]:
        w, h, rgb = read_ppm(path)
        write_png(os.path.splitext(path)[0] + ".png", w, h, rgb)
    return 0


if __name__ == "__main__":
    sys.exit(main())
