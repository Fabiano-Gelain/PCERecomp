"""Disassembly of one HuC6280 instruction.

`decode(read, pc)` reads the bytes with the function `read(logical address) -> byte` and returns an
Instruction with the assembly text and, for branches/jumps/calls, the target.
The zero page lives at $2000-$20FF (logical addresses), but the text shows it as $00-$FF,
as PC-Engine assemblers do.
"""
from collections import namedtuple

from .opcodes import OPCODES

Instruction = namedtuple("Instruction", "pc opcode bytes text target")
# target: fixed target of BRA/Bxx/BBRi/BBSi/JMP abs/JSR/BSR; pointer address in JMP (abs) and
# table base in JMP (abs,X); None otherwise.


def _rel(pc_next, offset):
    if offset >= 0x80:
        offset -= 0x100
    return (pc_next + offset) & 0xFFFF


def decode(read, pc):
    op = OPCODES[read(pc)]
    raw = bytes(read((pc + i) & 0xFFFF) for i in range(op.size))
    b1 = raw[1] if op.size > 1 else 0
    w1 = (raw[1] | (raw[2] << 8)) if op.size > 2 else 0
    nxt = (pc + op.size) & 0xFFFF
    m = op.mode
    target = None

    if m in ("imp", "brk"):
        operand = ""
    elif m == "acc":
        operand = "A"
    elif m == "imm":
        operand = f"#${b1:02X}"
    elif m == "zp":
        operand = f"${b1:02X}"
    elif m == "zpx":
        operand = f"${b1:02X},X"
    elif m == "zpy":
        operand = f"${b1:02X},Y"
    elif m == "abs":
        operand = f"${w1:04X}"
        if op.flow in ("jump", "call"):
            target = w1
    elif m == "absx":
        operand = f"${w1:04X},X"
    elif m == "absy":
        operand = f"${w1:04X},Y"
    elif m == "ind":
        operand = f"(${w1:04X})"
        target = w1
    elif m == "indx":
        operand = f"(${w1:04X},X)"
        target = w1
    elif m == "izx":
        operand = f"(${b1:02X},X)"
    elif m == "izy":
        operand = f"(${b1:02X}),Y"
    elif m == "izp":
        operand = f"(${b1:02X})"
    elif m == "rel":
        target = _rel(nxt, b1)
        operand = f"${target:04X}"
    elif m == "zprel":
        target = _rel(nxt, raw[2])
        operand = f"${b1:02X},${target:04X}"
    elif m == "immzp":
        operand = f"#${b1:02X},${raw[2]:02X}"
    elif m == "immzpx":
        operand = f"#${b1:02X},${raw[2]:02X},X"
    elif m == "immabs":
        operand = f"#${b1:02X},${raw[2] | (raw[3] << 8):04X}"
    elif m == "immabsx":
        operand = f"#${b1:02X},${raw[2] | (raw[3] << 8):04X},X"
    elif m == "block":
        src = raw[1] | (raw[2] << 8)
        dst = raw[3] | (raw[4] << 8)
        length = raw[5] | (raw[6] << 8)
        operand = f"${src:04X},${dst:04X},${length:04X}"
    else:
        raise ValueError(f"unknown mode: {m}")

    text = op.mnemonic if not operand else f"{op.mnemonic} {operand}"
    return Instruction(pc, op, raw, text, target)
