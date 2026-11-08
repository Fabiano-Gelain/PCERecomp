"""Test of the CD image reader (runtime/cd/cdimage.c): our cd_info and gg_cdinfo (Geargrafx)
read the same CUE/BIN images and must print the same table of contents (tracks, LBAs, pregaps, position
in the file) and the same CRC of each track's contents.

Images tested:
  - synthetic, generated here (pseudorandom contents): one track per file and several tracks in the
    same file, INDEX 00 (stored) and PREGAP (not stored) pregaps, MODE1/2048, incomplete last
    sector, lowercase keywords, invalid CUE;
  - optionally a real image (--cue), which in addition must not have unreadable sectors and must have
    the PC-Engine disc signature in sector 1 of the data track (read by our reader).

Usage: python run_cd_test.py <gg_cdinfo.exe> <cd_info.exe> <work dir> [--cue <image.cue>]
Exit code: 0 = equal, 1 = different, 77 = gg_cdinfo missing (test skipped).
A missing real image does not skip the test: only that image is not checked.
"""
import argparse
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "trace"))
import gg_cache  # noqa: E402

RAW = 2352


def blob(rng, size):
    return bytes(rng.getrandbits(8) for _ in range(size))


def write_image(folder, name, cue_text, files):
    """files: {bin name: size in bytes}. Returns the path of the .cue."""
    d = os.path.join(folder, name)
    os.makedirs(d, exist_ok=True)
    rng = random.Random(name)
    for fname, size in files.items():
        with open(os.path.join(d, fname), "wb") as f:
            f.write(blob(rng, size))
    cue = os.path.join(d, name + ".cue")
    with open(cue, "w", newline="\r\n") as f:
        f.write(cue_text)
    return cue


def synthetic_images(work):
    cues = []
    # Similar to a PC-Engine disc: audio, data with INDEX 00, audio with INDEX 00, audio with PREGAP,
    # a MODE1/2048 file and an audio track with an incomplete last sector.
    cues.append(write_image(work, "one_per_file", """REM test
CATALOG 0000000000000
FILE "a (Track 01).bin" BINARY
  TRACK 01 AUDIO
    INDEX 01 00:00:00
FILE "a (Track 02).bin" BINARY
  TRACK 02 MODE1/2352
    INDEX 00 00:00:00
    INDEX 01 00:00:20
FILE "a (Track 03).bin" BINARY
  TRACK 03 AUDIO
    INDEX 00 00:00:00
    INDEX 01 00:00:07
FILE "a (Track 04).bin" BINARY
  TRACK 04 AUDIO
    PREGAP 00:02:00
    INDEX 01 00:00:00
FILE "a (Track 05).bin" BINARY
  TRACK 05 MODE1/2048
    INDEX 01 00:00:00
FILE "a (Track 06).bin" BINARY
  TRACK 06 AUDIO
    INDEX 01 00:00:00
""", {"a (Track 01).bin": 40 * RAW, "a (Track 02).bin": 60 * RAW, "a (Track 03).bin": 30 * RAW,
      "a (Track 04).bin": 25 * RAW, "a (Track 05).bin": 33 * 2048, "a (Track 06).bin": 12 * RAW + 1000}))
    # Several tracks in the same file (INDEX 00 and PREGAP in the middle of the file), lowercase, unquoted name.
    cues.append(write_image(work, "single_file", """file disc.bin binary
  track 01 mode1/2352
    index 01 00:00:00
  track 02 audio
    pregap 00:00:10
    index 01 00:01:00
  track 03 audio
    index 00 00:01:40
    index 01 00:01:50
  track 04 mode1/2352
    index 00 00:02:10
    index 01 00:02:12
""", {"disc.bin": 300 * RAW}))
    # Invalid: MODE2 track and a file that does not exist.
    cues.append(write_image(work, "mode2", 'FILE "x.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n',
                            {"x.bin": 10 * RAW}))
    cues.append(write_image(work, "no_bin", 'FILE "missing.bin" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n',
                            {}))
    return cues


