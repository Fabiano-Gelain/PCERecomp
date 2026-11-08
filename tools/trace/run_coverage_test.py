"""Coverage test: records the coverage of the button script with our runtime (pce_trace
--coverage) and checks it with the Python disassembler (recomp.py coverage).

Usage: python run_coverage_test.py <pce_trace.exe> <rom> <script> <frames> <output file>
                                   [--bios <system card>]
     With --bios, <rom> is the CD image (.cue): the coverage comes out in version 2 (code loaded from the disc
     with its origin) and is checked against the BIOS and the disc image (recomp.py coverage --cd). It also
     requires that there is code loaded from the disc and no byte differing from the disc.
Exit code: 0 = ok, 1 = problems, 77 = ROM, BIOS or CD missing (test skipped).
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RECOMP = os.path.join(os.path.dirname(HERE), "recomp", "recomp.py")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ours")
    ap.add_argument("rom")
    ap.add_argument("script")
    ap.add_argument("frames")
    ap.add_argument("out")
    ap.add_argument("--bios")
    a = ap.parse_args()
    for path in (a.rom, a.bios):
        if path and not os.path.exists(path):
            print(f"file not found ({path}): test skipped")
            return 77
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    bios = ["--bios", a.bios] if a.bios else []
    subprocess.run([a.ours, "--frame", "--input", a.script, "--coverage", a.out] + bios +
                   [a.rom, a.frames, a.out + ".ppm"], check=True)
    if not a.bios:
        return subprocess.run([sys.executable, RECOMP, "coverage", a.rom, a.out]).returncode

    r = subprocess.run([sys.executable, RECOMP, "coverage", a.bios, a.out, "--cd", a.rom],
                       capture_output=True, text=True)
    print(r.stdout, r.stderr)
    if r.returncode != 0:
        return 1
    if "disc loads with code" not in r.stdout or " 0 disc loads" in r.stdout:
        print("FAILED: no code loaded from the disc was recorded")
        return 1
    with open(a.out) as f:
        header = [line for line in f if line.startswith("# origin-bytes-checked")]
    if not header or not header[0].rstrip().endswith(" 0"):
        print(f"FAILED: bytes differing from the disc: {header}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
