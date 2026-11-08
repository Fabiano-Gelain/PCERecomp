"""Recursive analysis of the ROM code, with bank tracking.

A code point is (page, bank, offset); see coverage.py. The analysis:
  1. starts from the interrupt vectors and from every point the coverage confirmed;
  2. follows the flow (next instruction, branches, calls, jumps; indirect jumps through the
     targets seen in the coverage);
  3. finds the target bank of calls/jumps into another page from the coverage, from a
     `LDA #n` + `TAM` right before, or because the page always holds the same bank;
  4. resolves conflicts: a confirmed (executed) instruction beats a deduced (static) one.

The result feeds the .asm listing (listing.py) and the C generator.
"""
from collections import defaultdict, namedtuple

from .config import Config
from .coverage import Point, logical
from .disasm import decode
from .opcodes import FLOW_BRANCH, FLOW_CALL, FLOW_ILLEGAL, FLOW_JUMP, FLOW_JUMP_IND, FLOW_NORMAL

# Instructions that do not change the accumulator (to follow the value of LDA #n up to the TAM).
A_PRESERVING = {
    "STA", "STX", "STY", "STZ", "TAM", "TAX", "TAY", "PHA", "PHX", "PHY", "CMP", "CPX", "CPY", "BIT", "TST",
    "LDX", "LDY", "INX", "INY", "DEX", "DEY", "TXS", "TSX", "CLC", "SEC", "CLI", "SEI", "CLD", "SED", "CLV",
    "CSL", "CSH", "CLX", "CLY", "NOP", "ST0", "ST1", "ST2", "SXY", "PLX", "PLY", "TII", "TDD", "TIN", "TIA",
    "TAI", "BCC", "BCS", "BEQ", "BNE", "BMI", "BPL", "BVC", "BVS", "BRA", "SET",
    "RMB0", "RMB1", "RMB2", "RMB3", "RMB4", "RMB5", "RMB6", "RMB7",
    "SMB0", "SMB1", "SMB2", "SMB3", "SMB4", "SMB5", "SMB6", "SMB7",
    "BBR0", "BBR1", "BBR2", "BBR3", "BBR4", "BBR5", "BBR6", "BBR7",
    "BBS0", "BBS1", "BBS2", "BBS3", "BBS4", "BBS5", "BBS6", "BBS7",
}
# Instructions that do not touch the carry (to follow the CLC up to the ADC)
CARRY_PRESERVING = {"CLC", "LDA", "LDX", "LDY", "STA", "STX", "STY", "STZ", "TAM", "TMA", "TAX", "TAY", "TXA",
                    "TYA", "PHA", "PHX", "PHY", "NOP", "SEI", "CLI", "CSL", "CSH"}
_WRITERS = {"STA", "STX", "STY", "STZ", "INC", "DEC", "ASL", "LSR", "ROL", "ROR", "TSB", "TRB"}
_LOAD_IMM = {"LDA": "A", "LDX": "X", "LDY": "Y"}
_STORE = {"STA": "A", "STX": "X", "STY": "Y"}
_TRANSFER = {"TAX": ("A", "X"), "TAY": ("A", "Y"), "TXA": ("X", "A"), "TYA": ("Y", "A")}
_X_CHANGERS = {"LDX", "INX", "DEX", "TSX", "PLX", "SXY", "SAX", "CLX"}
_Y_CHANGERS = {"LDY", "INY", "DEY", "PLY", "SXY", "SAY", "CLY"}


def _track_immediates(op, ins, imm):
    """Updates `imm` (register or 'zNN' -> known immediate value) after the instruction."""
    m = op.mnemonic
    zp = None
    if op.mode == "zp":
        zp = ins.bytes[1]
    elif op.mode == "abs" and ins.bytes[2] == 0x20:  # $20xx = zero page
        zp = ins.bytes[1]
    if m in _LOAD_IMM and op.mode == "imm":
        imm[_LOAD_IMM[m]] = ins.bytes[1]
        imm.pop("tA", None)
        return
    if m == "LDA" and op.mode in ("absx", "absy"):  # read from a table (for JMP ($20nn))
        imm.pop("A", None)
        imm["tA"] = ins.bytes[1] | (ins.bytes[2] << 8)
        return
    if m == "STA" and zp is not None and "tA" in imm:
        imm[f"t{zp:02X}"] = imm["tA"]
        imm.pop(f"z{zp:02X}", None)
        return
    if m in _STORE and zp is not None:
        r = _STORE[m]
        if r in imm:
            imm[f"z{zp:02X}"] = imm[r]
        else:
            imm.pop(f"z{zp:02X}", None)
        return
    if m in _TRANSFER:
        src, dst = _TRANSFER[m]
        if src in imm:
            imm[dst] = imm[src]
        else:
            imm.pop(dst, None)
        return
    if zp is not None:
        imm.pop(f"z{zp:02X}", None)  # another zero-page write/read: forget it to be safe
    if m not in A_PRESERVING:
        imm.pop("A", None)
        imm.pop("tA", None)
    if m in _X_CHANGERS:
        imm.pop("X", None)
    if m in _Y_CHANGERS:
        imm.pop("Y", None)


