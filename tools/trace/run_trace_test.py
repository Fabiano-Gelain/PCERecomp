"""Trace test: runs Geargrafx (gg_trace) and our runtime (pce_trace) for the same
number of instructions and requires the traces to be identical.

Usage: python run_trace_test.py <gg_trace.exe> <pce_trace.exe> <rom> <instructions> <work dir>
                                [--input <script>] [--from-frame Q] [--name <name>] [--bios <system card>]
(with --bios, <rom> is the CD image .cue)
Exit code: 0 = identical, 1 = diverged, 77 = ROM or gg_trace missing (test skipped).
Both run at the same time; the Geargrafx output is reused while nothing changes (gg_cache.py).
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gg_cache  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("gg")
    ap.add_argument("ours")
    ap.add_argument("rom")
    ap.add_argument("count")
    ap.add_argument("work")
    ap.add_argument("--input")
    ap.add_argument("--from-frame")
    ap.add_argument("--name", default="trace")
    ap.add_argument("--bios")
    a = ap.parse_args()

    if not os.path.exists(a.rom):
        print(f"ROM not found ({a.rom}): test skipped")
        return 77
    if a.bios and not os.path.exists(a.bios):
        print(f"BIOS not found ({a.bios}): test skipped")
        return 77
    if not os.path.exists(a.gg):
        print(f"gg_trace not built ({a.gg}): run tools\\reference_trace\\build.ps1. Test skipped")
        return 77
    os.makedirs(a.work, exist_ok=True)
    extra = []
    if a.input:
        extra += ["--input", a.input]
    if a.from_frame:
        extra += ["--from-frame", a.from_frame]
    if a.bios:
        extra += ["--bios", a.bios]
    ref_out = os.path.join(a.work, f"{a.name}_ref.txt")
    our_out = os.path.join(a.work, f"{a.name}_ours.txt")
    ref = gg_cache.start([a.gg] + extra + [a.rom, a.count, ref_out], [ref_out])
    ours = subprocess.run([a.ours] + extra + [a.rom, a.count, our_out])
    if ref.wait() != 0 or ours.returncode != 0:
        print("one of the tools failed (see the messages above)")
        return 1
    return subprocess.run([sys.executable, os.path.join(HERE, "compare_trace.py"), ref_out, our_out]).returncode


if __name__ == "__main__":
    sys.exit(main())
