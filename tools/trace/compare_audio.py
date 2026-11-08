"""Compares two raw sound files (signed 16-bit, interleaved stereo, 44100 Hz)
value by value and, optionally, saves WAV copies to listen to.

Usage: python tools/trace/compare_audio.py <reference.raw> <ours.raw> [--wav dir]

Exit code 0 = identical, 1 = different. Uses only the Python standard library.
"""
import argparse
import os
import struct
import sys
import wave


def write_wav(path, data):
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(44100)
        w.writeframes(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference")
    ap.add_argument("ours")
    ap.add_argument("--wav", help="directory where reference.wav and ours.wav are saved")
    a = ap.parse_args()

    ref = open(a.reference, "rb").read()
    our = open(a.ours, "rb").read()
    if a.wav:
        os.makedirs(a.wav, exist_ok=True)
        write_wav(os.path.join(a.wav, "reference.wav"), ref)
        write_wav(os.path.join(a.wav, "ours.wav"), our)

    nref, nour = len(ref) // 2, len(our) // 2
    rv = struct.unpack(f"<{nref}h", ref[:nref * 2])
    ov = struct.unpack(f"<{nour}h", our[:nour * 2])
    nonzero = sum(1 for v in rv if v)
    first = next((i for i in range(min(nref, nour)) if rv[i] != ov[i]), None)
    seconds = nref / 2 / 44100

    if first is None and nref == nour:
        print(f"EQUAL: {nref // 2} stereo samples ({seconds:.1f} s), {nonzero} nonzero values")
        return 0
    if first is None:
        print(f"DIFFERENT: sizes {nref} (reference) and {nour} (ours)")
    else:
        print(f"DIFFERENT: first differing value at index {first} "
              f"(sample {first // 2}, {first // 2 / 44100:.3f} s): reference {rv[first]}, ours {ov[first]}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
