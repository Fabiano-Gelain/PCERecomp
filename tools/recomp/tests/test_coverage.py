"""Tests of coverage reading/checking (tools/recomp/pcerecomp/coverage.py)."""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from pcerecomp.coverage import Coverage, Point, check  # noqa: E402
from pcerecomp.rom import BANK_SIZE, Rom  # noqa: E402


def make_rom():
    """Bank 0 (seen at $E000 through page 7): JSR $E010 / RTS / ... / BRA *."""
    data = bytearray(32 * BANK_SIZE)
    code = {0x0000: "20 10 E0", 0x0003: "80 FE", 0x0010: "60"}
    for off, hexbytes in code.items():
        b = bytes.fromhex(hexbytes)
        data[off:off + len(b)] = b
    return Rom(bytes(data))


def write(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="ascii")
    f.write(text)
    f.close()
    return f.name


class TestCoverage(unittest.TestCase):
    def setUp(self):
        self.rom = make_rom()
        self.files = []

    def tearDown(self):
        for f in self.files:
            os.unlink(f)

    def load(self, body):
        text = f"# pcerecomp coverage v1\n# rom-crc32 {self.rom.crc32:08X}\n# executed-instructions 10\n" + body
        path = write(text)
        self.files.append(path)
        return Coverage.load([path])

    def test_parse_and_ok(self):
        cov = self.load("X 7 00 0000\nX 7 00 0003\nX 7 00 0010\n"
                        "E C 7 00 0000 7 00 0010\nE R 7 00 0010 7 00 0003\n")
        self.assertEqual(len(cov.exec), 3)
        self.assertIn(Point(7, 0, 0x10), cov.exec)
        self.assertEqual(len(cov.edges), 2)
        self.assertEqual(check(cov, self.rom), [])

    def test_wrong_call_target(self):
        cov = self.load("X 7 00 0000\nX 7 00 0012\nE C 7 00 0000 7 00 0012\n")
        problems = check(cov, self.rom)
        self.assertEqual(len(problems), 1)
        self.assertIn("went to $E012", problems[0])

    def test_wrong_kind(self):
        cov = self.load("X 7 00 0003\nX 7 00 0000\nE R 7 00 0003 7 00 0000\n")
        self.assertTrue(any("RTS" not in p and "BRA" in p for p in check(cov, self.rom)))

    def test_instruction_crossing_window(self):
        # JSR at $9FFF (last byte of bank 1 on page 4): the address is on page 5.
        data = bytearray(32 * BANK_SIZE)
        data[1 * BANK_SIZE + 0x1FFF] = 0x20          # JSR, bank 1, last byte
        data[3 * BANK_SIZE + 0x0000:3 * BANK_SIZE + 2] = bytes([0x3D, 0xA0])  # bank 3: $A03D
        data[2 * BANK_SIZE + 0x0000:2 * BANK_SIZE + 2] = bytes([0x11, 0x22])  # bank 2 (bank+1): another target
        self.rom = Rom(bytes(data))
        body = "X 4 01 1FFF\nX 5 03 003D\nE C 4 01 1FFF 5 03 003D\n"
        # Without an S record: assumes bank + 1 (= 2) and reports the difference.
        self.assertTrue(check(self.load(body), self.rom))
        # With the S record (bank 3 in the next window): it matches.
        cov = self.load(body + "E S 4 01 1FFF 5 03 0000\n")
        self.assertEqual(cov.next_bank(Point(4, 1, 0x1FFF)), 3)
        self.assertEqual(check(cov, self.rom), [])

    def test_other_rom_rejected(self):
        path = write("# rom-crc32 12345678\nX 7 00 0000\n")
        self.files.append(path)
        cov = Coverage.load([path])
        self.assertIn("CRC32", check(cov, self.rom)[0])

    def test_merge_sessions(self):
        a = self.load("X 7 00 0000\n")
        b = self.load("X 7 00 0003\n")
        merged = Coverage.load(self.files)
        self.assertEqual(len(a.exec) + len(b.exec), len(merged.exec))
        self.assertEqual(merged.instructions, 20)


if __name__ == "__main__":
    unittest.main()
