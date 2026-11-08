"""Disc map of a CD game (games/<game>/disc.toml): which stretches of the disc the game puts in RAM
with code, where, which window each bank runs in, and where the code starts.

With the map, the recompiler finds all the game's code from the disc, without recorded play sessions
(coverage is then only used for checking). Format (TOML, in the game project):

  base_sector           1st sector of the data track
  [table]               file size table (sector, offset, entries, sectors_per_file, first):
                        file N starts at sector base_sector + (N + first) * sectors_per_file
  [[family]]            a group of loads with the same RAM layout:
    files | size | sector   which ones: a list of numbers, every file with that size in the
                            table, or a fixed stretch of the disc (sector + sectors)
    sectors             load size (otherwise, the one from the table)
    ram                 tracked RAM position (as in runtime/cd/origin.c: CD RAM 0-$FFFF,
                        work RAM $10000+)
    banks               real bank -> window its code runs in
    entries             entry points ("page:bank:offset", real bank)
    routine_records     game tables holding a routine address and its bank: { address = position
                        of the word with the address (default 0), bank = position of the bank byte,
                        bank_add = how much to add to it to get the real bank (default 0), discard =
                        { position = value } (a record with that byte is invalid, e.g. end of list) };
                        searches the whole file for records whose address falls in the window of the
                        given bank, and each address becomes an entry
    loose_pointers      guesses: every word in the file that falls in the window of a code bank
    script              guesses: script opcode -> position of the word with a routine address
  [[far_call]]          routine (logical address), bank (real) and, optionally, family: JSR routine /
                        BRA *+4 / .dw target calls the target in that bank
Guesses are only accepted if the code starting at them decodes completely (Analysis._plausible).
"""
import tomllib

from .config import parse_point
from .coverage import Point

SECTOR = 2048
# Reason of an entry that is only a guess (pointer found in data): the analysis only accepts it if
# the code starting at it decodes completely with no invalid opcode (Analysis._plausible)
WEAK = "guess: "


class Load:
    """A load: `length` bytes of the disc starting at `pos` (sector * 2048) placed at `ram`."""

    def __init__(self, family, number, pos, length, ram):
        self.family = family
        self.number = number  # file number in the table (None = fixed stretch)
        self.pos = pos
        self.length = length
        self.ram = ram

    @property
    def delta(self):
        """Disc position - RAM position: identifies the load (as in program.py)."""
        return self.pos - self.ram

    def name(self):
        return f"{self.family.name}" + (f" ${self.number:02X}" if self.number is not None else "")


class Family:
    def __init__(self, data):
        self.name = data["name"]
        self.files = data.get("files")
        self.size = data.get("size")
        self.sector = data.get("sector")
        self.sectors = data.get("sectors")
        self.ram = data["ram"]
        self.banks = {int(k, 16): int(v) for k, v in data.get("banks", {}).items()}
        self.entries = [parse_point(t) for t in data.get("entries", [])]
        rr = data.get("routine_records")
        # (address position, bank position, bank add, {position: value that discards})
        self.routine_records = None if rr is None else (
            int(rr.get("address", 0)), int(rr["bank"]), int(rr.get("bank_add", 0)),
            {int(k): int(v) for k, v in rr.get("discard", {}).items()})
        self.loose_pointers = bool(data.get("loose_pointers", False))
        self.script_ops = {int(k, 16): int(v) for k, v in data.get("script", {}).items()}
        for p in self.entries:
            if self.banks.get(p.bank) != p.page:
                raise ValueError(f"family {self.name}: entry {t_point(p)} outside the 'banks' windows")


def t_point(p):
    return f"{p.page}:{p.bank:02X}:{p.offset:04X}"


