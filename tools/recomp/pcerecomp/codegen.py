"""C generator: one C function per game function (functions.py), from an Analysis.

Each instruction becomes `hu_pre` / `hu_op_XX(operands)` / `hu_post` (runtime/cpu/huc6280_recomp.h),
which runs exactly the same code as the interpreter. Control flow becomes C:
  - next instruction / branch / JMP into the function -> `goto` (or falls through to the next line);
  - any exit from the function (JSR, RTS, RTI, JMP out, indirect jump, TAM, unknown
    target) -> `return`, and the dispatcher (pce_step) finds the next function by the REAL bank.
So a wrong guess by the analysis never becomes wrong behavior: at worst, it goes back to the
dispatcher (or to the interpreter).

Banks: the dispatcher only calls the function after checking the bank of the entry instruction's
window. Inside the function, the bank of a window only changes through TAM, and after TAM it always
goes back to the dispatcher. So it is enough to check the bank when the flow moves to ANOTHER window,
and the bank of the next window for instructions that cross the end of the window.

Output: PARTS files rc_partNN.c with the functions (of similar size, to compile in parallel) and rc_table.c
with the lookup table recomp_lookup().
"""
import os

from .functions import _fallthrough
from .opcodes import FLOW_BRANCH, FLOW_CALL, FLOW_ILLEGAL, FLOW_JUMP, FLOW_NORMAL

# Instructions whose operand the game rewrites and that may read it from RAM at run time (codegen._live_operand)
_LIVE_FLOWS = (FLOW_NORMAL, FLOW_BRANCH, FLOW_JUMP, FLOW_CALL)

PAGES = range(8)
PARTS = 24  # rc_partNN.c files (CMakeLists.txt uses the same number)


_WRITE_MNEMONICS = {"STA", "STX", "STY", "STZ", "TRB", "TSB", "TII", "TDD", "TIN", "TIA", "TAI"}
_RMW_MNEMONICS = {"INC", "DEC", "ASL", "LSR", "ROL", "ROR"}


def _writes_memory(op):
    """Does the instruction write to memory (outside the stack)? The stack (PHA, JSR...) is left out:
    there is no game code in the stack page."""
    if op.mnemonic in _WRITE_MNEMONICS:
        return True
    if op.mnemonic in _RMW_MNEMONICS:
        return op.mode != "acc"
    return op.mnemonic[:3] in ("RMB", "SMB")


def _gen_page(phys):
    """Index of the 256-byte RAM chunk in ram_gen (pce.h): CD RAM 0-255, work RAM 256+."""
    bank, off = phys >> 13, phys & 0x1FFF
    if 0x80 <= bank <= 0x87:
        return (bank - 0x80) * 32 + (off >> 8)
    return 256 + (off >> 8)


def point_name(p):
    return f"{p.page}_{p.bank:02X}_{p.offset:04X}"


def logical(p):
    return (p.page << 13) | p.offset


def func_name(p):
    return f"f_{point_name(p)}"


def label(p):
    return f"L_{point_name(p)}"


