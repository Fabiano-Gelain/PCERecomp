"""Tests of the recursive analysis (tools/recomp/pcerecomp/analysis.py) and the listing."""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from pcerecomp.analysis import Analysis  # noqa: E402
from pcerecomp.config import Config  # noqa: E402
from pcerecomp.coverage import Coverage, Point  # noqa: E402
from pcerecomp.functions import find_functions, function_stats  # noqa: E402
from pcerecomp.listing import write_listing  # noqa: E402
from pcerecomp.rom import BANK_SIZE, Rom  # noqa: E402


def make_rom(code):
    """code: {(bank, offset): 'hex bytes'}; reset vector = $E000 (bank 0)."""
    data = bytearray(32 * BANK_SIZE)
    for (bank, off), hexbytes in code.items():
        b = bytes.fromhex(hexbytes)
        data[bank * BANK_SIZE + off:bank * BANK_SIZE + off + len(b)] = b
    for vec in (0x1FF6, 0x1FF8, 0x1FFA, 0x1FFE):  # all vectors at $E000
        data[vec:vec + 2] = bytes([0x00, 0xE0])
    return Rom(bytes(data))


def coverage(rom, body=""):
    f = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="ascii")
    f.write(f"# rom-crc32 {rom.crc32:08X}\n{body}")
    f.close()
    try:
        return Coverage.load([f.name])
    finally:
        os.unlink(f.name)


class TestAnalysis(unittest.TestCase):
    def test_bank_tracking_lda_tam(self):
        # LDA #$03 / TAM #$04 (page 2 = bank 3) / JSR $4000 / BRA *
        rom = make_rom({(0, 0): "A9 03 53 04 20 00 40 80 FE", (3, 0): "60"})
        an = Analysis(rom, coverage(rom))
        self.assertIn(Point(2, 3, 0), an.code)
        self.assertEqual(an.code[Point(2, 3, 0)].ins.text, "RTS")
        self.assertIn("sub", an.labels[Point(2, 3, 0)])
        self.assertEqual(an.unresolved, [])

    def test_unknown_bank_reported(self):
        # JSR $4000 without knowing the bank of page 2
        rom = make_rom({(0, 0): "20 00 40 80 FE"})
        an = Analysis(rom, coverage(rom))
        self.assertEqual(len(an.unresolved), 1)
        self.assertEqual(an.unresolved[0].target, 0x4000)

    def test_data_after_jump_is_not_code(self):
        # JMP $E010 followed by data (text) that would form a fake JSR
        rom = make_rom({(0, 0): "4C 10 E0 20 4E 45", (0, 0x10): "80 FE"})
        an = Analysis(rom, coverage(rom))
        self.assertIn(Point(7, 0, 0x10), an.code)
        self.assertNotIn(Point(7, 0, 3), an.code)

    def test_executed_wins_conflict(self):
        # The deduced path reads "A9 20 ..." at $E005 but the coverage confirms an instruction at $E006.
        rom = make_rom({(0, 0): "D0 03 4C 00 E0 A9 20 4C 00 E0"})
        cov = coverage(rom, "X 7 00 0006\n")
        an = Analysis(rom, cov)
        self.assertIn(Point(7, 0, 6), an.code)
        self.assertTrue(an.code[Point(7, 0, 6)].executed)
        # The executed one at $E006 reads 3 bytes ($E006-$E008, "20 4C 00" = JSR $004C) and drops the two
        # deduced ones that overlap it: LDA #$20 at $E005 and JMP $E000 at $E007.
        self.assertNotIn(Point(7, 0, 5), an.code)
        self.assertNotIn(Point(7, 0, 7), an.code)
        self.assertEqual(len(an.conflicts), 2)

    def test_call_without_return_not_followed(self):
        # Executed JSR whose next instruction never executed (the routine reads data after the JSR)
        rom = make_rom({(0, 0): "20 10 E0 41 42 43", (0, 0x10): "60"})
        cov = coverage(rom, "X 7 00 0000\nX 7 00 0010\nE C 7 00 0000 7 00 0010\n")
        an = Analysis(rom, cov)
        self.assertNotIn(Point(7, 0, 3), an.code)

    def test_indirect_jump_from_coverage(self):
        # JMP ($E020,X) whose targets only the coverage knows
        rom = make_rom({(0, 0): "7C 20 E0", (0, 0x30): "80 FE", (0, 0x40): "80 FE"})
        cov = coverage(rom, "X 7 00 0000\nX 7 00 0030\nX 7 00 0040\n"
                            "E N 7 00 0000 7 00 0030\nE N 7 00 0000 7 00 0040\n")
        an = Analysis(rom, cov)
        self.assertEqual(an.targets[Point(7, 0, 0)], {Point(7, 0, 0x30), Point(7, 0, 0x40)})