class DiscMap:
    def __init__(self, data, disc):
        self.base = data["base_sector"]
        t = data["table"]
        self.table_sector = t["sector"]
        self.table_offset = t["offset"]
        self.table_entries = t["entries"]
        self.file_sectors = t["sectors_per_file"]
        self.first = t["first"]
        self.disc = disc
        self.sizes = list(disc.read_bytes(self.table_sector * SECTOR + self.table_offset, self.table_entries))
        self.families = [Family(f) for f in data.get("family", [])]
        # Far call: logical address of the routine -> (real bank of the target, load family or None)
        self.far_calls = {}
        for item in data.get("far_call", []):
            fam = item.get("family")
            if fam is not None and fam not in {f.name for f in self.families}:
                raise ValueError(f"far_call: unknown family '{fam}'")
            self.far_calls[item["routine"]] = (int(item["bank"], 16), fam)
        self.loads = []
        for fam in self.families:
            if fam.sector is not None:
                self.loads.append(Load(fam, None, fam.sector * SECTOR, fam.sectors * SECTOR, fam.ram))
                continue
            numbers = fam.files if fam.files is not None else [
                n for n, s in enumerate(self.sizes) if s == fam.size]
            for n in numbers:
                sectors = fam.sectors if fam.sectors is not None else self.sizes[n]
                self.loads.append(Load(fam, n, self.file_pos(n), sectors * SECTOR, fam.ram))

    @classmethod
    def load(cls, path, disc):
        with open(path, "rb") as f:
            return cls(tomllib.load(f), disc)

    def file_pos(self, number):
        return (self.base + (number + self.first) * self.file_sectors) * SECTOR

    def file_of(self, pos):
        """Number of the table file that contains the disc position (or None)."""
        n = (pos // SECTOR - self.base) // self.file_sectors - self.first
        return n if 0 <= n < self.table_entries else None

    def far_call_banks(self, program, routine, src_vbank):
        """Virtual banks holding the target of a far call through routine `routine` (logical
        address) made from virtual bank `src_vbank`; None if the routine is not a far call."""
        if routine not in self.far_calls:
            return None
        real, fam = self.far_calls[routine]
        if fam is None:
            vb = program.same_load(src_vbank, real)
            return [vb] if vb is not None else []
        return sorted(vb for ld in self.loads if ld.family.name == fam
                      for vb in [program.vbank.get((ld.delta, real))] if vb is not None)

    # ---- entry points ----

    def seeds(self, program):
        """Entry points (in virtual banks of `program`) of all loads."""
        out = []
        for ld in self.loads:
            for p in ld.family.entries:
                vb = program.vbank.get((ld.delta, p.bank))
                if vb is None:
                    continue
                out.append((Point(p.page, vb, p.offset), f"entry of {ld.name()}"))
                # Jump table: consecutive JMP abs starting at the entry (the resident code at $4000 and
                # the programs at $4800 start like this; each JMP is a routine called from outside)
                off = p.offset + 3
                while off + 3 <= 0x2000 and program.read(vb, p.offset) == 0x4C and program.read(vb, off) == 0x4C:
                    out.append((Point(p.page, vb, off), f"jump table of {ld.name()}"))
                    off += 3
            if ld.family.loose_pointers:
                for p in self._loose_pointers(ld):
                    vb = program.vbank.get((ld.delta, p.bank))
                    if vb is not None:
                        out.append((Point(p.page, vb, p.offset), WEAK + f"loose pointer of {ld.name()}"))
            if ld.family.script_ops:
                for p in self._script_pointers(ld):
                    vb = program.vbank.get((ld.delta, p.bank))
                    if vb is not None:
                        out.append((Point(p.page, vb, p.offset), WEAK + f"script of {ld.name()}"))
            if ld.family.routine_records:
                for p in self._routine_records(ld):
                    vb = program.vbank.get((ld.delta, p.bank))
                    if vb is not None:
                        out.append((Point(p.page, vb, p.offset), f"routine record of {ld.name()}"))
        return out

    def _loose_pointers(self, ld):
        """Every 16-bit word of the file that points into the window of one of the load's code banks
        (the analysis drops the ones that do not decode as code)."""
        data = self.disc.read_bytes(ld.pos, ld.length)
        found = set()
        for real, page in ld.family.banks.items():
            for i in range(len(data) - 1):
                addr = data[i] | (data[i + 1] << 8)
                if addr >> 13 == page:
                    found.add(Point(page, real, addr & 0x1FFF))
        return sorted(found)

    def _script_pointers(self, ld):
        """Script commands (e.g. from cutscenes) holding a routine address (`script`: opcode ->
        position of the word after it; e.g. with { "14" = 1, "04" = 2 }, 14 lo hi and 04 xx lo hi).
        Guesses: the analysis only accepts the ones that decode."""
        data = self.disc.read_bytes(ld.pos, ld.length)
        found = set()

        def add(i):
            addr = data[i] | (data[i + 1] << 8)
            for real, pg in ld.family.banks.items():
                if pg == addr >> 13:
                    found.add(Point(pg, real, addr & 0x1FFF))

        for i in range(len(data) - 3):
            skip = ld.family.script_ops.get(data[i])
            if skip is not None and i + skip + 1 < len(data):
                add(i + skip)
        return sorted(found)

    def _routine_records(self, ld):
        """Game tables holding a routine address and its bank (`routine_records`; e.g. a list
        [address (2 bytes), priority (FF = end), bank - $80] has address = 0, bank = 3,
        bank_add = 0x80, discard = { 2 = 0xFF }). Searches the whole file, at every position, for
        records whose bank (byte + add) is one of the load's code banks and whose address falls in
        its window."""
        addr_at, bank_at, add, skip = ld.family.routine_records
        size = max([addr_at + 2, bank_at + 1] + [k + 1 for k in skip])
        data = self.disc.read_bytes(ld.pos, ld.length)
        windows = {(real - add) & 0xFF: (real, page) for real, page in ld.family.banks.items()}
        found = set()
        for i in range(len(data) - size + 1):
            if any(data[i + k] == v for k, v in skip.items()):
                continue
            bank = data[i + bank_at]
            if bank not in windows:
                continue
            real, page = windows[bank]
            addr = data[i + addr_at] | (data[i + addr_at + 1] << 8)
            if addr >> 13 == page:
                found.add(Point(page, real, addr & 0x1FFF))
        return sorted(found)
