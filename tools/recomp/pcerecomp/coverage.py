"""Reading and checking of the coverage files written by the runtime
(format described in runtime/pce/coverage.h).

A point is (page, bank, offset): the 8 KB logical window the instruction was in (0-7),
the physical bank (0x00-0xFF) and the offset inside the bank (0-0x1FFF).
The logical address is page * 0x2000 + offset.
"""
from collections import namedtuple

from .disasm import decode
from .opcodes import FLOW_ILLEGAL

Point = namedtuple("Point", "page bank offset")
Edge = namedtuple("Edge", "kind src dst")

EDGE_KINDS = {
    "C": "call (JSR/BSR)",
    "J": "jump (JMP abs)",
    "N": "indirect jump (JMP (abs) / JMP (abs,X))",
    "R": "return (RTS)",
    "T": "return from interrupt (RTI)",
    "K": "BRK",
    "I": "interrupt",
    "S": "instruction at the end of the window (records the bank of the next window)",
}


def logical(point):
    return (point.page << 13) | point.offset


def is_rom(point):
    return point.bank < 0x80


class Coverage:
    def __init__(self):
        self.exec = set()
        self.edges = set()
        # CD games (coverage v2): RAM point -> disc positions of the code that ran there
        # (None = bytes that did not come intact from the disc)
        self.origins = {}
        self.origin_mismatches = 0  # bytes the runtime found differing from the disc (should be 0)
        self.loads = set()  # (RAM start, size, disc position): stretches with code copied from the disc
        # CD games: (page, real bank, offset) of the instructions that ran modified by the game
        # (bytes differing from the disc); filled by CdProgram.virtual_coverage
        self.modified = set()
        self.rom_crc32 = None
        self.instructions = 0
        self.files = []

    @classmethod
    def load(cls, paths):
        """Reads one or more files and merges everything (each play session is one file)."""
        cov = cls()
        for path in paths:
            cov._load_one(path)
        return cov

    def _load_one(self, path):
        self.files.append(path)
        with open(path, encoding="ascii") as f:
            lines = f.readlines()
        # Recorded without the BIOS file (routines in C): the ROM is the synthetic BIOS, which is not the
        # BIOS the recompiler reads. Only the game code counts: from the ROM, only the BIOS entries
        # ($E000-$E0FF, the same as in the real one), as points and as targets of the game's calls
        synthetic = any(line.split()[:2] == ["#", "synthetic-bios"] for line in lines)

        def keep(p):
            return not synthetic or not is_rom(p) or (p.page == 7 and p.bank == 0 and p.offset < 0x100)
        for n, line in enumerate(lines, 1):
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "#":
                if len(parts) >= 3 and parts[1] == "rom-crc32" and not synthetic:
                    crc = int(parts[2], 16)
                    if self.rom_crc32 is not None and crc != self.rom_crc32:
                        raise ValueError(f"{path}: coverage of another ROM ({crc:08X})")
                    self.rom_crc32 = crc
                elif len(parts) >= 3 and parts[1] == "executed-instructions":
                    self.instructions += int(parts[2])
                elif len(parts) >= 7 and parts[1] == "origin-bytes-checked":
                    self.origin_mismatches += int(parts[6])  # "A, differing from disc B"
                continue
            if parts[0] == "L" and len(parts) == 4:
                self.loads.add(tuple(int(x, 16) for x in parts[1:4]))  # (RAM start, size, disc)
                continue
            if parts[0] == "O" and len(parts) == 5:
                point = Point(*(int(x, 16) for x in parts[1:4]))
                self.origins.setdefault(point, set()).add(None if parts[4] == "-" else int(parts[4], 16))
                continue
            nums = [int(x, 16) for x in parts[1:] if x not in EDGE_KINDS]
            if parts[0] == "X" and len(nums) == 3:
                if keep(Point(*nums)):
                    self.exec.add(Point(*nums))
            elif parts[0] == "E" and len(parts) == 8 and parts[1] in EDGE_KINDS:
                src, dst = Point(*nums[0:3]), Point(*nums[3:6])
                if keep(dst) and (not synthetic or not is_rom(src)):
                    self.edges.add(Edge(parts[1], src, dst))
            else:
                raise ValueError(f"{path}:{n}: invalid line: {line.strip()}")

    def next_bank(self, point):
        """Bank that was in the next window when the instruction at `point` (near the end of the
        window) executed. Without an S record (old files), assumes bank + 1, which is how games
        usually lay out code that crosses windows."""
        banks = sorted(e.dst.bank for e in self.edges if e.kind == "S" and e.src == point)
        if banks:
            return banks[0]
        return (point.bank + 1) & 0xFF

    def summary(self):
        banks = {}
        for p in self.exec:
            banks[p.bank] = banks.get(p.bank, 0) + 1
        kinds = {}
        for e in self.edges:
            kinds[e.kind] = kinds.get(e.kind, 0) + 1
        return banks, kinds


