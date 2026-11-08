"""Test: a session recorded with the real BIOS, replayed with the BIOS routines in C (--hle), produces the
same image as the real BIOS, frame by frame, except in a given range.

The C routines take the exact time of the real BIOS; a few transient ranges still differ (ALLOWED: the CD chip
and the ADPCM chip handle one event per clock step, and the large C routines, such as the VBlank interrupt, do
not advance in the steps of the BIOS instructions). The test runs pce_trace twice at the same time
(with --bios, and with --bios --hle) and compares the image CRC of each frame.

Usage: python tools/trace/run_hle_timing_test.py <pce_trace> <system card> <image.cue> <session.txt>
                                                 <allowed> <frames> <output dir>
     allowed: range of frames where a difference is accepted (e.g. 2881-4248), or 0 for none
     (the session's save is the .brm with the same name, if it exists)
Exit code: 0 = equal, 1 = different, 77 = missing files (skipped).
"""
import os
import subprocess
import sys


def run(cmd):
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def read_hashes(path):
    out = {}
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) >= 3:
                out[int(p[0])] = p[2]
    return out


def main():
    if sys.argv[1:2] in (["-h"], ["--help"]):
        print(__doc__)
        return 0
    if len(sys.argv) != 8:
        print(__doc__)
        return 2
    exe, bios, cue, session, allowed, frames, out = sys.argv[1:8]
    frames = int(frames)
    lo, _, hi = allowed.partition("-")
    lo, hi = int(lo), int(hi or lo)
    for path in (bios, cue, session):
        if not os.path.exists(path):
            print("SKIPPED: missing %s" % path)
            return 77
    os.makedirs(out, exist_ok=True)
    bram = os.path.splitext(session)[0] + ".brm"
    base = [exe, "--frame", "--input", session] + (["--bram", bram] if os.path.exists(bram) else []) + ["--bios", bios]
    real_h, hle_h = os.path.join(out, "bios_real.txt"), os.path.join(out, "bios_c.txt")
    procs = [run(base + ["--hashes", real_h, cue, str(frames), os.path.join(out, "real.ppm")]),
             run(base + ["--hle", "--hashes", hle_h, cue, str(frames), os.path.join(out, "c.ppm")])]
    if any(p.wait() != 0 for p in procs):
        print("FAILED: pce_trace did not finish the session")
        return 1
    real, hle = read_hashes(real_h), read_hashes(hle_h)
    every = [f for f in range(1, frames + 1) if real.get(f) != hle.get(f)]
    diff = [f for f in every if not lo <= f <= hi]
    print("frames 1-%d: %d different (%d in the allowed range %d-%d)" %
          (frames, len(every), len(every) - len(diff), lo, hi))
    if diff:
        print("FAILED: first different frame outside the range: %d" % diff[0])
        return 1
    print("OK: image equal to the real BIOS one in all frames from 1 to %d, except %d-%d" % (frames, lo, hi))
    return 0


if __name__ == "__main__":
    sys.exit(main())
