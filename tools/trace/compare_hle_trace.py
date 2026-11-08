"""Finds the first BIOS routine in C (runtime/bios) whose timing differs from the real BIOS.

Compares two pce_trace traces made with --cycles (master clock on each line): one with the real BIOS and one
with --hle. In the --hle trace each C routine is a single line (pseudo-instruction); the other instructions are
the same as in the real BIOS. The script walks both, skipping in the real trace the instructions the C routine
replaced, and stops at the first matching instruction (PC and registers) that happens at a different cycle:
it shows the last pseudo-instruction before it (the suspect) and the difference.

Usage: python tools/trace/compare_hle_trace.py <real_bios_trace> <hle_trace> [--context N] [--no-p]
     --no-p: ignores the flags (P) when matching lines (the C routines do not always leave the BIOS flags
     when calling the internal parts done instruction by instruction, which recompute them before use)
Exit code: 0 = same cycles to the end, 1 = found a difference, 2 = lost sync (the traces diverged).
"""
import argparse
import sys


def parse(line, no_p=False):
    head, _, cyc = line.rpartition(" C:")
    if no_p:
        head = " ".join(w for w in head.split(" ") if not w.startswith("P:"))
    return head, int(cyc)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("real")
    ap.add_argument("hle")
    ap.add_argument("--context", type=int, default=6)
    ap.add_argument("--window", type=int, default=2000000,
                    help="how many lines of the real trace to search ahead for a line of the other")
    ap.add_argument("--no-p", action="store_true")
    a = ap.parse_args()

    real = [parse(l.rstrip("\n"), a.no_p) for l in open(a.real)]
    hle = [parse(l.rstrip("\n"), a.no_p) for l in open(a.hle)]
    # With --from-frame the two may start at different instructions: start at the 1st line of the real one equal
    # to the 1st line of the other (cycles count from there)
    i = 0
    while i < len(real) and real[i][0] != hle[0][0]:
        i += 1
    if i == len(real):
        print("the first line of the --hle trace does not appear in the real one")
        return 2
    real = real[i:]
    i = 0
    last_skip = None  # (line in the --hle trace, line in the real one) of the last replaced routine
    for j, (state, cyc) in enumerate(hle):
        if i >= len(real):
            break
        if real[i][0] != state:
            # The previous line of the --hle trace was a C routine: find where the real BIOS gets here
            k = i
            limit = min(len(real), i + a.window)
            while k < limit and real[k][0] != state:
                k += 1
            if k >= limit:
                # The C routines do not always leave A/X/Y like the BIOS at internal points: accept the real line
                # at the same cycle with the same PC, stack and windows
                want = cyc - hle[0][1] + real[0][1]
                key = [w for w in state.split(" ") if w[:2] in ("S:", "M:")] + [state[:4]]
                k = i
                while k < limit and real[k][1] <= want:
                    r = real[k][0]
                    if real[k][1] == want and [w for w in r.split(" ") if w[:2] in ("S:", "M:")] + [r[:4]] == key:
                        break
                    k += 1
                else:
                    k = limit
                if k >= limit:
                    print("lost sync at line %d of the --hle trace: %s" % (j + 1, state))
                    return 2
                print("warning: different registers at line %d (--hle): %s / real: %s" % (j + 1, state, real[k][0]))
                real[k] = (state, real[k][1])
            last_skip = (j - 1, i - 1)
            i = k
        if real[i][1] - real[0][1] != cyc - hle[0][1]:
            print("first instruction at a different cycle: line %d (--hle) / %d (real): %s" % (j + 1, i + 1, state))
            print("  difference (ours - BIOS): %d master cycles" % ((cyc - hle[0][1]) - (real[i][1] - real[0][1])))
            if last_skip:
                print("  last C routine before: %s" % hle[last_skip[0]][0][:4])
            print("  --hle before:")
            for t in range(max(0, j - a.context), j + 1):
                print("    %s C:%d" % (hle[t][0], hle[t][1] - hle[0][1]))
            print("  real before:")
            for t in range(max(0, i - a.context), i + 1):
                print("    %s C:%d" % (real[t][0], real[t][1] - real[0][1]))
            return 1
        i += 1
    print("same cycles in %d lines" % len(hle))
    return 0


if __name__ == "__main__":
    sys.exit(main())
