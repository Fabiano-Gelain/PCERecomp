"""Checks the opcode table (tools/recomp/pcerecomp/opcodes.py) against two independent sources:

1. Mesen tests (reference/huc6280-tests): for each opcode, how far the PC moved.
   - normal instructions and block transfers: the PC always moves exactly the size;
   - conditional branches: when the branch is not taken, the PC moves the size.
2. Geargrafx name table (reference/Geargrafx/src/huc6280_names.h, PCEAS syntax).
"""
import os
import re
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
MESEN_DIR = os.path.join(ROOT, "reference", "huc6280-tests", "v1")
GG_NAMES = os.path.join(ROOT, "reference", "Geargrafx", "src", "huc6280_names.h")

from pcerecomp.opcodes import (  # noqa: E402
    FLOW_BRANCH, FLOW_ILLEGAL, FLOW_NORMAL, OPCODES,
)

TESTS_PER_OPCODE = 400


def mesen_pc_deltas(opcode):
    """List of (final PC - initial PC) of the first Mesen tests for the opcode.
    Binary format described in reference/huc6280-tests/transcode_json.py."""
    with open(os.path.join(MESEN_DIR, f"{opcode:02x}.json.bin"), "rb") as f:
        buf = f.read()
    count = struct.unpack_from("<I", buf, 0)[0]
    ptr = 4
    deltas = []
    for _ in range(min(count, TESTS_PER_OPCODE)):
        full = struct.unpack_from("<i", buf, ptr)[0]
        p = ptr + 4 + 50 + 4  # size, name (50), opcode
        pcs = []
        for _state in range(2):  # initial, final
            size = struct.unpack_from("<i", buf, p)[0]
            pcs.append(struct.unpack_from("<H", buf, p + 4 + 5)[0])
            p += size
        deltas.append((pcs[1] - pcs[0]) & 0xFFFF)
        ptr += full
    return deltas


class TestTable(unittest.TestCase):
    def test_complete(self):
        self.assertEqual(len(OPCODES), 256)
        self.assertTrue(all(op.code == i for i, op in enumerate(OPCODES)))

    def test_illegal_count(self):
        self.assertEqual(sum(op.flow == FLOW_ILLEGAL for op in OPCODES), 22)


@unittest.skipUnless(os.path.isdir(MESEN_DIR), "reference/huc6280-tests not found")
class TestSizesAgainstMesen(unittest.TestCase):
    def test_sizes(self):
        problems = []
        for op in OPCODES:
            deltas = mesen_pc_deltas(op.code)
            if op.flow in (FLOW_NORMAL, FLOW_ILLEGAL) or op.mode == "block":
                if set(deltas) != {op.size}:
                    problems.append(f"{op.code:02X} {op.mnemonic}: size {op.size}, Mesen {sorted(set(deltas))}")
            elif op.flow == FLOW_BRANCH:
                if op.size not in deltas:
                    problems.append(f"{op.code:02X} {op.mnemonic}: branch not taken never moved {op.size}")
        self.assertEqual(problems, [])


@unittest.skipUnless(os.path.exists(GG_NAMES), "Geargrafx not found in reference/")
class TestNamesAgainstGeargrafx(unittest.TestCase):
    def test_names(self):
        with open(GG_NAMES, encoding="utf-8") as f:
            text = f.read()
        body = text[text.index("k_huc6280_opcode_names[256]"):]
        names = []
        for line in body.splitlines():
            line = line.strip()
            if line.startswith("GG_OPCODE_SYNTAX("):
                pceas = re.findall(r'"([^"]*)"', line)[1]  # 2nd syntax = PCEAS
            elif line.startswith("GG_OPCODE("):
                pceas = re.findall(r'"([^"]*)"', line)[0]
            else:
                continue
            names.append(pceas.replace("{n}", "").split()[0])
        self.assertEqual(len(names), 256)
        problems = []
        for op, gg in zip(OPCODES, names):
            expected = ".db" if op.flow == FLOW_ILLEGAL else op.mnemonic
            if gg != expected:
                problems.append(f"{op.code:02X}: ours {op.mnemonic}, Geargrafx {gg}")
        self.assertEqual(problems, [])


if __name__ == "__main__":
    unittest.main()