class CodeGen:
    def __init__(self, analysis, functions, title=""):
        self.an = analysis
        self.functions = functions
        self.title = title
        self.owner = self._owners()

    def _owners(self):
        """For each point, the function the dispatcher calls when the PC is there."""
        owner = {}
        for entry in sorted(self.functions):
            owner[entry] = entry  # the entry itself always leads to its own function
        for entry in sorted(self.functions, key=lambda e: -len(self.functions[e].body)):
            for p in self._body(entry).values():  # only points the function actually generates
                owner.setdefault(p, entry)
        return owner

    # ---- one function ----

    def _body(self, entry):
        """Body without repeated logical addresses (same address in different banks) and without points
        of another load (CD games: the function's signature only covers its own load; going to another
        load goes back to the dispatcher)."""
        rom = self.an.rom
        key = rom.load_key(entry.bank)
        body = {}
        for p in sorted(self.functions[entry].body, key=lambda q: (q.bank != entry.bank, q)):
            if rom.load_key(p.bank) == key:
                body.setdefault(logical(p), p)
        return body

    def _real(self, bank):
        return self.an.rom.real_bank(bank)

    def _is_virtual(self, entry):
        return entry.bank >= 0x100

    def _live_operand(self, p, info):
        """Instruction in RAM that the game modifies (the coverage saw it run with bytes differing from the
        disc): if it is an ordinary instruction (no branch), the operands are read from RAM at run time and
        are left out of the signature; the opcode is still checked. Some games change the value of
        ST1/ST2 #imm this way in loops that draw to VRAM (and the whole function would fall back to the
        interpreter). Also branches with a fixed target: a game may change the target of a JSR (e.g. a
        drawing routine) before calling it; the target is read from RAM at run time and the dispatcher
        continues from there (the JMP and the conditional branch check where they went)."""
        op = info.ins.opcode
        return (p.bank >= 0x100 and op.flow in _LIVE_FLOWS and op.size > 1 and p.offset + op.size <= 0x2000
                and ((p.page, self._real(p.bank), p.offset) in self.an.cov.modified
                     or (p.page, self._real(p.bank), p.offset) in getattr(self.an, "modified", ())))

    def _sig_page(self, p):
        """256-byte RAM chunk (physical address >> 8) where the instruction at p starts."""
        return ((self._real(p.bank) << 13) | p.offset) >> 8

    def _sig(self, entry, p):
        return f"sig_{point_name(entry)}_{self._sig_page(p):04X}"

    def _sig_check(self, entry, src, dst):
        """Code in RAM: when moving to another 256-byte chunk, check its signature (the game may have
        put another load there only)."""
        if not self._is_virtual(entry) or (src is not None and self._sig_page(src) == self._sig_page(dst)):
            return ""
        return f"if (!recomp_ram_sig_ok(c->ctx, &{self._sig(entry, dst)})) return; "

    def _goto(self, entry, src, dst, body_points):
        """Code to continue at `dst` (the PC already points there)."""
        if dst not in body_points:
            return "return;"
        check = self._sig_check(entry, src, dst)
        if dst.page != src.page:
            return f"if (c->mpr[{dst.page}] != 0x{self._real(dst.bank):02X}) return; {check}goto {label(dst)};"
        return f"{check}goto {label(dst)};"

    def function_c(self, entry):
        body = self._body(entry)
        body_points = set(body.values())
        order = [body[a] for a in sorted(body)]
        out = [f"void {func_name(entry)}(HuC6280 *c)", "{"]
        # Code in RAM: only valid if the RAM holds the bytes of this load; check the 256-byte chunk where
        # it starts (and each chunk it enters later): a part of the function replaced by another load
        # does not stop the rest from running
        out.append("    switch (c->pc) {")
        for p in order:
            out.append(f"    case 0x{logical(p):04X}: {self._sig_check(entry, None, p)}goto {label(p)};")
        out += ["    default: return;", "    }"]

        for i, p in enumerate(order):
            info = self.an.code[p]
            ins = info.ins
            op = ins.opcode
            b = list(ins.bytes[1:]) + [0] * (7 - len(ins.bytes))
            nxt = _fallthrough(info)
            out.append(f"{label(p)}: /* {logical(p):04X}  {ins.text} */")
            if p.offset + op.size > 0x2000:  # crosses the end of the window: check the next bank
                out.append(f"    if (c->mpr[{(p.page + 1) & 7}] != 0x{self._real(info.next_bank):02X}) return;")
            args = ", ".join(f"0x{x:02X}" for x in b[:6])
            live = self._live_operand(p, info)
            if live:
                base = (self._real(p.bank) << 13) | p.offset
                args = ", ".join(f"recomp_ram_byte(c->ctx, 0x{base + 1 + k:06X})" if k < op.size - 1 else "0x00"
                                 for k in range(6))
            out.append(f"    {{ int t_ = hu_pre(c, 0x{op.code:02X}, 0x{logical(nxt):04X}); int x_; "
                       f"hu_op_{op.code:02X}(c, {args}, t_, &x_); if (hu_post(c, x_)) return; }}")
            if self._is_virtual(entry) and _writes_memory(op):
                # The game may have just modified its own code: go back to the dispatcher, which checks.
                out.append(f"    if (recomp_ram_sig_changed(c->ctx, &{self._sig(entry, p)})) return;")

            following = order[i + 1] if i + 1 < len(order) else None
            flow = op.flow
            if op.mnemonic == "TAM":
                out.append("    return; /* banks changed: the dispatcher decides */")
                continue
            if flow == FLOW_BRANCH:
                targets = sorted(self.an.targets.get(p, ()))
                tgt = next((t for t in targets if logical(t) == ins.target), None)
                if tgt is not None:
                    out.append(f"    if (c->pc == 0x{ins.target:04X}) {{ {self._goto(entry, p, tgt, body_points)} }}")
                else:
                    out.append(f"    if (c->pc != 0x{logical(nxt):04X}) return;")
                if live and tgt is not None:  # offset read from RAM: neither the analysis target nor the next one
                    out.append(f"    if (c->pc != 0x{logical(nxt):04X}) return;")
            if flow in (FLOW_NORMAL, FLOW_BRANCH, FLOW_ILLEGAL):
                if nxt == following and nxt.page == p.page:
                    check = self._sig_check(entry, p, nxt)
                    if check:
                        out.append("    " + check.strip())
                    continue  # falls straight into the next generated instruction
                out.append("    " + self._goto(entry, p, nxt, body_points))
            elif flow == FLOW_JUMP:
                targets = sorted(self.an.targets.get(p, ()))
                tgt = next((t for t in targets if logical(t) == ins.target), None)
                if tgt is None:
                    out.append("    return;")
                else:
                    out.append(f"    if (c->pc != 0x{ins.target:04X}) return;")
                    out.append("    " + self._goto(entry, p, tgt, body_points))
            else:  # call, return, indirect jump, BRK: the dispatcher continues
                out.append("    return;")
        out.append("}")
        return "\n".join(out) + "\n"

    # ---- signature (code in RAM) ----

    def signature_c(self, entry):
        """Bytes of the function's instructions, by physical address (huc6280_recomp.h, HuRamSig): one
        signature per 256-byte chunk where instructions start (the whole instruction, even past the chunk)."""
        rom = self.an.rom
        by_page = {}
        for p in self._body(entry).values():
            info = self.an.code[p]
            found = by_page.setdefault(self._sig_page(p), {})
            live = self._live_operand(p, info)
            for i in range(info.ins.opcode.size):
                if live and i > 0:
                    continue  # operand read from RAM at run time (_live_operand)
                off = p.offset + i
                bank = p.bank if off < 0x2000 else info.next_bank
                phys = (self._real(bank) << 13) | (off & 0x1FFF)
                found[phys] = rom.read(bank, off & 0x1FFF)
        lines = []
        for sp, found in sorted(by_page.items()):
            runs = []
            for phys in sorted(found):
                if runs and runs[-1][0] + len(runs[-1][1]) == phys:
                    runs[-1][1].append(found[phys])
                else:
                    runs.append([phys, [found[phys]]])
            pages = sorted({_gen_page(phys) for phys in found})
            name = f"{point_name(entry)}_{sp:04X}"
            data = [b for _, bs in runs for b in bs]
            lines.append(f"static const uint8_t sb_{name}[] = {{")
            for i in range(0, len(data), 24):
                lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 24]) + ",")
            lines.append("};")
            lines.append(f"static const HuRamRun sr_{name}[] = {{")
            pos = 0
            for phys, bs in runs:
                lines.append(f"    {{0x{phys:06X}, {len(bs)}, sb_{name} + {pos}}},")
                pos += len(bs)
            lines.append("};")
            lines.append(f"static const uint16_t sp_{name}[] = {{{', '.join(str(x) for x in pages)}}};")
            lines.append(f"static HuRamSig sig_{name} = {{sr_{name}, {len(runs)}, sp_{name}, {len(pages)}, 0, 0}};")
        return "\n".join(lines) + "\n"

    # ---- files ----

    def _is_cd(self):
        return hasattr(self.an.rom, "disc")

    def _bios(self, entry):
        """In a CD game, banks $00-$1F are the BIOS: they do not become C (the BIOS routines are the ones
        in runtime/bios, and the generated code must not contain anything from the BIOS)."""
        return self._is_cd() and self._real(entry.bank) < 0x20

    def write(self, outdir):
        os.makedirs(outdir, exist_ok=True)
        self.functions = {e: v for e, v in self.functions.items() if not self._bios(e)}
        self.owner = {p: e for p, e in self.owner.items() if not self._bios(e)}
        header = (f"/* {self.title}: recompiled code generated by tools/recomp (do NOT commit: derived from the game). */\n"
                  '#include "cpu/huc6280_recomp.h"\n\n')
        # PARTS files of similar size (the compiler builds several at once), with the functions of
        # each load together; the number is fixed so CMake knows the names
        rom = self.an.rom
        order = sorted(self.functions, key=lambda e: (str(rom.load_key(e.bank)), e))
        sizes = {e: len(self._body(e)) for e in order}
        total = sum(sizes.values()) or 1
        parts = [[] for _ in range(PARTS)]
        done = 0
        for entry in order:
            parts[min(PARTS - 1, done * PARTS // total)].append(entry)
            done += sizes[entry]
        files = []
        for n, entries in enumerate(parts):
            path = os.path.join(outdir, f"rc_part{n:02d}.c")
            with open(path, "w", encoding="utf-8") as f:
                f.write(header)
                f.write("#if defined(_MSC_VER)\n#pragma warning(disable: 4102) /* unreferenced label */\n#endif\n\n")
                for entry in entries:
                    if self._is_virtual(entry):
                        f.write(self.signature_c(entry))
                    f.write(self.function_c(entry))
                    f.write("\n")
            files.append(path)

        path = os.path.join(outdir, "rc_table.c")
        with open(path, "w", encoding="utf-8") as f:
            f.write(header)
            for entry in sorted(self.functions):
                f.write(f"void {func_name(entry)}(HuC6280 *c);\n")
            # By (window, REAL bank): at each offset, the candidate functions. In ROM there is only one;
            # in RAM (CD game) there may be one per load: a selector tries each one (the function returns
            # without executing anything if the RAM does not hold its load).
            groups = {}
            for p, entry in self.owner.items():
                if p in self.an.code:
                    cands = groups.setdefault((p.page, self._real(p.bank)), {}).setdefault(p.offset, [])
                    if entry not in cands:
                        cands.append(entry)
            selectors = {}
            for (page, bank), table in sorted(groups.items()):
                for off, cands in sorted(table.items()):
                    if len(cands) > 1:
                        key = tuple(sorted(cands))
                        if key not in selectors:
                            selectors[key] = f"s_{len(selectors)}"
                            f.write(f"\nstatic void {selectors[key]}(HuC6280 *c)\n{{\n    uint64_t cy = c->cycles;\n")
                            for i, e in enumerate(key):
                                f.write(f"    {func_name(e)}(c);\n")
                                if i + 1 < len(key):
                                    f.write("    if (c->cycles != cy) return;\n")
                            f.write("}\n")
            for (page, bank), table in sorted(groups.items()):
                f.write(f"\nstatic const HuRecompFn t_{page}_{bank:02X}[0x2000] = {{\n")
                for off in sorted(table):
                    cands = table[off]
                    fn = func_name(cands[0]) if len(cands) == 1 else selectors[tuple(sorted(cands))]
                    f.write(f"    [0x{off:04X}] = {fn},\n")
                f.write("};\n")
            # CD game: the code comes from the disc (each function checks the RAM); the "ROM" is the BIOS,
            # which may not even be present (synthetic BIOS). 0 = do not check the ROM.
            crc = 0 if self._is_cd() else self.an.rom.crc32
            f.write(f"\nconst uint32_t recomp_rom_crc32 = 0x{crc:08X}u;\n")
            f.write("\nHuRecompFn recomp_lookup(int page, int bank, int offset)\n{\n")
            f.write("    switch ((page << 8) | bank) {\n")
            for page, bank in sorted(groups):
                f.write(f"    case 0x{(page << 8) | bank:03X}: return t_{page}_{bank:02X}[offset];\n")
            f.write("    default: return 0;\n    }\n}\n")
        files.append(path)
        return files

    def stats(self):
        return {
            "functions": len(self.functions),
            "instructions": sum(len(self._body(e)) for e in self.functions),
            "table_points": sum(1 for p in self.owner if p in self.an.code),
        }
