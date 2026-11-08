"""Manual per-game config (games/<game>/config.toml).

Tells the analysis what it cannot find out on its own. Points are written as
"page:bank:offset" in hexadecimal (the same format as the coverage), e.g. "7:00:0B0C"
= instruction of bank $00 seen in window 7, offset $0B0C (logical address $EB0C).

    rom_crc32 = "5F6F3C2A"

    [[target]]               # target bank of a call/jump/branch
    source = "7:00:0B0C"     # the instruction
    bank = "01"              # bank that was in the target's window
    reason = "free text"

    [[function]]             # function entry the analysis did not find
    entry = "5:01:0BCF"
    reason = "..."

    [[data]]                 # region that is NOT code (nothing deduced goes there)
    bank = "09"
    start = "1F14"           # offset inside the bank (inclusive)
    end = "1F33"             # (inclusive)
    reason = "..."

    [[table]]                # JMP (abs,X) table with a fixed number of entries
    source = "4:09:1F11"     # the indirect JMP
    entries = 8
    reason = "..."
"""
import tomllib

from .coverage import Point


def parse_point(text):
    parts = text.split(":")
    if len(parts) != 3:
        raise ValueError(f"invalid point '{text}' (use page:bank:offset, e.g. 7:00:0B0C)")
    page, bank, offset = (int(x, 16) for x in parts)
    if not (0 <= page <= 7 and 0 <= bank <= 0xFF and 0 <= offset <= 0x1FFF):
        raise ValueError(f"point out of range '{text}'")
    return Point(page, bank, offset)


def format_point(p):
    return f"{p.page}:{p.bank:02X}:{p.offset:04X}"


class Config:
    def __init__(self, data=None, path=None):
        data = data or {}
        self.path = path
        crc = data.get("rom_crc32")
        self.rom_crc32 = int(crc, 16) if crc else None
        self.destinations = {}  # Point of the instruction -> target bank
        self.functions = set()  # Point
        self.data_regions = []  # (bank, start, end)
        self.tables = {}        # Point of the indirect JMP -> number of entries
        for item in data.get("target", []):
            self.destinations[parse_point(item["source"])] = int(item["bank"], 16)
        for item in data.get("function", []):
            self.functions.add(parse_point(item["entry"]))
        for item in data.get("data", []):
            start, end = int(item["start"], 16), int(item["end"], 16)
            if not 0 <= start <= end <= 0x1FFF:
                raise ValueError(f"invalid data region: {item}")
            self.data_regions.append((int(item["bank"], 16), start, end))
        for item in data.get("table", []):
            self.tables[parse_point(item["source"])] = int(item["entries"])
        self.used = set()  # applied rules (to warn about the ones that were never used)

    @classmethod
    def load(cls, path):
        with open(path, "rb") as f:
            return cls(tomllib.load(f), path)

    def is_data(self, bank, offset):
        return any(b == bank and start <= offset <= end for b, start, end in self.data_regions)

    def unused(self):
        """Rules the analysis never needed to apply (probably a typo or an outdated rule)."""
        out = []
        for p in self.destinations:
            if ("target", p) not in self.used:
                out.append(f"target {format_point(p)}")
        for p in self.functions:
            if ("function", p) not in self.used:
                out.append(f"function {format_point(p)}")
        for p in self.tables:
            if ("table", p) not in self.used:
                out.append(f"table {format_point(p)}")
        return sorted(out)
