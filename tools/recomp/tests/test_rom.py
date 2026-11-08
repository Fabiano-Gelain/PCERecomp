"""Tests of the ROM reader (tools/recomp/pcerecomp/rom.py)."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from pcerecomp.rom import BANK_SIZE, Rom  # noqa: E402


def numbered_rom(banks):
    """Synthetic ROM in which the first byte of each bank is the bank number."""
    data = bytearray(banks * BANK_SIZE)
    for b in range(banks):
        data[b * BANK_SIZE] = b
    return bytes(data)


class TestMirroring(unittest.TestCase):
    def test_256k(self):
        rom = Rom(numbered_rom(32))
        for b in range(0x80):
            self.assertEqual(rom.read(b, 0), b % 32, f"bank {b:02X}")

    def test_384k_layout(self):
        rom = Rom(numbered_rom(48))
        self.assertEqual(rom.read(0x1F, 0), 0x1F)
        self.assertEqual(rom.read(0x20, 0), 0x00)
        self.assertEqual(rom.read(0x40, 0), 0x20)
        self.assertEqual(rom.read(0x4F, 0), 0x2F)
        self.assertEqual(rom.read(0x50, 0), 0x20)

    def test_copier_header_removed(self):
        rom = Rom(b"\x00" * 512 + numbered_rom(32))
        self.assertEqual(rom.size, 32 * BANK_SIZE)
        self.assertEqual(rom.read(5, 0), 5)

    def test_non_rom_bank_rejected(self):
        rom = Rom(numbered_rom(32))
        with self.assertRaises(ValueError):
            rom.read(0xF8, 0)

    def test_logical_read_uses_mpr(self):
        rom = Rom(numbered_rom(32))
        mpr = [0, 0, 7, 0, 0, 0, 0, 3]
        self.assertEqual(rom.read_logical(mpr, 0x4000), 7)
        self.assertEqual(rom.read_logical(mpr, 0xE000), 3)


if __name__ == "__main__":
    unittest.main()
