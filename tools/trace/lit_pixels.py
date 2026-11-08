"""Counts the lit (non-black) pixels of each PPM image: finds screen changes without opening the images.

Usage: python tools\\trace\\lit_pixels.py [--changes] FILE.ppm [...]   (or a DIR: all its .ppm files)
  --changes  only shows the images whose count (or size) changed from the previous one
"""
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compare_frames import read_ppm  # noqa: E402


def lit(rgb):
    return sum(1 for i in range(0, len(rgb), 3) if rgb[i] | rgb[i + 1] | rgb[i + 2])


def main():
    if sys.argv[1:2] in (["-h"], ["--help"]):
        print(__doc__)
        return 0
    args = sys.argv[1:]
    only_changes = "--changes" in args
    args = [a for a in args if a != "--changes"]
    if not args:
        print(__doc__)
        return 2
    files = []
    for a in args:
        files += sorted(glob.glob(os.path.join(a, "*.ppm"))) if os.path.isdir(a) else [a]
    prev = None
    for path in files:
        w, h, rgb = read_ppm(path)
        n = lit(rgb)
        if not only_changes or (w, h, n) != prev:
            print(f"{os.path.basename(path)} {w}x{h} lit {n}")
        prev = (w, h, n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
