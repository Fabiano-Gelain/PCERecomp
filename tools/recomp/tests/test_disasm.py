"""Tests of the disassembler (tools/recomp/pcerecomp/disasm.py)."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from pcerecomp.disasm import decode  # noqa: E402
from pcerecomp.rom import Rom  # noqa: E402


def disasm_bytes(data, pc=0x8000):
    mem = dict(enumerate(data, pc))
    return decode(lambda a: mem.get(a, 0), pc)


class TestModes(unittest.TestCase):
    """One instruction of each addressing mode."""
    CASES = [
        ("78", "SEI", None),
        ("0A", "ASL A", None),
        ("00 55", "BRK", None),
        ("A9 12", "LDA #$12", None),
        ("A5 12", "LDA $12", None),
        ("B5 12", "LDA $12,X", None),
        ("B6 12", "LDX $12,Y", None),
        ("AD 34 12", "LDA $1234", None),
        ("BD 34 12", "LDA $1234,X", None),
        ("B9 34 12", "LDA $1234,Y", None),
        ("A1 12", "LDA ($12,X)", None),
        ("B1 12", "LDA ($12),Y", None),
        ("B2 12", "LDA ($12)", None),
        ("6C 34 12", "JMP ($1234)", 0x1234),
        ("7C 34 12", "JMP ($1234,X)", 0x1234),
        ("4C 34 12", "JMP $1234", 0x1234),
        ("20 34 12", "JSR $1234", 0x1234),
        ("D0 FE", "BNE $8000", 0x8000),       # branch to itself
        ("80 10", "BRA $8012", 0x8012),
        ("44 80", "BSR $7F82", 0x7F82),       # negative offset
        ("0F 12 05", "BBR0 $12,$8008", 0x8008),
        ("FF 34 FB", "BBS7 $34,$7FFE", 0x7FFE),
        ("83 0F 12", "TST #$0F,$12", None),
        ("A3 0F 12", "TST #$0F,$12,X", None),
        ("93 0F 34 12", "TST #$0F,$1234", None),
        ("B3 0F 34 12", "TST #$0F,$1234,X", None),
        ("73 00 20 01 20 FF 1F", "TII $2000,$2001,$1FFF", None),
        ("53 04", "TAM #$04", None),
        ("03 05", "ST0 #$05", None),
        ("0B", "???", None),
    ]

    def test_modes(self):
        for hexbytes, text, target in self.CASES:
            data = bytes.fromhex(hexbytes)
            ins = disasm_bytes(data)
            self.assertEqual(ins.text, text, hexbytes)
            self.assertEqual(ins.opcode.size, len(data), hexbytes)
            self.assertEqual(ins.target, target, hexbytes)


if __name__ == "__main__":
    unittest.main()
