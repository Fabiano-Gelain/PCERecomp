"""Tests of the disc map (tools/recomp/pcerecomp/discmap.py) and of the analysis without coverage: a
synthetic "disc" with one code file placed in the CD RAM."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from pcerecomp.analysis import Analysis  # noqa: E402
from pcerecomp.codegen import CodeGen  # noqa: E402
from pcerecomp.coverage import Coverage, Point  # noqa: E402
from pcerecomp.discmap import DiscMap  # noqa: E402
from pcerecomp.functions import find_functions  # noqa: E402
from pcerecomp.program import CdProgram  # noqa: E402
from test_analysis import make_rom  # noqa: E402

SECTOR = 2048


class FakeDisc:
    def __init__(self, data):
        self.data = data

    def read(self, pos):
        return self.data[pos]

    def read_bytes(self, pos, n):
        return bytes(self.data[pos:pos + n])


# File 1 (sector 8), placed at $0000 of the CD RAM (bank $80), which runs in window 2 ($4000)
CODE = {
    0x0000: "4C 10 40",             # $4000 JMP $4010      (map entry: jump table)
    0x0003: "4C 20 40",             # $4003 JMP $4020
    0x0010: "A9 40 A2 30 20 50 40",  # $4010 LDA #$40 / LDX #$30 / JSR $4050: address $4030 in the registers
    0x0017: "A9 40 A0 60 20 50 40",  # $4017 LDA #$40 / LDY #$60 / JSR $4050: $4060 is garbage (BRK)
    0x001E: "60",                   # $401E RTS
    # $4020 STA $4031 / STA $4041 / JSR $4040 / RTS: rewrites the operand of the instruction at $4030 and the
    # target of the JSR at $4040 (like a drawing routine that changes the target)
    0x0020: "8D 31 40 8D 41 40 20 40 40 60",
    0x0030: "A9 01 60",             # $4030 LDA #$01 / RTS
    0x0040: "20 50 40 60",          # $4040 JSR $4050 (rewritten target) / RTS
    0x0050: "60",                   # $4050 RTS
    0x0060: "00 00 60",             # $4060 BRK...
}


def make_map():
    data = bytearray(16 * SECTOR)
    data[0:4] = bytes([0, 1, 0, 0])  # size table (sectors)
    for off, hexbytes in CODE.items():
        b = bytes.fromhex(hexbytes)
        data[8 * SECTOR + off:8 * SECTOR + off + len(b)] = b
    cfg = {
        "base_sector": 0,
        "table": {"sector": 0, "offset": 0, "entries": 4, "sectors_per_file": 4, "first": 1},
        "family": [{"name": "prog", "files": [1], "ram": 0, "banks": {"80": 2}, "entries": ["2:80:0000"]}],
    }
    disc = FakeDisc(data)
    dm = DiscMap(cfg, disc)
    prog = CdProgram(make_rom({}), disc, discmap=dm)
    an = Analysis(prog, Coverage(), vectors=False, seeds=dm.seeds(prog))
    return dm, prog, an


class TestDiscMap(unittest.TestCase):
    def setUp(self):
        self.dm, self.prog, self.an = make_map()
        self.vb = self.prog.vbank[(8 * SECTOR - 0, 0x80)]

    def pt(self, addr):
        return Point(2, self.vb, addr & 0x1FFF)

    def test_loads_from_table(self):
        self.assertEqual(len(self.dm.loads), 1)
        ld = self.dm.loads[0]
        self.assertEqual((ld.number, ld.pos, ld.length, ld.ram), (1, 8 * SECTOR, SECTOR, 0))

    def test_jump_table_and_immediate_pointer(self):
        code = self.an.code
        self.assertIn(self.pt(0x4003), code)  # 2nd JMP of the entry's jump table
        self.assertIn(self.pt(0x4020), code)
        self.assertIn(self.pt(0x4030), code)  # LDA #hi / LDX #lo / JSR
        self.assertIn(self.pt(0x4030), self.an.weak)

    def test_garbage_rejected(self):
        self.assertNotIn(self.pt(0x4060), self.an.code)  # BRK: guess rejected
        self.assertIn(self.pt(0x4060), self.an.rejected)

    def test_self_modified(self):
        self.assertIn((2, 0x80, 0x0030), self.an.modified)
        self.assertIn((2, 0x80, 0x0040), self.an.modified)

    def test_rewritten_call_target(self):
        """JSR whose target the game rewrites: the target is read from RAM at run time and is left out of the
        signature (otherwise the signature would never match and the whole function would fall back to the
        interpreter)."""
        funcs = find_functions(self.an)
        gen = CodeGen(self.an, funcs, "test")
        p = self.pt(0x4040)
        owner = next(e for e, f in funcs.items() if p in f.body)
        c = gen.function_c(owner)
        jsr = c[c.index("hu_op_20(c, recomp_ram_byte"):]
        self.assertTrue(jsr.startswith("hu_op_20(c, recomp_ram_byte(c->ctx, 0x100041), "
                                       "recomp_ram_byte(c->ctx, 0x100042)"))
        self.assertIn("return;", jsr.split("\n")[1])  # call: the dispatcher continues from the target read
        sig = gen.signature_c(owner)
        self.assertNotIn("0x20, 0x50, 0x40", sig)    # the JSR operand is not part of the signature

    def test_partition_one_owner(self):
        funcs = find_functions(self.an)
        owners = {}
        for e, f in funcs.items():
            for p in f.body:
                owners.setdefault(p, []).append(e)
        self.assertEqual(set(owners), set(self.an.code))
        self.assertTrue(all(len(v) == 1 for v in owners.values()))


class TestRoutineRecords(unittest.TestCase):
    """`routine_records`: game tables with [address (2 bytes), priority (FF = end), bank - $80]."""

    def test_records(self):
        data = bytearray(16 * SECTOR)
        data[0:4] = bytes([0, 1, 0, 0])
        recs = bytes([0x30, 0x40, 0x05, 0x00,   # $4030, priority 5, bank $80: valid
                      0x50, 0x40, 0xFF, 0x00,   # $4050, FF = end: discarded
                      0x70, 0x40, 0x05, 0x01])  # $4070 in bank $81 (not part of the load): discarded
        data[8 * SECTOR + 0x100:8 * SECTOR + 0x100 + len(recs)] = recs
        cfg = {
            "base_sector": 0,
            "table": {"sector": 0, "offset": 0, "entries": 4, "sectors_per_file": 4, "first": 1},
            "family": [{"name": "prog", "files": [1], "ram": 0, "banks": {"80": 2},
                        "routine_records": {"address": 0, "bank": 3, "bank_add": 0x80,
                                            "discard": {"2": 0xFF}}}],
        }
        dm = DiscMap(cfg, FakeDisc(data))
        self.assertEqual(dm._routine_records(dm.loads[0]), [Point(2, 0x80, 0x0030)])


if __name__ == "__main__":
    unittest.main()
