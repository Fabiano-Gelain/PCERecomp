# HuC6280 CPU implementation notes

Implementation: `runtime/cpu/huc6280.c`. Test: `tests/cpu/huc6280_tests.c` (CTest `cpu_huc6280`),
which runs the 640,000 tests in `reference/huc6280-tests/v1` (2,500 per opcode).

## What each test checks
Registers (A, X, Y, S, P, PC), the 8 MPRs, the final memory listed, the total cycle count and the
absence of writes outside the list. The "phantom" bus cycles (dummy reads, almost always of the next
code byte or of $1FFFFF) are **not** emulated or checked.

## Differences from the Mesen tests (decided)

| Case | Mesen | What we do | Why |
|---|---|---|---|
| TSB/TRB (04 0C 14 1C), Z flag | Z = (A AND memory) == 0 | Z = written result == 0 | Geargrafx (`huc6280_opcodes_inline.h`, outside test mode) and Mednafen (`X_ZN_BIT(x \| A, x)`) agree with each other. The test checks Z against the result and the rest of P against Mesen. |
| TMA with operand 0 | returns a value that is not in the test state | returns the last value that went through TAM/TMA (`mpr_buffer`) | Depends on hidden state; 13 tests skipped. Games do not use it. |
| Block transfer whose destination wraps around 64 KB and overwrites its own source (or the stack with A/X/Y, when the test's memory list was cut) | keeps reading the old source value | byte-by-byte sequential copy (like Mednafen) | Real memory does not "remember" the old value. If the result differs in this case, the test is counted as skipped (2,698 today). A mutation test (breaking the TAI alternation) confirmed that real errors are still reported. |

The test generator cuts the memory list at ~499 pairs. When the **initial** list was cut (long
transfers), only registers and cycles are checked.

## Timing and interrupts
- Same as Geargrafx: the instruction's base cycles go to the hardware at its END; VDC/VCE accesses
  (+1 cycle, +2 on the VDC during a block transfer), VRAM waits (3 master cycles each) and ST0-2 (2 cycles
  before the write) advance the hardware immediately; a PSG write first makes the hardware catch up with
  the cycles already counted. CSL/CSH: 12 or 3 master-clock cycles per CPU cycle.
- Interrupts are sampled after the opcode fetch and again at each hardware advance in the middle of the
  instruction; they are serviced at the end of the instruction (8 cycles). Priority: timer, IRQ1 (VDC), IRQ2.
- Block transfers run in one call, but the hardware advances on every byte (14 + 6/byte + 3).
  Interrupts are only serviced at the end.