CodeInfo = namedtuple("CodeInfo", "point ins executed next_bank")
Unresolved = namedtuple("Unresolved", "src target reason")


class Analysis:
    def __init__(self, rom, coverage, vectors=True, config=None, seeds=None):
        """`seeds`: entry points besides the coverage, [(Point, reason)] (disc map)."""
        self.rom = rom
        self.cov = coverage
        self.seeds = list(seeds or [])
        self.seed_reason = {}
        for p, why in self.seeds:
            self.seed_reason.setdefault(p, why)
        # Without coverage (disc map only), no instruction is "confirmed": overlapping instructions
        # are all kept (each one is checked by its signature at run time), instead of dropping one
        # that may be the right one
        self.keep_overlaps = bool(getattr(rom, "vpages", None)) and not coverage.exec
        self.config = config if config is not None else Config()
        if self.config.rom_crc32 is not None and self.config.rom_crc32 != rom.crc32:
            raise ValueError(f"config is for another ROM (CRC32 {self.config.rom_crc32:08X})")
        self.data_executed = []             # executed points inside config [[data]] regions
        self.code = {}                      # Point -> CodeInfo
        self.labels = defaultdict(set)      # Point -> {"sub", "loc", "vec:reset", ...}
        self.unresolved = []                # targets with unknown bank
        self.ram_targets = set()            # (source, target) falling in RAM/I/O
        self.conflicts = []                 # (dropped, winner)
        self.exec_conflicts = []            # two executed points that overlap
        self.dropped = set()
        self.targets = defaultdict(set)     # Point -> resolved targets (branch/jump/call)
        self.indirect = {}                  # Point of the indirect JMP -> (table address, state, indexed)
        self.tables = {}                    # Point of the indirect JMP -> targets read from the table in ROM
        self.by_candidate = set()           # (source, target) resolved by the single-candidate rule
        self.by_context = set()             # (source, target) resolved by learned context
        self._edges_from = defaultdict(set)
        self._span = {}                     # Point -> bank of the next window (S edges)
        # Learned context: when code in bank B (on page P) calls/jumps to page Q, which banks were
        # in Q. If always the same one, it applies to the calls that were not executed.
        self._context = defaultdict(set)    # (P, B, Q) -> banks seen in Q
        for e in coverage.edges:
            self._edges_from[(e.src, e.kind)].add(e.dst)
            if e.kind == "S":
                self._span.setdefault(e.src, e.dst.bank)
            if e.kind in ("C", "J", "N") and self._is_code(e.src) and e.dst.page != e.src.page:
                self._context[(e.src.page, e.src.bank, e.dst.page)].add(e.dst.bank)
        self.page_banks = self._page_banks()
        self._run(vectors)

    # ---- banks ----

    def _is_code(self, point):
        """Point in a code bank: ROM or, in CD games, the virtual bank of a load (program.py)."""
        return self.rom.is_code_bank(point.bank)

    def _page_banks(self):
        """Banks seen executing code on each page (from the coverage)."""
        banks = defaultdict(set)
        for p in self.cov.exec:
            banks[p.page].add(p.bank)
        if getattr(self.rom, "vpages", None):  # disc map: the banks of each window come from it
            for page, vbs in self.rom.page_banks().items():
                banks[page] |= vbs
        return banks

    def _state_bank(self, src, state, page):
        """Bank of window `page` from the state (LDA #n + TAM). In the RAM of a CD game, the real bank
        becomes the virtual bank of the same load as `src` (disc map)."""
        bank = state[page]
        if not self.rom.is_code_bank(bank) and hasattr(self.rom, "same_load"):
            vb = self.rom.same_load(src.bank, bank)
            if vb is not None:
                return vb
        return bank

    def _map_banks(self, src, page):
        """Disc map: the banks of the same load as `src` that run in window `page`."""
        if not getattr(self.rom, "vpages", None) or src.bank not in self.rom.vinfo:
            return []
        key = self.rom.load_key(src.bank)
        return sorted(b for b in self.page_banks.get(page, ()) if self.rom.load_key(b) == key)

    def _map_bank(self, src, page):
        cands = self._map_banks(src, page)
        return cands[0] if len(cands) == 1 else None

    def _resolve(self, src, target, state, kinds):
        """Possible target points for a logical address `target` leaving `src`."""
        page = target >> 13
        offset = target & 0x1FFF
        if page == src.page:
            if getattr(self.rom, "vpages", None) and not self._in_load(src.bank, offset):
                return self._other_loads(page, offset)  # e.g. from the program ($4800+) to the resident code ($4000)
            return [Point(page, src.bank, offset)]
        seen = sorted(d for k in kinds for d in self._edges_from.get((src, k), ()) if d.page == page)
        if seen:
            return seen
        if src in self.config.destinations:
            self.config.used.add(("target", src))
            return [Point(page, self.config.destinations[src], offset)]
        if page in state:
            return [Point(page, self._state_bank(src, state, page), offset)]
        if page == ((src.page + 1) & 7) and src in self._span:
            return [Point(page, self._span[src], offset)]
        if len(self.page_banks.get(page, ())) == 1:
            return [Point(page, next(iter(self.page_banks[page])), offset)]
        # Disc map: all the banks of the same load that run in that window (e.g. a program that
        # swaps banks 81 and 82 in window 3). The wrong one becomes extra code that never runs: the
        # signature and the bank are checked at run time
        banks = self._map_banks(src, page)
        if banks:
            return [Point(page, b, offset) for b in banks]
        if getattr(self.rom, "vpages", None) and src.bank in self.rom.vinfo:
            # The load of `src` has no bank in that window (e.g. the area calls a routine of the program
            # at $4xxx): any load whose bank runs there
            out = self._other_loads(page, offset)
            if out:
                return out
        bank = self._context_bank(src, page)
        if bank is not None:
            dst = Point(page, bank, offset)
            self.by_context.add((src, dst))
            return [dst]
        # Page that switches banks: if only one of the banks seen on it has confirmed code
        # at exactly that address, it is that one.
        cands = [b for b in sorted(self.page_banks.get(page, ())) if Point(page, b, offset) in self.cov.exec]
        if len(cands) == 1:
            dst = Point(page, cands[0], offset)
            self.by_candidate.add((src, dst))
            return [dst]
        self.unresolved.append(Unresolved(src, target, "unknown bank"))
        return []

    def _after_terminators(self):
        """Disc map: right after an RTS/RTI/JMP/BRA another routine often starts (e.g. the 2nd phase
        of a character, whose address the game computes). Guesses, checked by _plausible."""
        work = []
        for p, info in list(self.code.items()):
            op = info.ins.opcode
            if op.mnemonic not in ("RTS", "RTI", "JMP", "BRA"):
                continue
            t = Point(p.page, p.bank, p.offset + op.size)
            if t.offset >= 0x2000 or t in self.code or t in self.rejected or t in self._tried_after:
                continue
            self._tried_after.add(t)
            if self._in_load(t.bank, t.offset) and self._plausible(t):
                self.weak.add(t)
                self.labels[t].add("sub")
                work.append((t, {}))
            else:
                self.rejected.add(t)
        return work

    def _in_load(self, bank, offset):
        try:
            self.rom.read(bank, offset)
            return True
        except ValueError:
            return False

    def _other_loads(self, page, offset):
        """Disc map: the address in each load whose bank runs in window `page` and that has that byte.
        If there is more than one, they are guesses (Analysis.weak), each checked by _plausible."""
        cands = [b for b in sorted(self.page_banks.get(page, ())) if self._in_load(b, offset)]
        out = []
        for b in cands:
            t = Point(page, b, offset)
            if t in self.code:
                out.append(t)
            elif len(cands) == 1:
                out.append(t)
            elif t not in self.rejected:
                if self._plausible(t):
                    self.weak.add(t)
                    out.append(t)
                else:
                    self.rejected.add(t)
        return out

    def _bank_for(self, src, page, state):
        """Bank of a page as seen from `src`, without reporting failure (for tables)."""
        if page == src.page:
            return src.bank
        if page in state:
            return self._state_bank(src, state, page)
        if page == ((src.page + 1) & 7) and src in self._span:
            return self._span[src]
        if len(self.page_banks.get(page, ())) == 1:
            return next(iter(self.page_banks[page]))
        bank = self._map_bank(src, page)
        if bank is not None:
            return bank
        return self._context_bank(src, page)

    def _context_bank(self, src, page):
        """Bank of page `page` when the code at `src` runs, learned from the coverage.

        1. Seen directly: calls/jumps from the bank of `src` to `page` always found the same bank.
        2. Banks that show up together: other banks that saw the bank of `src` on its page
           always saw the same bank in `page` (e.g. whoever sees 0B in window 6 sees 0A in window 5)."""
        direct = self._context.get((src.page, src.bank, page), set())
        if len(direct) == 1:
            return next(iter(direct))
        if direct:
            return None
        companions = set()
        for (p, b, q), banks in self._context.items():
            if q == src.page and banks == {src.bank}:
                companions |= self._context.get((p, b, page), set())
        if len(companions) == 1:
            return next(iter(companions))
        return None

    # ---- reading ----

    def _next_bank(self, point, state):
        page = (point.page + 1) & 7
        if point in self._span:
            return self._span[point]
        if page in state:
            return state[page]
        return self.rom.next_bank(point.bank)

    def _decode(self, point, next_bank):
        mpr = [0] * 8
        mpr[point.page] = point.bank
        mpr[(point.page + 1) & 7] = next_bank
        return decode(lambda a: self.rom.read_logical(mpr, a), logical(point))

    # ---- traversal ----

    def _run(self, vectors):
        work = []
        if vectors:
            for name, addr in self.rom.vectors().items():
                if name == "nmi":
                    continue  # the console has no NMI
                p = Point(addr >> 13, 0, addr & 0x1FFF)  # at reset, MPR7 = 0
                self.labels[p].add(f"vec_{name}")
                work.append((p, {7: 0}))
        for p in sorted(self.cov.exec):
            if self._is_code(p):
                work.append((p, {}))
        self.rejected = set()  # rejected guesses (_plausible)
        self._tried_after = set()
        self.weak = set()      # accepted guesses (less certain entries: functions._partition leaves them for last)
        for p, why in self.seeds:
            if not self._is_code(p):
                continue
            if why.startswith("guess: "):
                if not self._plausible(p):
                    self.rejected.add(p)
                    continue
                self.weak.add(p)
            self.labels[p].add("sub")
            work.append((p, {}))
        for p in sorted(self.config.functions):
            self.config.used.add(("function", p))
            self.labels[p].add("sub")
            work.append((p, {}))
        for e in sorted(self.cov.edges):
            if e.kind in ("C", "J", "N", "I", "K") and self._is_code(e.dst):
                self.labels[e.dst].add("sub" if e.kind in ("C", "I", "K") else "loc")

        # Rounds: follow the known flow; then read the jump tables (knowing where there is already code,
        # so code is not read as a table); repeat while something new shows up.
        # The traversal order does not matter: conflicts are resolved at the end (_resolve_conflicts).
        visited = set()
        while work:
            while work:
                point, state = work.pop()
                if point in visited:
                    continue
                visited.add(point)
                if not self._is_code(point):
                    continue
                try:
                    next_bank = self._next_bank(point, state)
                    ins = self._decode(point, next_bank)
                except ValueError:
                    continue
                executed = point in self.cov.exec
                if ins.opcode.flow == FLOW_ILLEGAL and not executed:
                    continue  # data read as code: do not follow
                if self._in_data_region(point, ins):
                    if not executed:
                        continue  # the config says this is data
                    self.data_executed.append(point)
                self.code[point] = CodeInfo(point, ins, executed, next_bank)
                work.extend(self._successors(point, ins, state, executed, next_bank))
            work = self._read_tables()
            if not work and getattr(self.rom, "vpages", None):
                work = self._after_terminators()

        for site, (table_addr, _state, _indexed) in self.indirect.items():
            if not self.targets.get(site):
                self.unresolved.append(Unresolved(site, table_addr, "indirect jump with no known target"))
        self._resolve_conflicts()
        self.modified = self._self_modified() if getattr(self.rom, "vpages", None) else set()

    def _self_modified(self):
        """Disc map (no coverage): instructions whose operands the game itself rewrites in RAM
        (e.g. a game that changes the value of ST1/ST2 #imm in loops that draw to VRAM). A write to a
        fixed address (STA/STX/STY/STZ abs...) that hits an operand byte of an instruction in the same
        load marks that instruction: the generator reads the operand from RAM at run time
        (codegen._live_operand). Returns {(window, real bank, offset)}, like Coverage.modified."""
        operand_of = {}
        for q, info in self.code.items():
            for i in range(1, info.ins.opcode.size):
                if q.offset + i < 0x2000:
                    operand_of.setdefault((q.bank, q.offset + i), []).append(q)
        out = set()
        for p, info in self.code.items():
            op = info.ins.opcode
            if op.mode != "abs" or not (op.mnemonic in _WRITERS or op.mnemonic[:3] in ("RMB", "SMB")):
                continue
            addr = info.ins.bytes[1] | (info.ins.bytes[2] << 8)
            page = addr >> 13
            banks = [p.bank] if page == p.page else self._map_banks(p, page)
            for b in banks:
                for q in operand_of.get((b, addr & 0x1FFF), ()):
                    out.add((q.page, self.rom.real_bank(q.bank), q.offset))
        return out

    def _in_data_region(self, point, ins):
        if not self.config.data_regions:
            return False
        return any(self.config.is_data(point.bank, point.offset + i)
                   for i in range(ins.opcode.size) if point.offset + i < 0x2000)

    # ---- jump tables ----

    def _code_bytes(self):
        return {b for info in self.code.values() for b in self._bytes(info)}

    def _read_tables(self):
        """Reads from the ROM the JMP (abs,X) / JMP (abs) tables not yet read. Returns new work."""
        work = []
        code_bytes = None
        for site, (table_addr, state, indexed) in list(self.indirect.items()):
            if site in self.tables:
                continue
            if code_bytes is None:
                code_bytes = self._code_bytes()
            entries = self._table_entries(site, table_addr, state, indexed, code_bytes)
            self.tables[site] = entries
            for t in entries:
                self.labels[t].add("loc")
                self.targets[site].add(t)
                work.append((t, {}))
        return work

    def _table_entries(self, site, table_addr, state, indexed, code_bytes):
        page = table_addr >> 13
        bank = self._bank_for(site, page, state)
        if bank is None or not self.rom.is_code_bank(bank):
            return []  # table in RAM or in an unknown bank
        fixed = self.config.tables.get(site)  # number of entries given by the config
        if fixed is not None:
            self.config.used.add(("table", site))
        count = fixed if fixed is not None else (128 if indexed else 1)
        limit = 0x10000  # the table ends before the first target that lies right after it
        entries = []
        for i in range(count):
            addr = table_addr + 2 * i
            if (addr + 1) >> 13 != page:
                break  # end of the window
            if fixed is None:
                if addr + 1 >= limit:
                    break  # reached code pointed to by the table itself
                if (bank, addr & 0x1FFF) in code_bytes or (bank, (addr + 1) & 0x1FFF) in code_bytes:
                    break  # reached already known code: the table is over
            try:
                lo = self.rom.read(bank, addr & 0x1FFF)
                hi = self.rom.read(bank, (addr + 1) & 0x1FFF)
            except ValueError:
                break  # CD game: the table would go past the end of the load (program.py)
            target = lo | (hi << 8)
            if fixed is None and table_addr <= target < addr + 2:
                break  # points to the already read part of the table: not an address
            if target > table_addr:
                limit = min(limit, target)
            tbank = self._bank_for(site, target >> 13, state)
            if tbank is None or not self.rom.is_code_bank(tbank):
                continue  # entry with unknown bank (or unused): skip it, the table may go on
            point = Point(target >> 13, tbank, target & 0x1FFF)
            try:
                illegal = self._decode(point, self.rom.next_bank(tbank)).opcode.flow == FLOW_ILLEGAL
            except ValueError:
                illegal = True
            if illegal:
                if fixed is None:
                    break
                continue  # size fixed by the config: just skip the bad entry
            entries.append(point)
        return entries

    def _successors(self, p, ins, state, executed, next_bank):
        op = ins.opcode
        state = dict(state)
        # Follows LDA #n ... TAM #mask (also LDA #n / CLC / ADC $FFF5, with $FFF5 in the BIOS ROM
        # = 1st bank of the CD RAM, and INC A between one TAM and the next: this is how some games switch banks)
        a_known = state.pop("A", None)
        carry_clear = state.pop("clc", False)
        if op.mnemonic == "LDA" and op.mode == "imm":
            a_known = ins.bytes[1]
        elif op.mnemonic == "CLC":
            carry_clear = True
        elif op.mnemonic == "ADC" and op.mode == "abs" and a_known is not None and carry_clear \
                and ins.bytes[2] >= 0xE0:
            addr = ins.bytes[1] | (ins.bytes[2] << 8)
            a_known = (a_known + self.rom.read(0, addr & 0x1FFF)) & 0xFF  # window 7 = ROM bank 0
        elif op.code == 0x1A and a_known is not None:  # INC A
            a_known = (a_known + 1) & 0xFF
        elif op.code == 0x3A and a_known is not None:  # DEC A
            a_known = (a_known - 1) & 0xFF
        elif op.mnemonic == "TAM":
            for page in range(8):
                if ins.bytes[1] & (1 << page):
                    if a_known is None:
                        state.pop(page, None)
                    else:
                        state[page] = a_known
        elif op.mnemonic not in A_PRESERVING:
            a_known = None
        if a_known is not None:
            state["A"] = a_known
        if carry_clear and op.mnemonic in CARRY_PRESERVING:
            state["clc"] = True
        state[p.page] = p.bank

        out = []
        # Disc map: immediate values in A/X/Y and in the zero page; an address built from two of
        # them right before a call is an entry guess (e.g. LDA #$7D / LDX #$9C / JSR $4821;
        # LDA #lo / STA $20 / LDA #hi / STA $21 / JSR $401E)
        table_jump = None
        if getattr(self.rom, "vpages", None):
            imm = dict(state.pop("imm", ()))
            if op.flow == FLOW_JUMP_IND and op.mode == "ind" and ins.target >> 8 == 0x20:
                # JMP ($20nn) with $nn/$nn+1 read from a table (LDA t,Y / STA $nn / LDA t+1,Y / STA $nn+1)
                zp = ins.target & 0xFF
                t = imm.get(f"t{zp:02X}")
                if t is not None and imm.get(f"t{(zp + 1) & 0xFF:02X}") == t + 1:
                    table_jump = t
            if op.flow == FLOW_CALL or (op.flow == FLOW_JUMP and op.mnemonic == "JMP"):
                # (also JMP: tail call, e.g. LDX #lo / LDY #hi / JMP $E05D, interrupt vector)
                out += self._immediate_pointers(p, imm)
                imm = {}
            else:
                _track_immediates(op, ins, imm)
            if imm:
                state["imm"] = tuple(sorted(imm.items()))
        end = p.offset + op.size
        if end < 0x2000:
            fall = Point(p.page, p.bank, end)
        else:  # the next instruction is already in the next window
            fall = Point((p.page + 1) & 7, next_bank, end - 0x2000)

        if op.flow in (FLOW_NORMAL, FLOW_BRANCH):
            out.append((fall, state))
        if op.flow == FLOW_BRANCH:
            # Short branch into the next window: continues in the same routine, which goes on in whatever
            # bank is there (same rule as _next_bank: bank + 1 if there is no other information).
            branch_state = state
            if (ins.target >> 13) == ((p.page + 1) & 7) and ((p.page + 1) & 7) not in state:
                branch_state = dict(state)
                branch_state[(p.page + 1) & 7] = next_bank
            for t in self._resolve(p, ins.target, branch_state, ()):
                self.labels[t].add("loc")
                self.targets[p].add(t)
                out.append((t, state))
        elif op.flow == FLOW_JUMP:
            for t in self._resolve(p, ins.target, state, ("J",)):
                self.labels[t].add("loc")
                self.targets[p].add(t)
                out.append((t, state))
        elif op.flow == FLOW_CALL:
            for t in self._resolve(p, ins.target, state, ("C",)):
                self.labels[t].add("sub")
                self.targets[p].add(t)
                out.append((t, {}))
            # Return from the call: if the call was executed but the next instruction never was, the
            # called routine may not return there (e.g. it reads data right after the JSR). Do not follow.
            if not executed or fall in self.cov.exec:
                out.append((fall, {k: v for k, v in state.items() if k != "A"}))
            out += self._inline_pointer(p, fall, ins.target)
        elif op.flow == FLOW_JUMP_IND:
            for t in sorted(self._edges_from.get((p, "N"), ())):
                self.labels[t].add("loc")
                self.targets[p].add(t)
                out.append((t, {}))
            if table_jump is not None:
                self.indirect[p] = (table_jump, state, True)
            else:
                self.indirect[p] = (ins.target, state, op.mode == "indx")
        for k in ("C", "J", "N"):
            for d in self._edges_from.get((p, k), ()):
                if not self._is_code(d):
                    self.ram_targets.add((p, d))
        return out

    def _immediate_pointers(self, p, imm):
        """Entry guesses from the immediates before a call at `p` (_track_immediates):
        (high, low) register pairs and zero-page pairs (n, n+1). Each address that falls in the
        window of a code bank of the same load becomes an entry, if it passes _plausible."""
        regs = [r for r in ("A", "X", "Y") if r in imm]
        cands = {(imm[h] << 8) | imm[lo] for h in regs for lo in regs if h != lo}
        for k, v in imm.items():
            if k[0] == "z" and f"z{(int(k[1:], 16) + 1) & 0xFF:02X}" in imm:
                cands.add(v | (imm[f"z{(int(k[1:], 16) + 1) & 0xFF:02X}"] << 8))
        # Address taken from a table into zero-page (n, n+1): read the table (LDA t,Y / STA $20 /
        # LDA t+1,Y / STA $21 / JSR $57E6 registers a character with the routine from the table)
        for k, t in imm.items():
            if k[0] == "t" and k != "tA" and imm.get(f"t{(int(k[1:], 16) + 1) & 0xFF:02X}") == t + 1:
                cands |= self._table_words(p, t)
        out = []
        for addr in sorted(cands):
            page = addr >> 13
            banks = [p.bank] if page == p.page else self._map_banks(p, page)
            for b in banks:
                t = Point(page, b, addr & 0x1FFF)
                if t in self.code or t in self.rejected:
                    continue
                if not self._plausible(t):
                    self.rejected.add(t)
                    continue
                self.labels[t].add("sub")
                self.weak.add(t)
                out.append((t, {}))
        return out

    def _table_words(self, p, base, count=64):
        """Words of a table at `base` (bank from the load of `p`), up to the first one that does not point
        into the window of some code bank of the same load."""
        page = base >> 13
        banks = [p.bank] if page == p.page else self._map_banks(p, page)
        if len(banks) != 1:
            return set()
        words = set()
        for i in range(count):
            off = (base & 0x1FFF) + 2 * i
            if off + 1 >= 0x2000:
                break
            try:
                w = self.rom.read(banks[0], off) | (self.rom.read(banks[0], off + 1) << 8)
            except ValueError:
                break
            if w >> 13 != p.page and not self._map_banks(p, w >> 13):
                break
            words.add(w)
        return words

    def _plausible(self, start, limit=4000):
        """Does an entry guess (pointer found in data) look like code? Follows the flow within the
        same window and bank (next instruction, branches, jumps; without entering calls): every
        instruction must decode, with a valid opcode, without leaving the bank. Data read as code
        almost always hits an invalid opcode within a few dozen instructions."""
        seen = set()
        work = [start]
        while work:
            p = work.pop()
            if p in seen:
                continue
            seen.add(p)
            if len(seen) > limit:
                return False
            try:
                ins = self._decode(p, self.rom.next_bank(p.bank))
            except ValueError:
                return False
            op = ins.opcode
            # BRK (byte $00, very common in data) also fails: games hardly ever use BRK in code
            if op.flow == FLOW_ILLEGAL or op.mnemonic == "BRK" or p.offset + op.size > 0x2000:
                return False
            nxt = []
            if op.flow in (FLOW_NORMAL, FLOW_BRANCH, FLOW_CALL):
                nxt.append(p.offset + op.size)
            if op.flow in (FLOW_BRANCH, FLOW_JUMP) and ins.target is not None and ins.target >> 13 == p.page:
                nxt.append(ins.target & 0x1FFF)
            for k, off in enumerate(nxt):
                if off >= 0x2000:
                    return False
                if k == 0 and op.flow in (FLOW_NORMAL, FLOW_BRANCH, FLOW_CALL) or self._in_load(p.bank, off):
                    work.append(Point(p.page, p.bank, off))  # jump out of the load (another load): do not follow
        return True

    def _inline_pointer(self, p, fall, callee):
        """Call with a routine address right after it: `JSR x / BRA *+4 / .dw routine` (the
        "far call": game routines that put a bank in window 3 and call the routine). Only
        with the disc map; the bank comes from it (far_call) or, with no rule for `x`, is one of
        the banks of the same load that run in the routine's window."""
        if not getattr(self.rom, "vpages", None) or fall.page != p.page or fall.offset + 4 > 0x2000:
            return []
        try:
            if self.rom.read(fall.bank, fall.offset) != 0x80 or self.rom.read(fall.bank, fall.offset + 1) != 0x02:
                return []
            addr = self.rom.read(fall.bank, fall.offset + 2) | (self.rom.read(fall.bank, fall.offset + 3) << 8)
        except ValueError:
            return []
        out = []
        page = addr >> 13
        banks = self.rom.discmap.far_call_banks(self.rom, callee, p.bank) if self.rom.discmap else None
        if banks is not None:  # the far call bank only counts if it runs in the target's window
            banks = [b for b in banks if self.rom.vpages.get(b) == page] or None
        if banks is None:
            banks = [p.bank] if page == p.page else self._map_banks(p, page)
        for b in banks:
            t = Point(page, b, addr & 0x1FFF)
            self.labels[t].add("sub")
            out.append((t, {}))
        return out

    # ---- conflicts ----

    def _bytes(self, info):
        """(bank, offset) of each byte of the instruction."""
        p = info.point
        for i in range(info.ins.opcode.size):
            off = p.offset + i
            yield (p.bank, off) if off < 0x2000 else (info.next_bank, off - 0x2000)

    def _resolve_conflicts(self):
        if self.keep_overlaps:
            return
        owner = {}
        order = sorted(self.code.values(), key=lambda c: (not c.executed, c.point.bank, c.point.offset, c.point.page))
        for info in order:
            clash = None
            for b in self._bytes(info):
                other = owner.get(b)
                if other is not None and (other.point.bank, other.point.offset) != (info.point.bank, info.point.offset):
                    clash = other
                    break
            if clash is None:
                for b in self._bytes(info):
                    owner.setdefault(b, info)
                continue
            if info.executed and clash.executed:
                self.exec_conflicts.append((info.point, clash.point))
                for b in self._bytes(info):
                    owner.setdefault(b, info)
            else:
                self.conflicts.append((info.point, clash.point))
                self.dropped.add(info.point)
        for p in self.dropped:
            del self.code[p]

    # ---- summary ----

    def table_mismatches(self):
        """Executed indirect jumps whose targets (coverage) are not all in the table read from the ROM."""
        out = []
        for site, entries in sorted(self.tables.items()):
            seen = self._edges_from.get((site, "N"), set())
            missing = sorted(seen - set(entries))
            if seen and missing:
                out.append((site, missing))
        return out

    def stats(self):
        executed = sum(1 for c in self.code.values() if c.executed)
        rom_exec = {p for p in self.cov.exec if self._is_code(p)}
        missing = sorted(rom_exec - set(self.code))
        return {
            "instructions": len(self.code),
            "executed": executed,
            "static": len(self.code) - executed,
            "missing_executed": missing,
            "dropped_conflicts": len(self.conflicts),
            "executed_conflicts": len(self.exec_conflicts),
            "unresolved": len(self.unresolved),
            "ram_targets": len({d for _, d in self.ram_targets}),
            "resolved_by_candidate": len(self.by_candidate),
            "resolved_by_context": len(self.by_context),
            "tables_read": sum(1 for t in self.tables.values() if t),
            "table_targets": len({t for ts in self.tables.values() for t in ts}),
            "diverging_tables": self.table_mismatches(),
            "config_unused": self.config.unused(),
            "config_data_executed": sorted(self.data_executed),
            "code_bytes": len({b for c in self.code.values() for b in self._bytes(c)}),
        }
