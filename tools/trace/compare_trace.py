"""Compares two traces line by line and shows the first divergence.

Usage: python tools/trace/compare_trace.py <reference> <ours> [--context N]

Exit code 0 = identical (up to the end of the shorter one), 1 = diverged.
"""
import argparse
import sys


def fields(line):
    parts = line.split()
    out = {"PC": parts[0]}
    for p in parts[1:]:
        k, v = p.split(":", 1)
        out[k] = v
    return out


def same_bytes(a, b):
    """Number of lines if the two files are identical, otherwise None."""
    chunk = 8 << 20
    lines = 0
    with open(a, "rb") as fa, open(b, "rb") as fb:
        while True:
            x, y = fa.read(chunk), fb.read(chunk)
            if x != y:
                return None
            if not x:
                return lines
            lines += x.count(b"\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference")
    ap.add_argument("ours")
    ap.add_argument("--context", type=int, default=8)
    args = ap.parse_args()

    # Fast path (traces of hundreds of MB): byte-identical files. Only on a difference (or if one ends
    # early) the line-by-line comparison below finds and describes the first differing instruction.
    lines = same_bytes(args.reference, args.ours)
    if lines is not None:
        print(f"EQUAL: {lines} instructions")
        return 0

    history = []
    with open(args.reference) as fr, open(args.ours) as fo:
        n = 0
        for ref, our in zip(fr, fo):
            ref, our = ref.rstrip("\n"), our.rstrip("\n")
            if ref != our:
                print(f"DIVERGENCE at instruction {n} (line {n + 1})")
                print("  last equal instructions:")
                for h in history:
                    print("    " + h)
                print("  reference: " + ref)
                print("  ours:      " + our)
                fr_, fo_ = fields(ref), fields(our)
                diff = [k for k in fr_ if fr_.get(k) != fo_.get(k)]
                print("  differing fields: " + ", ".join(f"{k} (ref {fr_[k]}, ours {fo_.get(k)})" for k in diff))
                return 1
            history.append(ref)
            if len(history) > args.context:
                history.pop(0)
            n += 1
    print(f"EQUAL: {n} instructions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