def check(cov, rom):
    """Checks the coverage against the ROM and the disassembler. Returns the list of problems (empty = ok).

    - the ROM is the same (CRC32);
    - every instruction recorded in the ROM is a documented opcode;
    - recorded JSR/BSR/JMP go to the target written in the code itself;
    - a recorded indirect JMP really is a JMP (abs)/(abs,X); same for RTS/RTI/BRK;
    - the target of every edge was executed (except, at most, the last instruction of each session).
    """
    problems = []
    if cov.rom_crc32 is not None and cov.rom_crc32 != rom.crc32:
        problems.append(f"coverage CRC32 {cov.rom_crc32:08X} != ROM {rom.crc32:08X}")
        return problems

    def instruction_at(point):
        mpr = [0] * 8
        mpr[point.page] = point.bank
        mpr[(point.page + 1) & 7] = cov.next_bank(point)
        return decode(lambda a: rom.read_logical(mpr, a), logical(point))

    for p in sorted(cov.exec):
        if not is_rom(p):
            continue
        try:
            ins = instruction_at(p)
        except ValueError as err:  # instruction continues into a window that is not ROM
            problems.append(f"{p.page}:{p.bank:02X}:{p.offset:04X}: could not read ({err})")
            continue
        if ins.opcode.flow == FLOW_ILLEGAL:
            problems.append(f"{p.page}:{p.bank:02X}:{p.offset:04X}: undocumented opcode {ins.opcode.code:02X}")

    expected_mnemonics = {"N": ("JMP",), "R": ("RTS",), "T": ("RTI",), "K": ("BRK",)}
    missing_dst = []
    for e in sorted(cov.edges):
        if e.kind == "S":
            continue
        if e.dst not in cov.exec:
            missing_dst.append(e)
        if not is_rom(e.src):
            continue
        try:
            ins = instruction_at(e.src)
        except ValueError:
            continue  # already reported above
        where = f"{e.src.page}:{e.src.bank:02X}:{e.src.offset:04X}"
        if e.kind in ("C", "J"):
            if ins.target is None or ins.opcode.flow not in ("call", "jump"):
                problems.append(f"{where}: edge {e.kind} but the instruction is '{ins.text}'")
            elif ins.target != logical(e.dst):
                problems.append(f"{where}: '{ins.text}' went to ${logical(e.dst):04X}")
        elif e.kind in expected_mnemonics and ins.opcode.mnemonic not in expected_mnemonics[e.kind]:
            problems.append(f"{where}: edge {e.kind} but the instruction is '{ins.text}'")
    if len(missing_dst) > len(cov.files):
        problems.append(f"{len(missing_dst)} edges with a never executed target, e.g.: {missing_dst[:3]}")
    return problems


def ram_linear(point):
    """Position of the point inside the tracked RAM (as in runtime/cd/origin.c): CD RAM 0-$FFFF,
    work RAM from $10000."""
    if 0x80 <= point.bank <= 0x87:
        return (point.bank - 0x80) * 0x2000 + point.offset
    return 0x10000 + point.offset


def instruction_from_disc(disc, point, position):
    """Decodes the instruction that ran at `point` reading the disc bytes starting at `position`."""
    base = logical(point)
    return decode(lambda a: disc.read(position + ((a - base) & 0xFFFF)), base)


def check_origins(cov, disc):
    """Checks coverage v2 (code loaded from the CD) against the disc image.

    - the runtime found no byte differing from the disc;
    - every instruction with an origin decodes, from the disc, to a documented opcode;
    - recorded JSR/BSR/JMP leaving a RAM point with a single origin go to the target written
      in the code read from the disc.
    Returns (problems, stats)."""
    problems = []
    if cov.origin_mismatches:
        problems.append(f"the runtime found {cov.origin_mismatches} bytes differing from the disc")
    checked = 0
    for point, positions in sorted(cov.origins.items()):
        for pos in positions:
            if pos is None:
                continue
            try:
                ins = instruction_from_disc(disc, point, pos)
            except ValueError as err:
                problems.append(f"{point.page}:{point.bank:02X}:{point.offset:04X} <- disc {pos:X}: {err}")
                continue
            checked += 1
            if ins.opcode.flow == FLOW_ILLEGAL:
                problems.append(f"{point.page}:{point.bank:02X}:{point.offset:04X} <- disc {pos:X}: "
                                f"undocumented opcode {ins.opcode.code:02X}")
    jumps = 0
    for e in sorted(cov.edges):
        if e.kind not in ("C", "J") or e.src not in cov.origins:
            continue
        positions = cov.origins[e.src] - {None}
        if len(positions) != 1 or None in cov.origins[e.src]:
            continue
        ins = instruction_from_disc(disc, e.src, next(iter(positions)))
        jumps += 1
        if ins.target is None or ins.target != logical(e.dst):
            problems.append(f"{e.src.page}:{e.src.bank:02X}:{e.src.offset:04X}: '{ins.text}' (read from the disc) "
                            f"went to ${logical(e.dst):04X}")
    stats = {"instructions checked against the disc": checked, "jumps/calls checked": jumps}
    return problems, stats


def overlay_loads(cov):
    """Groups the code loaded from the CD into "loads": stretches copied from the disc to RAM in one go,
    in which the difference between the disc position and the RAM position is constant.
    Returns a list of dicts (disc range, RAM banks used, instructions), from largest to smallest."""
    loads = {}
    for point, positions in cov.origins.items():
        for pos in positions:
            if pos is None:
                continue
            delta = pos - ram_linear(point)
            entry = loads.setdefault(delta, {"points": [], "delta": delta})
            entry["points"].append((point, pos))
    result = []
    for entry in loads.values():
        pts = entry["points"]
        lo = min(p for _, p in pts)
        hi = max(p for _, p in pts)
        banks = sorted({pt.bank for pt, _ in pts})
        result.append({"disc": (lo, hi), "banks": banks, "instructions": len(pts)})
    return sorted(result, key=lambda r: -r["instructions"])
