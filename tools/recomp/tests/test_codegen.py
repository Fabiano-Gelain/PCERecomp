"""Tests of the C generator (tools/recomp/pcerecomp/codegen.py) with synthetic ROMs.

The behavior of the generated C is checked end to end by the CTest recomp_* tests (traces
identical to the interpreter, images and sound identical to Geargrafx); here the shape of the code is checked.
"""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from pcerecomp.analysis import Analysis  # noqa: E402
from pcerecomp.codegen import PARTS, CodeGen  # noqa: E402
from pcerecomp.coverage import Point  # noqa: E402
from pcerecomp.functions import find_functions  # noqa: E402
from test_analysis import coverage, make_rom  # noqa: E402


def generate(code, body=""):
    rom = make_rom(code)
    an = Analysis(rom, coverage(rom, body))
    return CodeGen(an, find_functions(an), "test"), an


class TestCodeGen(unittest.TestCase):
    def test_instruction_and_branch(self):
        # $E000: LDX #$05 / DEX / BNE $E002 / JSR $E010 / BRA *     $E010: RTS
        gen, _ = generate({(0, 0): "A2 05 CA D0 FD 20 10 E0 80 FE", (0, 0x10): "60"})
        c = gen.function_c(Point(7, 0, 0))
        self.assertIn("hu_op_A2(c, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, t_, &x_)", c)
        self.assertIn("hu_pre(c, 0xA2, 0xE002)", c)
        self.assertIn("if (c->pc == 0xE002) { goto L_7_00_0002; }", c)   # branch inward: goto
        jsr = c[c.index("hu_op_20("):]
        self.assertIn("return;", jsr[:jsr.index("L_7_00_0008")])         # JSR: back to the dispatcher
        self.assertIn("case 0xE002: goto L_7_00_0002;", c)                 # entry in the middle of the function

    def test_tam_returns(self):
        gen, _ = generate({(0, 0): "A9 03 53 04 80 FE"})
        c = gen.function_c(Point(7, 0, 0))
        tam = c[c.index("hu_op_53("):]
        self.assertTrue(tam.split("\n")[1].strip().startswith("return;"))

    def test_cross_window_guard(self):
        # Instruction at the last byte of window 6 (bank 1) that continues into window 7 (bank 0).
        gen, _ = generate({(0, 0): "A9 01 53 40 4C FF DF", (1, 0x1FFF): "EA"})
        f = gen.functions[Point(7, 0, 0)]
        self.assertIn(Point(6, 1, 0x1FFF), f.body)
        c = gen.function_c(Point(7, 0, 0))
        # Going back to another window: checks the bank before continuing.
        self.assertIn("if (c->mpr[6] != 0x01) return; goto L_6_01_1FFF;", c)

    def test_files_and_table(self):
        gen, an = generate({(0, 0): "20 10 E0 80 FE", (0, 0x10): "60"})
        with tempfile.TemporaryDirectory() as d:
            files = gen.write(d)
            self.assertEqual(len(files), PARTS + 1)
            with open(os.path.join(d, "rc_table.c"), encoding="utf-8") as f:
                table = f.read()
            self.assertIn(f"recomp_rom_crc32 = 0x{an.rom.crc32:08X}u", table)
            self.assertIn("[0x0010] = f_7_00_0010,", table)
            self.assertIn("case 0x700: return t_7_00[offset];", table)


if __name__ == "__main__":
    unittest.main()
