"""Table of the 256 HuC6280 opcodes: name, addressing mode, size and flow kind.

Checked by tests/test_opcodes.py against the Mesen tests (size) and against the Geargrafx name
table (name). The 22 undocumented opcodes behave as 1-byte NOPs (see runtime/cpu/huc6280.c);
here they show up as "???" and are marked illegal, because real code should not use them.
"""
from collections import namedtuple

# Mode -> total instruction size in bytes
MODE_SIZE = {
    "imp": 1,     # implied: CLC
    "acc": 1,     # accumulator: ASL A
    "brk": 2,     # BRK + signature byte (ignored by the CPU)
    "imm": 2,     # immediate: LDA #$12
    "zp": 2,      # zero page ($2000-$20FF): LDA $12
    "zpx": 2,     # LDA $12,X
    "zpy": 2,     # LDX $12,Y
    "abs": 3,     # LDA $1234
    "absx": 3,    # LDA $1234,X
    "absy": 3,    # LDA $1234,Y
    "ind": 3,     # JMP ($1234)
    "indx": 3,    # JMP ($1234,X)
    "izx": 2,     # LDA ($12,X)
    "izy": 2,     # LDA ($12),Y
    "izp": 2,     # LDA ($12)
    "rel": 2,     # BNE target
    "zprel": 3,   # BBR0 $12,target
    "immzp": 3,   # TST #$12,$34
    "immzpx": 3,  # TST #$12,$34,X
    "immabs": 4,  # TST #$12,$1234
    "immabsx": 4, # TST #$12,$1234,X
    "block": 7,   # TII source,destination,length
}

# Flow kind (for code analysis)
FLOW_NORMAL = "normal"      # continues to the next instruction
FLOW_BRANCH = "branch"      # conditional branch: may continue or jump
FLOW_JUMP = "jump"          # unconditional jump with a fixed target (JMP abs, BRA)
FLOW_JUMP_IND = "jump_ind"  # jump with a target read from memory (JMP (abs), JMP (abs,X))
FLOW_CALL = "call"          # subroutine call (JSR, BSR)
FLOW_RETURN = "return"      # RTS, RTI
FLOW_BRK = "brk"            # BRK: software interrupt
FLOW_ILLEGAL = "illegal"    # undocumented opcode

Opcode = namedtuple("Opcode", "code mnemonic mode size flow")

