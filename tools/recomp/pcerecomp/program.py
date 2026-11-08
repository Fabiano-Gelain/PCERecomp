"""The "program" of a CD game for the recompiler: the BIOS (System Card, read as a HuCard ROM
in banks $00-$7F) plus the code the game loads from the disc into RAM.

Code loaded from the disc has no fixed bank: the same RAM bank receives different parts of the
game at different times. Each LOAD (stretch copied from the disc in one go; coverage v2, L lines)
becomes, in each RAM bank it occupies, a VIRTUAL BANK (number >= 0x100). For the analysis and the C
generator, a virtual bank is like a ROM bank: the bytes come from the disc. The real bank (the one in
the MPR when the code runs) is kept separately; at run time, before using a virtual-bank function,
the RAM bytes are checked against the disc ones (codegen.py).

The interface is the same as Rom (read, read_logical, vectors, crc32, size) plus:
  is_code_bank(bank), real_bank(bank), next_bank(bank), load_key(bank).
"""
from collections import defaultdict

from .coverage import Coverage, Edge, Point, is_rom, logical
from .disasm import decode

VIRTUAL_BASE = 0x100
GAP_FILL = 32  # gaps smaller than this between stretches of the same load are filled in (modified code)


def ram_linear(bank, offset):
    """Position in the tracked RAM (as in runtime/cd/origin.c): CD RAM 0-$FFFF, work RAM $10000+."""
    if 0x80 <= bank <= 0x87:
        return (bank - 0x80) * 0x2000 + offset
    if 0xF8 <= bank <= 0xFB:
        return 0x10000 + offset
    return None


def ram_bank_of(linear):
    return 0x80 + linear // 0x2000 if linear < 0x10000 else 0xF8


