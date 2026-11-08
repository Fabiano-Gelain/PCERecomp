"""Test of the PCERecomp tools without any game:

  - pce_trace with a synthetic ROM built here (a loop that counts in X and writes to RAM): trace of 1000
    instructions in the gg_trace format, 3 frames with --hashes and the image, and the same sums in a 2nd run;
  - --help of each tool (programs and scripts): exits with 0 and shows the usage.

Usage: python run_tools_test.py <pce_trace.exe> <bios_check.exe> <cd_info.exe> <work dir>
Exit code: 0 = passed, 1 = failed.
"""
import os
import subprocess
import sys

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS = ["recomp/recomp.py", "recomp/bios_report.py", "cd/run_cd_test.py",
           "trace/compare_audio.py", "trace/compare_frames.py", "trace/compare_hle_trace.py",
           "trace/compare_trace.py", "trace/contact_sheet.py", "trace/lit_pixels.py", "trace/ppm2png.py",
           "trace/run_coverage_test.py", "trace/run_hle_timing_test.py", "trace/run_nobios_test.py",
           "trace/run_trace_test.py", "trace/run_tools_test.py", "trace/show_tiles.py", "trace/vdc_state.py"]

# $E000: SEI; CSH; LDA #$FF; TAM #1 (I/O); LDA #$F8; TAM #2 (RAM); LDX #0
# $E00C: INX; STX $2000; BRA $E00C
CODE = bytes([0x78, 0xD4, 0xA9, 0xFF, 0x53, 0x01, 0xA9, 0xF8, 0x53, 0x02, 0xA2, 0x00,
              0xE8, 0x8E, 0x00, 0x20, 0x80, 0xFA])


def make_rom(path):
    rom = bytearray(0x2000)
    rom[:len(CODE)] = CODE
    for vec in range(0x1FF6, 0x2000, 2):  # all vectors (IRQ2, IRQ1, TIMER, NMI, RESET) at $E000
        rom[vec], rom[vec + 1] = 0x00, 0xE0
    with open(path, "wb") as f:
        f.write(rom)


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")


def main():
    if sys.argv[1:2] in (["-h"], ["--help"]):
        print(__doc__)
        return 0
    if len(sys.argv) != 5:
        print(__doc__)
        return 1
    trace_exe, bios_check, cd_info, work = sys.argv[1:5]
    os.makedirs(work, exist_ok=True)
    fails = []

    def check(ok, what):
        print(("ok   " if ok else "FAIL ") + what)
        if not ok:
            fails.append(what)

    rom = os.path.join(work, "synthetic.pce")
    make_rom(rom)

    out = os.path.join(work, "trace.txt")
    r = run([trace_exe, rom, "1000", out])
    lines = open(out).read().splitlines() if r.returncode == 0 and os.path.exists(out) else []
    check(len(lines) == 1000, f"trace: 1000 lines (got {len(lines)}, exit code {r.returncode})")
    check(bool(lines) and lines[0].startswith("E000 A:") and " M:" in lines[0], f"trace: 1st line {lines[:1]}")
    loop = [ln.split()[0] for ln in lines[7:13]]
    check(loop == ["E00C", "E00D", "E010", "E00C", "E00D", "E010"], f"trace: the loop ($E00C) {loop}")
    check(len(lines) > 12 and " X:02 " in lines[13], f"trace: X counts ({lines[13] if len(lines) > 13 else '-'})")

    sums = []
    for k in range(2):
        ppm = os.path.join(work, f"frame{k}.ppm")
        hashes = os.path.join(work, f"hashes{k}.txt")
        r = run([trace_exe, "--frame", "--hashes", hashes, rom, "3", ppm])
        ok = r.returncode == 0 and os.path.exists(ppm) and os.path.exists(hashes)
        check(ok, f"--frame: 3 frames, run {k + 1} (exit code {r.returncode}) {r.stdout.strip()[-200:]}")
        if not ok:
            continue
        with open(ppm, "rb") as f:
            check(f.read(2) == b"P6", "--frame: PPM image")
        h = open(hashes).read().splitlines()
        check(len(h) == 3 and h[2].startswith("3 "), f"--hashes: 3 lines ({h[-1:]})")
        sums.append(h)
    check(len(sums) == 2 and sums[0] == sums[1], "--frame: both runs equal")

    for exe in (trace_exe, bios_check, cd_info):
        r = run([exe, "--help"])
        check(r.returncode == 0 and "usage:" in r.stdout, f"--help: {os.path.basename(exe)}")
    for rel in SCRIPTS:
        r = run([sys.executable, os.path.join(TOOLS, rel), "--help"])
        check(r.returncode == 0 and len(r.stdout.strip()) > 40, f"--help: {rel} (exit code {r.returncode})")

    print("\nRESULT: " + ("OK" if not fails else f"FAILED ({len(fails)})"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