class TestTablesAndContext(unittest.TestCase):
    def test_jump_table_after_instruction(self):
        # ASL A / TAX / JMP ($E010,X); table at $E010: [$0000 (no bank: skipped), $E020, $E030]
        # and the table ends at the first target ($E020), where the code starts.
        rom = make_rom({(0, 0): "0A AA 7C 10 E0",
                        (0, 0x10): "00 00 20 E0 30 E0",
                        (0, 0x20): "80 FE", (0, 0x30): "80 FE"})
        an = Analysis(rom, coverage(rom))
        self.assertEqual(an.tables[Point(7, 0, 2)], [Point(7, 0, 0x20), Point(7, 0, 0x30)])
        self.assertIn(Point(7, 0, 0x30), an.code)
        self.assertEqual(an.unresolved, [])

    def test_table_must_contain_coverage_targets(self):
        # The coverage saw the jump go to $E040, which is not in the table read: mismatch.
        rom = make_rom({(0, 0): "7C 10 E0", (0, 0x10): "20 E0", (0, 0x20): "80 FE", (0, 0x40): "80 FE"})
        cov = coverage(rom, "X 7 00 0000\nX 7 00 0040\nE N 7 00 0000 7 00 0040\n")
        an = Analysis(rom, cov)
        self.assertEqual(len(an.table_mismatches()), 1)

    def test_bank_learned_from_context(self):
        # Bank 3 (page 2): an executed JSR $8000 went to bank 5. Another JSR from the same bank to
        # page 4, never executed, uses the same bank.
        rom = make_rom({(3, 0): "20 00 80 80 FE", (3, 0x20): "20 10 80 80 FE",
                        (5, 0): "60", (5, 0x10): "60"})
        cov = coverage(rom, "X 2 03 0000\nX 4 05 0000\nX 2 03 0020\nE C 2 03 0000 4 05 0000\n")
        an = Analysis(rom, cov, vectors=False)
        self.assertIn(Point(4, 5, 0x10), an.targets[Point(2, 3, 0x20)])
        self.assertEqual(an.unresolved, [])


class TestFunctions(unittest.TestCase):
    def test_bodies_and_tail_call(self):
        # $E000: JSR $E010 / JSR $E020 / BRA *
        # $E010: BEQ $E014 / NOP / NOP / JMP $E020   (jump to another function = tail call)
        # $E020: RTS
        rom = make_rom({(0, 0): "20 10 E0 20 20 E0 80 FE",
                        (0, 0x10): "F0 02 EA EA 4C 20 E0", (0, 0x20): "60"})
        an = Analysis(rom, coverage(rom))
        fs = find_functions(an)
        self.assertEqual(set(fs), {Point(7, 0, 0), Point(7, 0, 0x10), Point(7, 0, 0x20)})
        f = fs[Point(7, 0, 0x10)]
        self.assertEqual(f.body, {Point(7, 0, o) for o in (0x10, 0x12, 0x13, 0x14)})
        self.assertEqual(f.tail_calls, {Point(7, 0, 0x20)})
        self.assertEqual(fs[Point(7, 0, 0)].body, {Point(7, 0, o) for o in (0, 3, 6)})
        self.assertEqual(function_stats(an, fs)["orphan_instructions"], [])


if __name__ == "__main__":
    unittest.main()