def run_pair(gg, ours, cue, work):
    # Geargrafx output per image, reused while the .cue and the tracks do not change (gg_cache.py).
    ref_path = os.path.join(work, "ref_" + os.path.splitext(os.path.basename(cue))[0] + ".txt")
    gg_run = gg_cache.start([gg, cue, ref_path], [ref_path])
    our = subprocess.run([ours, cue], capture_output=True, text=True).stdout.splitlines()
    gg_run.wait()
    ref = []
    if os.path.exists(ref_path):
        with open(ref_path) as f:
            ref = f.read().splitlines()
    # On error, all that matters is that both reject the image (the messages are different).
    if ref and ref[0] == "error":
        return ref, ["error"] if our and our[0].startswith("error") else our
    return ref, our


def has_signature(ours, cue, work):
    """The System Card only accepts the disc if sector 1 of the data track has this signature (offset 32)."""
    path = os.path.join(work, "sector1.bin")
    if subprocess.run([ours, cue, "--sector", "1", path]).returncode != 0:
        return False
    with open(path, "rb") as f:
        data = f.read()
    return data[32:56] == b"PC Engine CD-ROM SYSTEM\x00"


def python_layout_diff(cue, our_lines):
    """Compares the track positions of the Python reader (used by the recompiler) with those of cd_info.
    Returns the first difference (text) or None."""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "recomp"))
    from pcerecomp.cdimage import CdImage
    img = CdImage(cue)
    tracks = [line for line in our_lines if line.startswith("track ")]
    if len(tracks) != len(img.tracks):
        return f"{len(img.tracks)} tracks, cd_info {len(tracks)}"
    for line, t in zip(tracks, img.tracks):
        fields = dict(f.split("=", 1) for f in line.split()[3:] if "=" in f)
        lead = "-" if t.lead_in_lba is None else str(t.lead_in_lba)
        mine = {"start": str(t.start_lba), "end": str(t.end_lba), "pause": lead, "byte": str(t.file_offset)}
        for key, value in mine.items():
            if fields.get(key) != value:
                return f"track {t.number}: {key}={value}, cd_info {key}={fields.get(key)}"
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("gg")
    ap.add_argument("ours")
    ap.add_argument("work")
    ap.add_argument("--cue")
    a = ap.parse_args()
    a.gg, a.ours = os.path.abspath(a.gg), os.path.abspath(a.ours)

    if not os.path.exists(a.gg):
        print(f"gg_cdinfo not built ({a.gg}): run tools\\reference_trace\\build.ps1. Test skipped")
        return 77
    os.makedirs(a.work, exist_ok=True)

    cues = synthetic_images(a.work)
    if a.cue and os.path.exists(a.cue):
        cues.append(a.cue)
    elif a.cue:
        print(f"real image not found ({a.cue}): checking only the synthetic ones")

    fails = 0
    for cue in cues:
        ref, our = run_pair(a.gg, a.ours, cue, a.work)
        name = os.path.basename(cue)
        if ref != our:
            fails += 1
            print(f"DIFFERENT: {name}")
            for i in range(max(len(ref), len(our))):
                r = ref[i] if i < len(ref) else "(nothing)"
                o = our[i] if i < len(our) else "(nothing)"
                if r != o:
                    print(f"  line {i + 1}\n    Geargrafx: {r}\n    ours:      {o}")
            continue
        if cue == a.cue and any("failed=" in line for line in our):
            fails += 1
            print(f"UNREADABLE SECTORS in the real image: {name}")
            continue
        if cue == a.cue and not has_signature(a.ours, cue, a.work):
            fails += 1
            print(f"NO PC-Engine disc SIGNATURE in sector 1 of the data track: {name}")
            continue
        if ref[0] != "error":
            diff = python_layout_diff(cue, our)
            if diff:
                fails += 1
                print(f"PYTHON READER (tools/recomp/pcerecomp/cdimage.py) DIFFERENT in {name}: {diff}")
                continue
        print(f"equal: {name} ({ref[0]})")
        if cue == a.cue:
            print("\n".join("  " + line for line in our))

    print("PASSED" if fails == 0 else f"FAILED: {fails} image(s) different")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
