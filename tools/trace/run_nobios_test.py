"""Test: the CD game boots without the BIOS file (synthetic BIOS + routines in C, runtime/bios).

Runs pce_trace without --bios for N frames and checks: no BIOS routine missing, the CD music
played and the final image is not empty.

Usage: python tools/trace/run_nobios_test.py <pce_trace> <image.cue> <output dir> [frames]
           [--input <session> [--bram <save>]] [--adpcm <minimum seconds of ADPCM>]
Exit code: 0 = ok, 1 = failed, 77 = no CD image (test skipped).
"""
import os
import re
import subprocess
import sys


def main():
    if sys.argv[1:2] in (["-h"], ["--help"]):
        print(__doc__)
        return 0
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    exe, cue, out = sys.argv[1:4]
    rest = sys.argv[4:]
    frames = rest.pop(0) if rest and not rest[0].startswith("--") else "2500"
    opts = dict(zip(rest[0::2], rest[1::2]))
    extra = []
    for k in ("--input", "--bram"):
        if k in opts:
            extra += [k, opts[k]]
    if not os.path.exists(cue):
        print(f"no {cue}: test skipped")
        return 77
    os.makedirs(out, exist_ok=True)
    ppm = os.path.join(out, "no_bios.ppm")
    r = subprocess.run([exe, "--frame"] + extra + [cue, frames, ppm], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    text = r.stdout + r.stderr
    print(text.strip())
    ok = True
    if r.returncode != 0 or "Synthetic BIOS" in text:
        print("FAILED: booting without the BIOS stopped (see message above)")
        ok = False
    m = re.search(r"disc music played ([0-9.]+) s", text)
    if not m or float(m.group(1)) < 5:
        print("FAILED: the CD music did not play")
        ok = False
    if "--adpcm" in opts:
        m = re.search(r"ADPCM played ([0-9.]+) s", text)
        if not m or float(m.group(1)) < float(opts["--adpcm"]):
            print("FAILED: ADPCM did not play enough")
            ok = False
    data = open(ppm, "rb").read() if os.path.exists(ppm) else b""
    parts = data.split(b"\n", 3)
    pixels = parts[3] if len(parts) == 4 else b""
    lit = sum(1 for i in range(0, len(pixels) - 2, 3) if pixels[i] | pixels[i + 1] | pixels[i + 2])
    if not pixels or lit < len(pixels) // 3 // 20:
        print(f"FAILED: empty image ({lit} lit pixels)")
        ok = False
    print("OK: the game booted without the BIOS" if ok else "RESULT: failed")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