class CdProgram:
    def __init__(self, rom, disc, coverage=None, discmap=None):
        """Loads from the coverage (L lines: only the stretches the play sessions saw) or, with `discmap`
        (discmap.py), from the disc map: all the loads with game code."""
        self.rom = rom
        self.disc = disc
        self.discmap = discmap
        self.crc32 = rom.crc32
        self.size = rom.size
        self.bank_count = rom.bank_count
        # Loads: delta (disc - RAM) -> RAM ranges [start, end) (merged)
        ranges = defaultdict(list)
        if discmap is not None:
            loads = [(ld.ram, ld.length, ld.pos) for ld in discmap.loads]
        else:
            loads = coverage.loads
        for start, length, pos in loads:
            ranges[pos - start].append((start, start + length))
        self.ranges = {}
        for delta, rs in ranges.items():
            merged = []
            for s, e in sorted(rs):
                if merged and s <= merged[-1][1] + GAP_FILL:
                    merged[-1][1] = max(merged[-1][1], e)
                else:
                    merged.append([s, e])
            self.ranges[delta] = [tuple(r) for r in merged]
        # Virtual banks: one per (load, real RAM bank)
        pairs = set()
        for delta, rs in self.ranges.items():
            for s, e in rs:
                for lin in range(s - s % 0x2000, e, 0x2000):
                    if lin < 0x10000 or lin == 0x10000:
                        pairs.add((ram_bank_of(lin), delta))
        self.vbank = {}   # (delta, real bank) -> virtual bank
        self.vinfo = {}   # virtual bank -> (delta, real bank)
        for n, (real, delta) in enumerate(sorted(pairs)):
            vb = VIRTUAL_BASE + n
            self.vbank[(delta, real)] = vb
            self.vinfo[vb] = (delta, real)
        # Disc map: which window each virtual bank runs in (from the load's family)
        self.load_of = {}   # delta -> map Load
        self.vpages = {}    # virtual bank -> window
        if discmap is not None:
            for ld in discmap.loads:
                self.load_of[ld.delta] = ld
                for real, page in ld.family.banks.items():
                    vb = self.vbank.get((ld.delta, real))
                    if vb is not None:
                        self.vpages[vb] = page

    # ---- disc map ----

    def page_banks(self):
        """Window -> virtual banks that run in it (only with the disc map)."""
        out = defaultdict(set)
        for vb, page in self.vpages.items():
            out[page].add(vb)
        return out

    def same_load(self, vbank, real):
        """Virtual bank of the same load as `vbank` with real bank `real` (or None)."""
        if vbank not in self.vinfo:
            return None
        return self.vbank.get((self.vinfo[vbank][0], real))

    def load_name(self, vbank):
        if vbank not in self.vinfo:
            return "BIOS" if vbank < 0x80 else f"bank {vbank:02X}"
        delta, real = self.vinfo[vbank]
        ld = self.load_of.get(delta)
        return f"{ld.name()} (bank {real:02X})" if ld else f"load {delta:X} (bank {real:02X})"

    # ---- Rom interface ----

    def is_code_bank(self, bank):
        return bank < 0x80 or bank in self.vinfo

    def real_bank(self, bank):
        return self.vinfo[bank][1] if bank in self.vinfo else bank

    def load_key(self, bank):
        """Same load = same key (the ROM is a single load)."""
        return self.vinfo[bank][0] if bank in self.vinfo else "rom"

    def next_bank(self, bank):
        """Bank that usually comes in the next window (for instructions that cross the end of the window)."""
        if bank in self.vinfo:
            delta, real = self.vinfo[bank]
            return self.vbank.get((delta, real + 1), real + 1)
        return (bank + 1) & 0xFF

    def read(self, bank, offset):
        if bank < 0x80:
            return self.rom.read(bank, offset)
        if bank in self.vinfo:
            delta, real = self.vinfo[bank]
            lin = ram_linear(real, offset & 0x1FFF)
            for s, e in self.ranges[delta]:
                if s <= lin < e:
                    return self.disc.read(delta + lin)
            raise ValueError(f"virtual bank {bank:#x}: ${offset:04X} outside the load")
        raise ValueError(f"bank {bank:#x} is not code")

    def read_logical(self, mpr, address):
        return self.read(mpr[address >> 13], address & 0x1FFF)

    def vectors(self):
        return self.rom.vectors()

    # ---- coverage in virtual banks ----

    def virtual_coverage(self, cov):
        """The coverage with the RAM points replaced by their versions in virtual banks.

        A RAM point becomes one point per disc origin (one version per load that ran there).
        Edges leaving a point with several versions only apply to the versions whose instruction (read
        from the disc) is compatible; the RAM target stays in the version of the same load, if any."""
        versions = defaultdict(list)
        for p, positions in cov.origins.items():
            for pos in positions:
                if pos is None:
                    continue
                vb = self.vbank.get((pos - ram_linear(p.bank, p.offset), p.bank))
                if vb is not None:
                    versions[p].append(Point(p.page, vb, p.offset))

        def translate(p):
            if is_rom(p):
                return [p]
            return versions.get(p, [])

        out = Coverage()
        out.rom_crc32 = cov.rom_crc32
        out.instructions = cov.instructions
        out.files = list(cov.files)
        out.exec = {q for p in cov.exec for q in translate(p)}
        out.modified = {(p.page, p.bank, p.offset) for p, positions in cov.origins.items() if None in positions}
        for e in cov.edges:
            srcs = translate(e.src)
            for src in srcs:
                if len(srcs) > 1 and not self._edge_fits(e, src):
                    continue
                if e.kind == "S":  # instruction crossing the end of the window: continues in the same load
                    out.edges.add(Edge("S", src, Point(e.dst.page, self.next_bank(src.bank), 0)))
                    continue
                dsts = translate(e.dst)
                if not dsts:
                    out.edges.add(Edge(e.kind, src, e.dst))  # target with no version (modified code)
                    continue
                same = [d for d in dsts if self.load_key(d.bank) == self.load_key(src.bank)]
                for d in (same or dsts):
                    out.edges.add(Edge(e.kind, src, d))
        return out

    def _edge_fits(self, e, src):
        try:
            mpr = [0] * 8
            mpr[src.page] = src.bank
            mpr[(src.page + 1) & 7] = self.next_bank(src.bank)
            ins = decode(lambda a: self.read_logical(mpr, a), logical(src))
        except ValueError:
            return False
        if e.kind in ("C", "J"):
            return ins.target == logical(e.dst)
        if e.kind == "N":
            return ins.opcode.mnemonic == "JMP"
        if e.kind == "R":
            return ins.opcode.mnemonic == "RTS"
        if e.kind == "T":
            return ins.opcode.mnemonic == "RTI"
        if e.kind == "K":
            return ins.opcode.mnemonic == "BRK"
        return True

    def stats(self):
        return {
            "loads": len(self.ranges),
            "virtual_banks": len(self.vinfo),
        }
