"""Tests of the manual per-game config (tools/recomp/pcerecomp/config.py)."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from pcerecomp.analysis import Analysis  # noqa: E402
from pcerecomp.config import Config, parse_point  # noqa: E402
from pcerecomp.coverage import Point  # noqa: E402
from test_analysis import coverage, make_rom  # noqa: E402


class TestParse(unittest.TestCase):
    def test_point(self):
        self.assertEqual(parse_point("7:00:0B0C"), Point(7, 0, 0x0B0C))
        with self.assertRaises(ValueError):
            parse_point("7:00")
        with self.assertRaises(ValueError):
            parse_point("8:00:0000")

    def test_full(self):
        cfg = Config({
            "rom_crc32": "5F6F3C2A",
            "target": [{"source": "7:00:0B0C", "bank": "01"}],
            "function": [{"entry": "5:01:0BCF"}],
            "data": [{"bank": "09", "start": "1F14", "end": "1F33"}],
            "table": [{"source": "4:09:1F11", "entries": 8}],
        })
        self.assertEqual(cfg.rom_crc32, 0x5F6F3C2A)
        self.assertEqual(cfg.destinations, {Point(7, 0, 0xB0C): 1})
        self.assertEqual(cfg.functions, {Point(5, 1, 0xBCF)})
        self.assertTrue(cfg.is_data(9, 0x1F20))
        self.assertFalse(cfg.is_data(9, 0x1F34))
        self.assertEqual(cfg.tables, {Point(4, 9, 0x1F11): 8})


class TestApply(unittest.TestCase):
    def test_destination_resolves_bank(self):
        # JSR $4000 with unknown bank; the config says bank 3
        rom = make_rom({(0, 0): "20 00 40 80 FE", (3, 0): "60"})
        cfg = Config({"target": [{"source": "7:00:0000", "bank": "03"}]})
        an = Analysis(rom, coverage(rom), config=cfg)
        self.assertEqual(an.unresolved, [])
        self.assertIn(Point(2, 3, 0), an.code)
        self.assertEqual(cfg.unused(), [])

    def test_extra_function(self):
        # Code never called at $E010, added as a function by the config
        rom = make_rom({(0, 0): "80 FE", (0, 0x10): "EA 60"})
        cfg = Config({"function": [{"entry": "7:00:0010"}]})
        an = Analysis(rom, coverage(rom), config=cfg)
        self.assertIn(Point(7, 0, 0x11), an.code)
        self.assertIn("sub", an.labels[Point(7, 0, 0x10)])

    def test_data_region_blocks_deduced_code(self):
        # BNE jumps to $E005; the config says $E005-$E00F are data
        rom = make_rom({(0, 0): "D0 03 80 FE 00 EA EA 60"})
        cfg = Config({"data": [{"bank": "00", "start": "0004", "end": "000F"}]})
        an = Analysis(rom, coverage(rom), config=cfg)
        self.assertNotIn(Point(7, 0, 5), an.code)
        self.assertIn(Point(7, 0, 2), an.code)

    def test_data_region_executed_is_reported(self):
        rom = make_rom({(0, 0): "EA 80 FE"})
        cfg = Config({"data": [{"bank": "00", "start": "0000", "end": "0000"}]})
        an = Analysis(rom, coverage(rom, "X 7 00 0000\n"), config=cfg)
        self.assertEqual(an.stats()["config_data_executed"], [Point(7, 0, 0)])

    def test_fixed_table_size(self):
        # JMP ($E010,X): the automatic table would stop at the 1st entry (it points to the table itself);
        # the config fixes 2 entries and the second one ($E020) is used.
        rom = make_rom({(0, 0): "7C 10 E0", (0, 0x10): "11 E0 20 E0", (0, 0x20): "80 FE"})
        auto = Analysis(rom, coverage(rom))
        self.assertEqual(auto.tables[Point(7, 0, 0)], [])
        cfg = Config({"table": [{"source": "7:00:0000", "entries": 2}]})
        an = Analysis(rom, coverage(rom), config=cfg)
        self.assertIn(Point(7, 0, 0x20), an.tables[Point(7, 0, 0)])
        self.assertEqual(cfg.unused(), [])

    def test_unused_rule_reported(self):
        rom = make_rom({(0, 0): "80 FE"})
        cfg = Config({"target": [{"source": "7:00:0123", "bank": "01"}]})
        Analysis(rom, coverage(rom), config=cfg)
        self.assertEqual(cfg.unused(), ["target 7:00:0123"])

    def test_other_rom_rejected(self):
        rom = make_rom({(0, 0): "80 FE"})
        with self.assertRaises(ValueError):
            Analysis(rom, coverage(rom), config=Config({"rom_crc32": "12345678"}))


if __name__ == "__main__":
    unittest.main()
