"""Contact sheet: several PPM images reduced into a single PNG, in a grid (to look at many at once).

Usage: python tools\\trace\\contact_sheet.py OUTPUT.png COLUMNS REDUCTION FILE.ppm [...]
  REDUCTION = 2 shows each image at half the width and height (3 = one third...).
  The images are placed in argument order, left to right and top to bottom.
Together with `pce_trace --frame --every N DIR` (one frame every N), shows a whole sequence.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compare_frames import read_ppm, write_png  # noqa: E402


def main():
    if sys.argv[1:2] in (["-h"], ["--help"]):
        print(__doc__)
        return 0
    if len(sys.argv) < 5:
        print(__doc__)
        return 2
    out, cols, scale = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    imgs = [read_ppm(p) for p in sys.argv[4:]]
    cell_w = max(w for w, _, _ in imgs) // scale + 4
    cell_h = max(h for _, h, _ in imgs) // scale + 4
    rows = (len(imgs) + cols - 1) // cols
    width, height = cell_w * cols, cell_h * rows
    buf = bytearray([40]) * (width * height * 3)
    for k, (w, h, rgb) in enumerate(imgs):
        ox, oy = (k % cols) * cell_w + 2, (k // cols) * cell_h + 2
        for y in range(h // scale):
            for x in range(w // scale):
                src = ((y * scale) * w + x * scale) * 3
                dst = ((oy + y) * width + ox + x) * 3
                buf[dst:dst + 3] = rgb[src:src + 3]
    write_png(out, width, height, bytes(buf))
    return 0


if __name__ == "__main__":
    sys.exit(main())