_TABLE = """
00 BRK brk     01 ORA izx     02 SXY imp     03 ST0 imm     04 TSB zp      05 ORA zp      06 ASL zp      07 RMB0 zp
08 PHP imp     09 ORA imm     0A ASL acc     0B ??? imp     0C TSB abs     0D ORA abs     0E ASL abs     0F BBR0 zprel
10 BPL rel     11 ORA izy     12 ORA izp     13 ST1 imm     14 TRB zp      15 ORA zpx     16 ASL zpx     17 RMB1 zp
18 CLC imp     19 ORA absy    1A INC acc     1B ??? imp     1C TRB abs     1D ORA absx    1E ASL absx    1F BBR1 zprel
20 JSR abs     21 AND izx     22 SAX imp     23 ST2 imm     24 BIT zp      25 AND zp      26 ROL zp      27 RMB2 zp
28 PLP imp     29 AND imm     2A ROL acc     2B ??? imp     2C BIT abs     2D AND abs     2E ROL abs     2F BBR2 zprel
30 BMI rel     31 AND izy     32 AND izp     33 ??? imp     34 BIT zpx     35 AND zpx     36 ROL zpx     37 RMB3 zp
38 SEC imp     39 AND absy    3A DEC acc     3B ??? imp     3C BIT absx    3D AND absx    3E ROL absx    3F BBR3 zprel
40 RTI imp     41 EOR izx     42 SAY imp     43 TMA imm     44 BSR rel     45 EOR zp      46 LSR zp      47 RMB4 zp
48 PHA imp     49 EOR imm     4A LSR acc     4B ??? imp     4C JMP abs     4D EOR abs     4E LSR abs     4F BBR4 zprel
50 BVC rel     51 EOR izy     52 EOR izp     53 TAM imm     54 CSL imp     55 EOR zpx     56 LSR zpx     57 RMB5 zp
58 CLI imp     59 EOR absy    5A PHY imp     5B ??? imp     5C ??? imp     5D EOR absx    5E LSR absx    5F BBR5 zprel
60 RTS imp     61 ADC izx     62 CLA imp     63 ??? imp     64 STZ zp      65 ADC zp      66 ROR zp      67 RMB6 zp
68 PLA imp     69 ADC imm     6A ROR acc     6B ??? imp     6C JMP ind     6D ADC abs     6E ROR abs     6F BBR6 zprel
70 BVS rel     71 ADC izy     72 ADC izp     73 TII block   74 STZ zpx     75 ADC zpx     76 ROR zpx     77 RMB7 zp
78 SEI imp     79 ADC absy    7A PLY imp     7B ??? imp     7C JMP indx    7D ADC absx    7E ROR absx    7F BBR7 zprel
80 BRA rel     81 STA izx     82 CLX imp     83 TST immzp   84 STY zp      85 STA zp      86 STX zp      87 SMB0 zp
88 DEY imp     89 BIT imm     8A TXA imp     8B ??? imp     8C STY abs     8D STA abs     8E STX abs     8F BBS0 zprel
90 BCC rel     91 STA izy     92 STA izp     93 TST immabs  94 STY zpx     95 STA zpx     96 STX zpy     97 SMB1 zp
98 TYA imp     99 STA absy    9A TXS imp     9B ??? imp     9C STZ abs     9D STA absx    9E STZ absx    9F BBS1 zprel
A0 LDY imm     A1 LDA izx     A2 LDX imm     A3 TST immzpx  A4 LDY zp      A5 LDA zp      A6 LDX zp      A7 SMB2 zp
A8 TAY imp     A9 LDA imm     AA TAX imp     AB ??? imp     AC LDY abs     AD LDA abs     AE LDX abs     AF BBS2 zprel
B0 BCS rel     B1 LDA izy     B2 LDA izp     B3 TST immabsx B4 LDY zpx     B5 LDA zpx     B6 LDX zpy     B7 SMB3 zp
B8 CLV imp     B9 LDA absy    BA TSX imp     BB ??? imp     BC LDY absx    BD LDA absx    BE LDX absy    BF BBS3 zprel
C0 CPY imm     C1 CMP izx     C2 CLY imp     C3 TDD block   C4 CPY zp      C5 CMP zp      C6 DEC zp      C7 SMB4 zp
C8 INY imp     C9 CMP imm     CA DEX imp     CB ??? imp     CC CPY abs     CD CMP abs     CE DEC abs     CF BBS4 zprel
D0 BNE rel     D1 CMP izy     D2 CMP izp     D3 TIN block   D4 CSH imp     D5 CMP zpx     D6 DEC zpx     D7 SMB5 zp
D8 CLD imp     D9 CMP absy    DA PHX imp     DB ??? imp     DC ??? imp     DD CMP absx    DE DEC absx    DF BBS5 zprel
E0 CPX imm     E1 SBC izx     E2 ??? imp     E3 TIA block   E4 CPX zp      E5 SBC zp      E6 INC zp      E7 SMB6 zp
E8 INX imp     E9 SBC imm     EA NOP imp     EB ??? imp     EC CPX abs     ED SBC abs     EE INC abs     EF BBS6 zprel
F0 BEQ rel     F1 SBC izy     F2 SBC izp     F3 TAI block   F4 SET imp     F5 SBC zpx     F6 INC zpx     F7 SMB7 zp
F8 SED imp     F9 SBC absy    FA PLX imp     FB ??? imp     FC ??? imp     FD SBC absx    FE INC absx    FF BBS7 zprel
"""


def _flow(mnemonic, mode):
    if mnemonic == "???":
        return FLOW_ILLEGAL
    if mnemonic == "BRK":
        return FLOW_BRK
    if mnemonic in ("RTS", "RTI"):
        return FLOW_RETURN
    if mnemonic in ("JSR", "BSR"):
        return FLOW_CALL
    if mnemonic == "BRA" or (mnemonic == "JMP" and mode == "abs"):
        return FLOW_JUMP
    if mnemonic == "JMP":
        return FLOW_JUMP_IND
    if mode in ("rel", "zprel"):
        return FLOW_BRANCH
    return FLOW_NORMAL


def _build():
    tokens = _TABLE.split()
    table = [None] * 256
    for i in range(0, len(tokens), 3):
        code = int(tokens[i], 16)
        mnemonic, mode = tokens[i + 1], tokens[i + 2]
        if table[code] is not None:
            raise ValueError(f"opcode {code:02X} repeated")
        table[code] = Opcode(code, mnemonic, mode, MODE_SIZE[mode], _flow(mnemonic, mode))
    missing = [f"{i:02X}" for i, op in enumerate(table) if op is None]
    if missing:
        raise ValueError(f"missing opcodes: {missing}")
    return table


OPCODES = _build()
