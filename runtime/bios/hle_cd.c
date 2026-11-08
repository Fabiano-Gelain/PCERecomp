/*
 * C BIOS: CD reading and CD audio (cd_*), and the conversation with the CD drive (SCSI) that they
 * use. See hle.h and, for the hardware, runtime/cd/scsi.h.
 *
 * The conversation with the drive waits at the drive's pace (signals at $1800: BSY $80, REQ $40, MSG $20, C/D $10,
 * I/O $08). Each BIOS wait loop is a point here (one iteration = one pseudo-instruction), and the internal
 * parts that wait are "called" as in the BIOS (return address on the stack), so interrupts
 * can happen in the middle, as on the console. Phases ($1800 & $F8): $D0 command, $C8 data coming from the
 * drive, $D8 status, $F8 message.
 *
 * BIOS variables:
 *   $224C-$2255 command to send; $2256-$225F short reply (TOC, Q subchannel, error)
 *   $2260-$2267 copy of $20F8-$20FF (to repeat the operation after an error)
 *   $2273 which base to use (0 = $2274-$2276, 1 = $2277-$2279: starting sector added to the request)
 *   $227A last phase seen; $227B status; $227C error code (sense request); $227D ...
 *   $229B the drive did not respond; $229C mode of the last CD audio play; $22A4 background command
 *   $2241 VBlank counter (timeout of the waits); $2280/$2281 sectors to read; $2268 bank to restore
 */
#include <stdio.h>

#include "bios/alu.h"
#include "bios/hle.h"
#include "bios/internal.h"
#include "bios/routines.h"
#include "cd/origin.h"

#define A (pce->cpu.a)
#define X (pce->cpu.x)
#define Y (pce->cpu.y)

static uint8_t m(Pce *pce, uint16_t a) { return hle_rd(pce, a); }
static void mw(Pce *pce, uint16_t a, uint8_t v) { hle_wr(pce, a, v); }
/* Disc byte (just read from the data port) written to RAM: the coverage records where it came from, as it does
 * with the real BIOS's LDA/STA (cd/origin.h) */
static void mw_data(Pce *pce, uint16_t a, uint8_t v)
{
    uint32_t tag = pce->origin ? origin_data_tag(pce) : 0;
    mw(pce, a, v);
    if (pce->origin)
        origin_mark(pce->origin, huc6280_phys(&pce->cpu, a), tag);
}
static uint8_t z(Pce *pce, uint8_t n) { return hle_rd(pce, ZP(n)); }
static void zw(Pce *pce, uint8_t n, uint8_t v) { hle_wr(pce, ZP(n), v); }

static void call(Pce *pce, uint16_t target, uint16_t ret) { bios_call(pce, target, ret); }
static void go(Pce *pce, uint16_t pc) { pce->cpu.pc = pc; }

/* TST #imm,abs: Z = (imm & mem) == 0; N and V = bits 7 and 6 of memory. Returns imm & mem. */
uint8_t bios_tst(Pce *pce, uint8_t imm, uint16_t addr)
{
    uint8_t v = m(pce, addr);
    HuC6280 *c = &pce->cpu;
    c->p = (uint8_t)((c->p & ~(HU_FLAG_Z | HU_FLAG_V | HU_FLAG_N)) | ((imm & v) ? 0 : HU_FLAG_Z) |
                     (v & (HU_FLAG_V | HU_FLAG_N)));
    return (uint8_t)(imm & v);
}

/* TSB/TRB with A: N and V from the original memory, Z from the result. */
/* TSB/TRB abs: reads and writes at the start of the instruction, like the CPU, and spends its 7 cycles (on
 * memory with hle_ram_free, none) */
static void tsb_trb(Pce *pce, uint16_t addr, int set)
{
    HuC6280 *c = &pce->cpu;
    uint32_t phys = huc6280_phys(c, addr);
    uint8_t v = c->read(c->ctx, phys);
    uint8_t r = set ? (uint8_t)(v | A) : (uint8_t)(v & ~A);
    c->p = (uint8_t)((c->p & ~(HU_FLAG_Z | HU_FLAG_V | HU_FLAG_N)) | (r ? 0 : HU_FLAG_Z) |
                     (v & (HU_FLAG_V | HU_FLAG_N)));
    c->write(c->ctx, phys, r);
    if (!(pce->hle && pce->hle->ram_free && c->mpr[addr >> 13] != 0xFF))
        huc6280_add_cycles(c, 7);
}

void bios_tsb(Pce *pce, uint16_t addr, uint8_t a) { A = a; tsb_trb(pce, addr, 1); }
void bios_trb(Pce *pce, uint16_t addr, uint8_t a) { A = a; tsb_trb(pce, addr, 0); }

/* BIT #imm */
static void bit_imm(Pce *pce, uint8_t imm)
{
    HuC6280 *c = &pce->cpu;
    c->p = (uint8_t)((c->p & ~(HU_FLAG_Z | HU_FLAG_V | HU_FLAG_N)) | ((A & imm) ? 0 : HU_FLAG_Z) |
                     (imm & (HU_FLAG_V | HU_FLAG_N)));
}

/* Current drive phase, recorded at $227A (LDA $1800; AND #$F8; STA $227A) */
static void pla_plx(Pce *pce, uint8_t a, uint8_t x)
{
    A = a;
    X = alu_nz(pce, x);
}

/* TMA/TAM on a window given by a bit (like the mini-routine the BIOS builds at $228E) */
int bios_window_of_bit(uint8_t bit)
{
    int i = 0;
    while (i < 7 && !(bit & (1 << i)))
        i++;
    return i;
}

/* ---- Parts without waits ---- */

/* F107: clears the command ($224D-$2255) */
void bios_clear_command(Pce *pce)
{
    uint8_t x = X;
    for (X = 9; X; X--)
        mw(pce, (uint16_t)(0x224C + X), 0);
    pla_plx(pce, A, x);
}

/* F112: clears the short reply ($2256-$225F) */
static void clear_reply(Pce *pce)
{
    uint8_t x = X;
    X = 9;
    for (;;) {
        mw(pce, (uint16_t)(0x2256 + X), 0);
        X--;
        if (X == 0xFF)
            break;
    }
    pla_plx(pce, A, x);
}

/* F322 / F331: saves / restores $20F8-$20FF to/from $2260-$2267 */
void bios_save_args(Pce *pce)
{
    uint8_t x = X, a = A;
    for (int i = 7; i >= 0; i--)
        mw(pce, (uint16_t)(0x2260 + i), z(pce, (uint8_t)(0xF8 + i)));
    pla_plx(pce, a, x);
}

void bios_restore_args(Pce *pce)
{
    uint8_t x = X, a = A;
    for (int i = 7; i >= 0; i--)
        zw(pce, (uint8_t)(0xF8 + i), m(pce, (uint16_t)(0x2260 + i)));
    pla_plx(pce, a, x);
}

/* F11D: adds to the requested sector ($20FC-$20FE) the base chosen by $2273 */
void bios_add_base(Pce *pce)
{
    uint8_t t = m(pce, 0x2273);
    A = alu_asl(pce, t);
    alu_setc(pce, 0);
    A = alu_adc(pce, A, m(pce, 0x2273));
    Y = A;
    alu_setc(pce, 0);
    A = alu_adc(pce, z(pce, 0xFE), m(pce, (uint16_t)(0x2276 + Y)));
    zw(pce, 0xFE, A);
    A = alu_adc(pce, z(pce, 0xFD), m(pce, (uint16_t)(0x2275 + Y)));
    zw(pce, 0xFD, A);
    A = alu_adc(pce, z(pce, 0xFC), m(pce, (uint16_t)(0x2274 + Y)));
    zw(pce, 0xFC, A);
}

/* F340: 3 bytes from $20F8+X to the command at $224C+Y */
void bios_put3(Pce *pce, uint8_t x, uint8_t y)
{
    X = x;
    Y = y;
    for (int i = 0; i < 3; i++) {
        A = alu_nz(pce, z(pce, (uint8_t)(0xF8 + X + i)));
        mw(pce, (uint16_t)(0x224C + Y + i), A);
    }
}

/* EDB8: requested bytes ($20F8/$20F9) -> 2 KB sectors in $2280 (rounds up). Returns the BIOS
 * time (cycles, including the RTS) */
static int bytes_to_sectors(Pce *pce)
{
    int t = 2 + 5 + 4 + 5 + 4 + 5 + 7; /* LDA #, STA, LDA, STA, LDA, LDA, RTS */
    mw(pce, 0x2281, 0x01);
    uint8_t a = z(pce, 0xF9);
    for (int i = 0; i < 3; i++) {
        a = alu_lsr(pce, a);
        t += 2 + (alu_c(pce) ? 2 + 5 : 4); /* LSR, BCC (STZ) */
        if (alu_c(pce))
            mw(pce, 0x2281, 0);
    }
    mw(pce, 0x2280, a);
    A = alu_nz(pce, z(pce, 0xF8));
    t += A ? 2 + 5 : 4;
    if (A)
        mw(pce, 0x2281, 0);
    A = alu_nz(pce, m(pce, 0x2281));
    t += A ? 4 : 2 + 7;
    if (!A)
        mw(pce, 0x2280, alu_nz(pce, (uint8_t)(m(pce, 0x2280) + 1)));
    return t;
}

/* F350: VRAM write address = ($20FA) */
void bios_vram_write_address(Pce *pce)
{
    A = 0x01;
    mw(pce, 0x2272, A);
    A = z(pce, 0xFA);
    mw(pce, 0x0002, A);
    A = z(pce, 0xFB);
    mw(pce, 0x0003, A);
}

void bios_cd_vdc_select(Pce *pce, uint8_t reg)
{
    A = reg;
    zw(pce, 0xF7, reg);
    mw(pce, 0x0000, reg);
}

/* E619: BCD -> binary (C = 1 if it was not BCD; X preserved) */
static uint8_t bcd_to_bin(Pce *pce, uint8_t a)
{
    uint8_t x = X;
    X = a;
    uint8_t hi = (uint8_t)(a & 0xF0);
    alu_cmp(pce, hi, 0xA0);
    if (!alu_c(pce)) {
        uint8_t t = alu_lsr(pce, hi);
        zw(pce, 0xEC, t);
        t = alu_lsr(pce, alu_lsr(pce, t));
        alu_setc(pce, 0);
        t = alu_adc(pce, t, z(pce, 0xEC));
        zw(pce, 0xEC, t);
        uint8_t lo = (uint8_t)(X & 0x0F);
        alu_cmp(pce, lo, 0x0A);
        if (!alu_c(pce)) {
            alu_setc(pce, 0);
            X = alu_adc(pce, lo, z(pce, 0xEC));
            alu_setc(pce, 0);
        }
    }
    uint8_t r = X;
    X = alu_nz(pce, x);
    A = r;
    return r;
}

/* ---- Minute/second/frame -> sector conversion (BIOS bank 1, $CC89) ---- */

/* $EC:$ED:$EE (24 bits, $EC high) shifted left by 1 */
static void shl_ec(Pce *pce)
{
    uint8_t ee = z(pce, 0xEE), ed = z(pce, 0xED), ec = z(pce, 0xEC);
    zw(pce, 0xEE, alu_asl(pce, ee));
    zw(pce, 0xED, alu_rol(pce, ed));
    zw(pce, 0xEC, alu_rol(pce, ec));
}

/* $F8:$F9:$FA += $EC:$ED:$EE */
static void add_ec(Pce *pce)
{
    alu_setc(pce, 0);
    A = alu_adc(pce, z(pce, 0xFA), z(pce, 0xEE));
    zw(pce, 0xFA, A);
    A = alu_adc(pce, z(pce, 0xF9), z(pce, 0xED));
    zw(pce, 0xF9, A);
    A = alu_adc(pce, z(pce, 0xF8), z(pce, 0xEC));
    zw(pce, 0xF8, A);
}

static void shl_add(Pce *pce)
{
    shl_ec(pce);
    add_ec(pce);
}

static void load_ec(Pce *pce, uint8_t a)
{
    zw(pce, 0xEE, a);
    zw(pce, 0xED, 0);
    zw(pce, 0xEC, 0);
}

/* $2256-$2258 (BCD) -> sector in $20F8 (high) - $20FA: (min*60 + sec)*75 + frame - 150 */
void bios_msf_to_lba(Pce *pce)
{
    for (int i = 0; i < 3; i++) {
        uint16_t a = (uint16_t)(0x2256 + i);
        mw(pce, a, bcd_to_bin(pce, m(pce, a)));
    }
    zw(pce, 0xF8, 0);
    zw(pce, 0xF9, 0);
    zw(pce, 0xFA, 0);
    load_ec(pce, m(pce, 0x2256));
    shl_ec(pce);
    for (int i = 0; i < 4; i++)
        shl_add(pce);
    load_ec(pce, m(pce, 0x2257));
    add_ec(pce);
    zw(pce, 0xEC, z(pce, 0xF8));
    zw(pce, 0xED, z(pce, 0xF9));
    zw(pce, 0xEE, z(pce, 0xFA));
    shl_add(pce);
    shl_ec(pce);
    shl_add(pce);
    shl_ec(pce);
    shl_ec(pce);
    shl_add(pce);
    alu_setc(pce, 1);
    A = alu_sbc(pce, m(pce, 0x2258), 0x96);
    zw(pce, 0xEE, A);
    A = alu_sbc(pce, 0, 0);
    zw(pce, 0xED, A);
    A = alu_sbc(pce, 0, 0);
    zw(pce, 0xEC, A);
    add_ec(pce);
}

/* ======== Conversation with the drive (internal parts, with waits) ======== */

/* One instruction per pseudo-instruction: these are wait loops, and interrupts (VBlank, timer, CD) enter in
 * the middle of them at the exact point, as on the real BIOS. Each scsi_op call performs the BIOS instruction at
 * the PC's address, like the CPU: the accesses at its start (spending no time of their own), then its time
 * (the CPU's k_cycles; branch taken +2), and the interrupt decided with the requests from the start (or from
 * after the VDC wait). */

#define SCSI_READ_SLOW 0xEA52  /* data with REQ/ACK one by one into ($20FA), $20F8/$20F9 bytes */
#define SCSI_READ_FAST 0xEA90  /* data via $1808 (automatic acknowledge) into ($20FA),Y */
#define SCSI_READ_VRAM 0xEAD5  /* data via $1808 straight into VRAM */
#define SCSI_READ_VRAM_SLOW 0xEB2D /* data one by one (REQ/ACK) into VRAM, alternating $0002/$0003 */
#define SCSI_DRAIN 0xEB77      /* discards leftover data */

static uint8_t rd(Pce *pce, uint16_t a) { return hle_peek(pce, a); }
static void wr(Pce *pce, uint16_t a, uint8_t v) { hle_poke(pce, a, v); }

/* Disc byte written to memory (coverage: see mw_data) */
static void wr_data(Pce *pce, uint16_t a, uint8_t v)
{
    uint32_t tag = pce->origin ? origin_data_tag(pce) : 0;
    wr(pce, a, v);
    if (pce->origin)
        origin_mark(pce->origin, huc6280_phys(&pce->cpu, a), tag);
}

static uint16_t ptr_fa(Pce *pce)
{
    return (uint16_t)(rd(pce, ZP(0xFA)) | (rd(pce, ZP(0xFB)) << 8));
}

/* End of the instruction: its time and the next PC */
static void next(Pce *pce, int cycles, uint16_t pc) { hle_next(pce, cycles, pc); }

/* 2-byte conditional branch at `pc` */
static void branch(Pce *pce, uint16_t pc, int taken, uint16_t to)
{
    next(pce, taken ? 4 : 2, taken ? to : (uint16_t)(pc + 2));
}

static void tst(Pce *pce, uint8_t imm, uint16_t a)
{
    HuC6280 *c = &pce->cpu;
    uint8_t v = rd(pce, a);
    c->p = (uint8_t)((c->p & ~(HU_FLAG_Z | HU_FLAG_V | HU_FLAG_N)) | ((imm & v) ? 0 : HU_FLAG_Z) |
                     (v & (HU_FLAG_V | HU_FLAG_N)));
}

static int flag(Pce *pce, uint8_t f) { return (pce->cpu.p & f) != 0; }

static void scsi_op(Pce *pce)
{
    HuC6280 *c = &pce->cpu;
    uint16_t pc = c->pc;
    hle_irq_sample(pce);
    switch (pc) {
    /* E8FA: restarts the drive (bit 1 of $1804 for a while) */
    case 0xE8FA: A = alu_nz(pce, 0x02); next(pce, 2, 0xE8FC); break;
    case 0xE8FC: bios_tsb(pce, 0x1804, A); go(pce, 0xE8FF); break; /* (TSB on hardware already spends the 7 cycles) */
    case 0xE8FF: Y = alu_nz(pce, 0x0A); next(pce, 2, 0xE901); break;
    case 0xE901: X = alu_nz(pce, 0x3B); next(pce, 2, 0xE903); break;
    case 0xE903: case 0xE913: X = alu_nz(pce, (uint8_t)(X - 1)); next(pce, 2, (uint16_t)(pc + 1)); break;
    case 0xE904: case 0xE914: branch(pce, pc, X != 0, (uint16_t)(pc - 1)); break;
    case 0xE906: Y = alu_nz(pce, (uint8_t)(Y - 1)); next(pce, 2, 0xE907); break;
    case 0xE907: branch(pce, pc, Y != 0, 0xE901); break;
    case 0xE909: A = alu_nz(pce, rd(pce, 0x1804)); next(pce, 5, 0xE90C); break;
    case 0xE90C: A = alu_nz(pce, (uint8_t)(A & 0xFD)); next(pce, 2, 0xE90E); break;
    case 0xE90E: wr(pce, 0x1804, A); next(pce, 5, 0xE911); break;
    case 0xE911: X = alu_nz(pce, 0x77); next(pce, 2, 0xE913); break;

    /* E917: sends the command at $224C (waits for a background command to finish) */
    case 0xE917: A = alu_nz(pce, rd(pce, 0x22A4)); next(pce, 5, 0xE91A); break;
    case 0xE91A: branch(pce, pc, A != 0, 0xE917); break;
    case 0xE91C: wr(pce, 0x229B, 0); next(pce, 5, 0xE91F); break;
    /* E91F: selects the drive; busy: releases what was pending and tries again */
    case 0xE91F: A = alu_nz(pce, 0x81); next(pce, 2, 0xE921); break;
    case 0xE921: case 0xE941: case 0xE995: wr(pce, 0x1801, A); next(pce, 5, (uint16_t)(pc + 3)); break;
    case 0xE924: case 0xE953: case 0xEA34: tst(pce, 0x80, 0x1800); next(pce, 8, (uint16_t)(pc + 4)); break;
    case 0xE944: case 0xE94D: case 0xEA0A: case 0xEA2C: case 0xEA40:
        tst(pce, 0x40, 0x1800);
        next(pce, 8, (uint16_t)(pc + 4));
        break;
    case 0xE928: branch(pce, pc, flag(pce, HU_FLAG_Z), 0xE95B); break;
    case 0xE92A: A = alu_nz(pce, 0x60); next(pce, 2, 0xE92C); break;
    case 0xE92C: bios_trb(pce, 0x1802, A); go(pce, 0xE92F); break;
    case 0xE92F: case 0xE95B: wr(pce, 0x1800, A); next(pce, 5, (uint16_t)(pc + 3)); break;
    case 0xE932: X = alu_nz(pce, 0x95); next(pce, 2, 0xE934); break;
    /* E934: pause (double loop) and releases the bus */
    case 0xE934: A = alu_nz(pce, 0xED); next(pce, 2, 0xE936); break;
    case 0xE936: case 0xE968: A = alu_nz(pce, (uint8_t)(A - 1)); next(pce, 2, (uint16_t)(pc + 1)); break;
    case 0xE937: case 0xE969: branch(pce, pc, A != 0, (uint16_t)(pc - 1)); break;
    case 0xE939: case 0xE93A: case 0xE93B: case 0xE96B: case 0xE998: case 0xE999: case 0xE99A: case 0xE99B:
    case 0xEAF5: case 0xEAF6: case 0xEAF7: case 0xEAF8: case 0xEAF9: case 0xEAFA: case 0xEAFB: case 0xEAFC:
    case 0xEB94: case 0xEB95: case 0xEB96: case 0xEB97: case 0xEB98: case 0xEB99: case 0xEB9A: case 0xEB9B:
        next(pce, 2, (uint16_t)(pc + 1)); /* NOP */
        break;
    case 0xE93C: case 0xE96C: case 0xEAAA: case 0xEAC7: case 0xEAE9: case 0xEB17: case 0xEB8B: case 0xEB9F:
        X = alu_nz(pce, (uint8_t)(X - 1)); /* DEX */
        next(pce, 2, (uint16_t)(pc + 1));
        break;
    case 0xE93D: branch(pce, pc, X != 0, 0xE934); break;
    case 0xE93F: A = alu_nz(pce, 0xFF); next(pce, 2, 0xE941); break;
    case 0xE948: branch(pce, pc, flag(pce, HU_FLAG_Z), 0xE91F); break;
    case 0xE94A: case 0xE99C: case 0xEB4C: /* JSR $EA3E */
        next(pce, 7, 0);
        bios_call(pce, 0xEA3E, (uint16_t)(pc + 3));
        break;
    case 0xE951: case 0xE957: branch(pce, pc, !flag(pce, HU_FLAG_Z), pc == 0xE951 ? 0xE94A : 0xE94D); break;
    case 0xE959: case 0xE96F: next(pce, 4, 0xE91F); break;
    case 0xE95E: case 0xE974: X = 0; next(pce, 2, (uint16_t)(pc + 1)); break; /* CLX */
    /* E95F: waits for the drive to request the command (REQ), with a timeout of 256 iterations */
    case 0xE962: A = alu_nz(pce, (uint8_t)(A & 0x40)); next(pce, 2, 0xE964); break;
    case 0xE964: branch(pce, pc, A != 0, 0xE971); break;
    case 0xE966: A = alu_nz(pce, 0x5A); next(pce, 2, 0xE968); break;
    case 0xE96D: branch(pce, pc, X != 0, 0xE95F); break;
    case 0xE971: wr(pce, 0x227A, 0); next(pce, 5, 0xE974); break;

    /* Current drive phase, recorded at $227A: LDA $1800; AND #$F8; STA $227A */
    case 0xE95F: case 0xE975: case 0xE9BE: case 0xE9F6: case 0xEA12: case 0xEA52: case 0xEA7E: case 0xEA90:
    case 0xEACC: case 0xEAD5: case 0xEB1C: case 0xEB35: case 0xEB66: case 0xEB77:
        A = alu_nz(pce, rd(pce, 0x1800));
        next(pce, 5, (uint16_t)(pc + 3));
        break;
    case 0xE978: case 0xE9C1: case 0xE9F9: case 0xEA15: case 0xEA55: case 0xEA81: case 0xEA93: case 0xEACF:
    case 0xEAD8: case 0xEB1F: case 0xEB38: case 0xEB69: case 0xEB7A:
        A = alu_nz(pce, (uint8_t)(A & 0xF8));
        next(pce, 2, (uint16_t)(pc + 2));
        break;
    case 0xE97A: case 0xE9C3: case 0xE9FB: case 0xEA17: case 0xEA27: case 0xEA57: case 0xEA83: case 0xEA95:
    case 0xEAD1: case 0xEADA: case 0xEB21: case 0xEB3A: case 0xEB6B: case 0xEB7C:
        wr(pce, 0x227A, A);
        next(pce, 5, (uint16_t)(pc + 3));
        break;
    case 0xE981: case 0xEA86: case 0xEB24: case 0xEB6E: A = alu_nz(pce, (uint8_t)(A & 0xB8)); next(pce, 2, (uint16_t)(pc + 2)); break;

    /* Comparisons (CMP #) and the branches that follow them */
    case 0xE97D: alu_cmp(pce, A, 0xD0); next(pce, 2, 0xE97F); break;
    case 0xE983: alu_cmp(pce, A, 0x98); next(pce, 2, 0xE985); break;
    case 0xE987: case 0xEA88: case 0xEB26: case 0xEB70: alu_cmp(pce, A, 0x88); next(pce, 2, (uint16_t)(pc + 2)); break;
    case 0xE98B: alu_cmp(pce, A, 0x80); next(pce, 2, 0xE98D); break;
    case 0xE9AC: alu_cmp(pce, A, 0xD9); next(pce, 2, 0xE9AE); break;
    case 0xE9B3: alu_cmp(pce, A, 0x01); next(pce, 2, 0xE9B5); break;
    case 0xE9B7: alu_cmp(pce, A, 0x02); next(pce, 2, 0xE9B9); break;
    case 0xE9D2: alu_cmp(pce, A, 0xFE); next(pce, 2, 0xE9D4); break;
    case 0xE9FE: case 0xEA1E: case 0xEA5E: case 0xEA9C: case 0xEAE1: case 0xEB41: case 0xEB83:
        alu_cmp(pce, A, 0xD8);
        next(pce, 2, (uint16_t)(pc + 2));
        break;
    case 0xEA1A: alu_cmp(pce, A, 0xF8); next(pce, 2, 0xEA1C); break;
    case 0xEA5A: case 0xEA98: case 0xEADD: case 0xEB3D: case 0xEB7F: alu_cmp(pce, A, 0xC8); next(pce, 2, (uint16_t)(pc + 2)); break;
    case 0xE97F: branch(pce, pc, flag(pce, HU_FLAG_Z), 0xE991); break;
    case 0xE985: case 0xE989: case 0xE98D: branch(pce, pc, flag(pce, HU_FLAG_Z), 0xE9A1); break;
    case 0xE98F: case 0xE99F: next(pce, 4, 0xE975); break;
    /* E975: the command bytes, while the drive is in the command phase */
    case 0xE991: A = alu_nz(pce, rd(pce, (uint16_t)(0x224C + X))); next(pce, 5, 0xE994); break;
    case 0xE994: X = alu_nz(pce, (uint8_t)(X + 1)); next(pce, 2, 0xE995); break;

    /* E9A1: command sent. In the background (or CD audio that does not wait), returns right away; otherwise
     * waits for the drive to respond (REQ), with a timeout of about 4 seconds. */
    case 0xE9A1: A = alu_nz(pce, rd(pce, 0x22A4)); next(pce, 5, 0xE9A4); break;
    case 0xE9A4: A = alu_nz(pce, (uint8_t)(A | rd(pce, 0x2248))); next(pce, 5, 0xE9A7); break;
    case 0xE9A7: branch(pce, pc, !flag(pce, HU_FLAG_Z), 0xE9DB); break;
    case 0xE9A9: A = alu_nz(pce, rd(pce, 0x224C)); next(pce, 5, 0xE9AC); break;
    case 0xE9AE: branch(pce, pc, !flag(pce, HU_FLAG_Z), 0xE9BB); break;
    case 0xE9B0: A = alu_nz(pce, rd(pce, 0x224D)); next(pce, 5, 0xE9B3); break;
    case 0xE9B5: case 0xE9B9: branch(pce, pc, flag(pce, HU_FLAG_Z), 0xE9DB); break;
    case 0xE9BB: X = alu_nz(pce, rd(pce, 0x2241)); next(pce, 5, 0xE9BE); break;
    case 0xE9C6: bit_imm(pce, 0x40); next(pce, 2, 0xE9C8); break;
    case 0xE9C8: branch(pce, pc, !flag(pce, HU_FLAG_Z), 0xE9DB); break;
    case 0xE9CA: A = alu_nz(pce, X); next(pce, 2, 0xE9CB); break;
    case 0xE9CB: case 0xEB05: case 0xEB57: alu_setc(pce, 1); next(pce, 2, (uint16_t)(pc + 1)); break;
    case 0xE9CC: A = alu_sbc(pce, A, rd(pce, 0x2241)); next(pce, 5, 0xE9CF); break;
    case 0xE9CF: A = alu_nz(pce, (uint8_t)(A ^ 0xFF)); next(pce, 2, 0xE9D1); break;
    case 0xE9D1: A = alu_nz(pce, (uint8_t)(A + 1)); next(pce, 2, 0xE9D2); break;
    case 0xE9D4: branch(pce, pc, !alu_c(pce), 0xE9BE); break;
    case 0xE9D6: A = alu_nz(pce, 0x01); next(pce, 2, 0xE9D8); break;
    case 0xE9D8: wr(pce, 0x229B, A); next(pce, 5, 0xE9DB); break;
    case 0xE9DB: case 0xE9E9: case 0xE9F2: case 0xEA3D: case 0xEA4B: case 0xEA51: case 0xEA8F: case 0xEAD4:
    case 0xEB2C: case 0xEB76: case 0xEBA7: case 0xE916:
        next(pce, 7, 0);
        hle_rts(pce);
        break;

    /* E9DC: status and message at the end of the command */
    case 0xE9DC: A = alu_nz(pce, rd(pce, 0x229B)); next(pce, 5, 0xE9DF); break;
    case 0xE9DF: branch(pce, pc, flag(pce, HU_FLAG_Z), 0xE9EA); break;
    case 0xE9E1: wr(pce, 0x229B, 0); next(pce, 5, 0xE9E4); break;
    case 0xE9E4: next(pce, 7, 0); bios_call(pce, 0xE8FA, 0xE9E7); break; /* the drive did not respond: restarts */
    case 0xE9E7: A = alu_nz(pce, 0x06); next(pce, 2, 0xE9E9); break;
    case 0xE9EA: next(pce, 7, 0); bios_call(pce, 0xE9F3, 0xE9ED); break;
    case 0xE9ED: branch(pce, pc, !flag(pce, HU_FLAG_Z), 0xE9F2); break;
    case 0xE9EF: wr(pce, 0x227C, 0); next(pce, 5, 0xE9F2); break;
    case 0xE9F3: wr(pce, 0x227B, 0); next(pce, 5, 0xE9F6); break;
    case 0xEA00: branch(pce, pc, !flag(pce, HU_FLAG_Z), 0xE9F6); break;
    case 0xEA02: case 0xEA24: case 0xEA64: case 0xEB47: A = alu_nz(pce, rd(pce, 0x1801)); next(pce, 5, (uint16_t)(pc + 3)); break;
    case 0xEA05: wr(pce, 0x227B, A); next(pce, 5, 0xEA08); break;
    case 0xEA08: case 0xEA2A: case 0xEA3E: /* BSR $EA4C */
        next(pce, 8, 0);
        bios_call(pce, 0xEA4C, (uint16_t)(pc + 2));
        break;
    case 0xEA10: case 0xEA32: /* BSR $EA46 */
        next(pce, 8, 0);
        bios_call(pce, 0xEA46, (uint16_t)(pc + 2));
        break;
    case 0xEA0E: case 0xEA30: case 0xEA38: case 0xEA44: branch(pce, pc, !flag(pce, HU_FLAG_Z), (uint16_t)(pc - 4)); break;
    case 0xEA1C: branch(pce, pc, flag(pce, HU_FLAG_Z), 0xEA24); break;
    case 0xEA20: branch(pce, pc, !flag(pce, HU_FLAG_Z), 0xEA12); break;
    case 0xEA22: next(pce, 4, 0xE9F6); break;
    case 0xEA3A: A = alu_nz(pce, rd(pce, 0x227B)); next(pce, 5, 0xEA3D); break;
    /* EA3E: ACK on; waits for REQ to drop; ACK off (the TSB/TRB on hardware already spend the 7 cycles) */
    case 0xEA46: case 0xEA4C: A = alu_nz(pce, 0x80); next(pce, 2, (uint16_t)(pc + 2)); break;
    case 0xEA48: bios_trb(pce, 0x1802, A); go(pce, 0xEA4B); break;
    case 0xEA4E: bios_tsb(pce, 0x1802, A); go(pce, 0xEA51); break;

    /* EA52: data one by one with REQ/ACK, into ($20FA) */
    case 0xEA5C: branch(pce, pc, flag(pce, HU_FLAG_Z), 0xEA64); break;
    case 0xEA60: branch(pce, pc, flag(pce, HU_FLAG_Z), 0xEA7E); break;
    case 0xEA62: next(pce, 4, SCSI_READ_SLOW); break;
    case 0xEA7C: branch(pce, pc, !flag(pce, HU_FLAG_Z), SCSI_READ_SLOW); break;
    case 0xEA67: wr_data(pce, ptr_fa(pce), A); next(pce, 7, 0xEA69); break;
    case 0xEA69: next(pce, 8, 0); bios_call(pce, 0xEA3E, 0xEA6B); break;
    case 0xEA6B: case 0xEA6F: case 0xEAC5: { /* INC $FA / INC $FB */
        uint16_t a = ZP(pc == 0xEA6B ? 0xFA : 0xFB);
        wr(pce, a, alu_nz(pce, (uint8_t)(rd(pce, a) + 1)));
        next(pce, 6, (uint16_t)(pc + 2));
        break;
    }
    case 0xEA6D: branch(pce, pc, !flag(pce, HU_FLAG_Z), 0xEA71); break;
    case 0xEA71: case 0xEAA2: case 0xEAB5: case 0xEB03: case 0xEB55: /* LDA $F8 */
        A = alu_nz(pce, rd(pce, ZP(0xF8)));
        next(pce, 4, (uint16_t)(pc + 2));
        break;
    case 0xEA73: case 0xEAB7: branch(pce, pc, !flag(pce, HU_FLAG_Z), (uint16_t)(pc + 4)); break;
    case 0xEA75: case 0xEAB9: wr(pce, ZP(0xF9), alu_nz(pce, (uint8_t)(rd(pce, ZP(0xF9)) - 1))); next(pce, 6, (uint16_t)(pc + 2)); break;
    case 0xEA77: case 0xEABB: A = alu_nz(pce, (uint8_t)(A - 1)); next(pce, 2, (uint16_t)(pc + 1)); break;
    case 0xEA78: case 0xEABC: case 0xEB08: case 0xEB5A: wr(pce, ZP(0xF8), A); next(pce, 4, (uint16_t)(pc + 2)); break;
    case 0xEA7A: case 0xEAA4: case 0xEABE: A = alu_nz(pce, (uint8_t)(A | rd(pce, ZP(0xF9)))); next(pce, 4, (uint16_t)(pc + 2)); break;
    case 0xEA8A: branch(pce, pc, !flag(pce, HU_FLAG_Z), 0xEA8F); break;
    case 0xEA8C: next(pce, 4, SCSI_DRAIN); break;

    /* EA90: data via $1808, 2 KB (8 x 256) per phase iteration */
    case 0xEA9A: branch(pce, pc, flag(pce, HU_FLAG_Z), 0xEAA2); break;
    case 0xEA9E: branch(pce, pc, flag(pce, HU_FLAG_Z), 0xEAD4); break;
    case 0xEAA0: next(pce, 4, SCSI_READ_FAST); break;
    case 0xEAA6: case 0xEAC0: branch(pce, pc, flag(pce, HU_FLAG_Z), 0xEACC); break;
    case 0xEAA8: case 0xEAE7: X = alu_nz(pce, 0x03); next(pce, 2, (uint16_t)(pc + 2)); break;
    case 0xEAAB: case 0xEAEA: case 0xEB8C: branch(pce, pc, X != 0, (uint16_t)(pc - 1)); break;
    case 0xEAAD: case 0xEAEC: case 0xEB8E: Y = 0; next(pce, 2, (uint16_t)(pc + 1)); break; /* CLY */
    case 0xEAAE: case 0xEB8F: X = alu_nz(pce, 0x08); next(pce, 2, (uint16_t)(pc + 2)); break;
    case 0xEAB0: case 0xEAEF: case 0xEAFD: case 0xEB91: A = alu_nz(pce, rd(pce, 0x1808)); next(pce, 5, (uint16_t)(pc + 3)); break;
    case 0xEAB3: wr_data(pce, (uint16_t)(ptr_fa(pce) + Y), A); next(pce, 7, 0xEAB5); break;
    case 0xEAC2: case 0xEB9C: Y = alu_nz(pce, (uint8_t)(Y + 1)); next(pce, 2, (uint16_t)(pc + 1)); break;
    case 0xEAC3: case 0xEAC8: branch(pce, pc, !flag(pce, HU_FLAG_Z), 0xEAB0); break;
    case 0xEACA: next(pce, 4, SCSI_READ_FAST); break;

    /* EAD5: data via $1808 straight into VRAM (byte pairs) */
    case 0xEADF: branch(pce, pc, flag(pce, HU_FLAG_Z), 0xEAE7); break;
    case 0xEAE3: case 0xEB12: branch(pce, pc, flag(pce, HU_FLAG_Z), 0xEB1C); break;
    case 0xEAE5: case 0xEB1A: next(pce, 4, SCSI_READ_VRAM); break;
    case 0xEAED: X = alu_nz(pce, 0x04); next(pce, 2, 0xEAEF); break;
    case 0xEAF2: wr(pce, 0x0002, A); next(pce, 5, 0xEAF5); break;
    case 0xEB00: wr(pce, 0x0003, A); next(pce, 5, 0xEB03); break;
    case 0xEB06: A = alu_sbc(pce, A, 0x02); next(pce, 2, 0xEB08); break;
    case 0xEB58: A = alu_sbc(pce, A, 0x01); next(pce, 2, 0xEB5A); break;
    case 0xEB0A: case 0xEB5C: A = alu_nz(pce, rd(pce, ZP(0xF9))); next(pce, 4, (uint16_t)(pc + 2)); break;
    case 0xEB0C: case 0xEB5E: A = alu_sbc(pce, A, 0x00); next(pce, 2, (uint16_t)(pc + 2)); break;
    case 0xEB0E: case 0xEB60: wr(pce, ZP(0xF9), A); next(pce, 4, (uint16_t)(pc + 2)); break;
    case 0xEB10: case 0xEB62: A = alu_nz(pce, (uint8_t)(A | rd(pce, ZP(0xF8)))); next(pce, 4, (uint16_t)(pc + 2)); break;
    case 0xEB14: Y = alu_nz(pce, (uint8_t)(Y - 1)); next(pce, 2, 0xEB15); break;
    case 0xEB15: case 0xEB18: branch(pce, pc, !flag(pce, HU_FLAG_Z), 0xEAEF); break;
    case 0xEB28: branch(pce, pc, !flag(pce, HU_FLAG_Z), 0xEB2C); break;
    case 0xEB2A: case 0xEB74: next(pce, 4, SCSI_DRAIN); break;

    /* EB2D: data one by one (REQ/ACK) into VRAM, alternating $0002/$0003 */
    case 0xEB2D: A = alu_nz(pce, 0x02); next(pce, 2, 0xEB2F); break;
    case 0xEB2F: case 0xEB53: wr(pce, ZP(0xFA), A); next(pce, 4, (uint16_t)(pc + 2)); break;
    case 0xEB31: A = alu_nz(pce, 0x00); next(pce, 2, 0xEB33); break;
    case 0xEB33: wr(pce, ZP(0xFB), A); next(pce, 4, 0xEB35); break;
    case 0xEB3F: branch(pce, pc, flag(pce, HU_FLAG_Z), 0xEB47); break;
    case 0xEB43: branch(pce, pc, flag(pce, HU_FLAG_Z), 0xEB76); break;
    case 0xEB72: branch(pce, pc, !flag(pce, HU_FLAG_Z), 0xEB76); break;
    case 0xEB45: next(pce, 4, 0xEB35); break;
    case 0xEB4A: wr_data(pce, ptr_fa(pce), A); next(pce, 7, 0xEB4C); break;
    case 0xEB4F: A = alu_nz(pce, rd(pce, ZP(0xFA))); next(pce, 4, 0xEB51); break;
    case 0xEB51: A = alu_nz(pce, (uint8_t)(A ^ 0x01)); next(pce, 2, 0xEB53); break;
    case 0xEB64: branch(pce, pc, !flag(pce, HU_FLAG_Z), 0xEB35); break;

    /* EB77: discards whatever the drive still sends */
    case 0xEB81: branch(pce, pc, flag(pce, HU_FLAG_Z), 0xEB89); break;
    case 0xEB85: branch(pce, pc, flag(pce, HU_FLAG_Z), 0xEBA4); break;
    case 0xEB87: case 0xEBA2: next(pce, 4, SCSI_DRAIN); break;
    case 0xEB89: X = alu_nz(pce, 0x05); next(pce, 2, 0xEB8B); break;
    case 0xEB9D: case 0xEBA0: branch(pce, pc, !flag(pce, HU_FLAG_Z), 0xEB91); break;
    case 0xEBA4: A = alu_nz(pce, rd(pce, 0x227A)); next(pce, 5, 0xEBA7); break;
    default:
        fprintf(stderr, "C BIOS: scsi_op has no instruction $%04X\n", pc);
        next(pce, 2, pc);
        break;
    }
}

/* The addresses of all the instructions in E8FA-EBA7 (scsi_op) */
static const uint16_t scsi_ops[] = {
    0xE8FA, 0xE8FC, 0xE8FF, 0xE901, 0xE903, 0xE904, 0xE906, 0xE907, 0xE909, 0xE90C, 0xE90E, 0xE911, 0xE913,
    0xE914, 0xE916, 0xE917, 0xE91A, 0xE91C, 0xE91F, 0xE921, 0xE924, 0xE928, 0xE92A, 0xE92C, 0xE92F, 0xE932,
    0xE934, 0xE936, 0xE937, 0xE939, 0xE93A, 0xE93B, 0xE93C, 0xE93D, 0xE93F, 0xE941, 0xE944, 0xE948, 0xE94A,
    0xE94D, 0xE951, 0xE953, 0xE957, 0xE959, 0xE95B, 0xE95E, 0xE95F, 0xE962, 0xE964, 0xE966, 0xE968, 0xE969,
    0xE96B, 0xE96C, 0xE96D, 0xE96F, 0xE971, 0xE974, 0xE975, 0xE978, 0xE97A, 0xE97D, 0xE97F, 0xE981, 0xE983,
    0xE985, 0xE987, 0xE989, 0xE98B, 0xE98D, 0xE98F, 0xE991, 0xE994, 0xE995, 0xE998, 0xE999, 0xE99A, 0xE99B,
    0xE99C, 0xE99F, 0xE9A1, 0xE9A4, 0xE9A7, 0xE9A9, 0xE9AC, 0xE9AE, 0xE9B0, 0xE9B3, 0xE9B5, 0xE9B7, 0xE9B9,
    0xE9BB, 0xE9BE, 0xE9C1, 0xE9C3, 0xE9C6, 0xE9C8, 0xE9CA, 0xE9CB, 0xE9CC, 0xE9CF, 0xE9D1, 0xE9D2, 0xE9D4,
    0xE9D6, 0xE9D8, 0xE9DB, 0xE9DC, 0xE9DF, 0xE9E1, 0xE9E4, 0xE9E7, 0xE9E9, 0xE9EA, 0xE9ED, 0xE9EF, 0xE9F2,
    0xE9F3, 0xE9F6, 0xE9F9, 0xE9FB, 0xE9FE, 0xEA00, 0xEA02, 0xEA05, 0xEA08, 0xEA0A, 0xEA0E, 0xEA10, 0xEA12,
    0xEA15, 0xEA17, 0xEA1A, 0xEA1C, 0xEA1E, 0xEA20, 0xEA22, 0xEA24, 0xEA27, 0xEA2A, 0xEA2C, 0xEA30, 0xEA32,
    0xEA34, 0xEA38, 0xEA3A, 0xEA3D, 0xEA3E, 0xEA40, 0xEA44, 0xEA46, 0xEA48, 0xEA4B, 0xEA4C, 0xEA4E, 0xEA51,
    0xEA52, 0xEA55, 0xEA57, 0xEA5A, 0xEA5C, 0xEA5E, 0xEA60, 0xEA62, 0xEA64, 0xEA67, 0xEA69, 0xEA6B, 0xEA6D,
    0xEA6F, 0xEA71, 0xEA73, 0xEA75, 0xEA77, 0xEA78, 0xEA7A, 0xEA7C, 0xEA7E, 0xEA81, 0xEA83, 0xEA86, 0xEA88,
    0xEA8A, 0xEA8C, 0xEA8F, 0xEA90, 0xEA93, 0xEA95, 0xEA98, 0xEA9A, 0xEA9C, 0xEA9E, 0xEAA0, 0xEAA2, 0xEAA4,
    0xEAA6, 0xEAA8, 0xEAAA, 0xEAAB, 0xEAAD, 0xEAAE, 0xEAB0, 0xEAB3, 0xEAB5, 0xEAB7, 0xEAB9, 0xEABB, 0xEABC,
    0xEABE, 0xEAC0, 0xEAC2, 0xEAC3, 0xEAC5, 0xEAC7, 0xEAC8, 0xEACA, 0xEACC, 0xEACF, 0xEAD1, 0xEAD4, 0xEAD5,
    0xEAD8, 0xEADA, 0xEADD, 0xEADF, 0xEAE1, 0xEAE3, 0xEAE5, 0xEAE7, 0xEAE9, 0xEAEA, 0xEAEC, 0xEAED, 0xEAEF,
    0xEAF2, 0xEAF5, 0xEAF6, 0xEAF7, 0xEAF8, 0xEAF9, 0xEAFA, 0xEAFB, 0xEAFC, 0xEAFD, 0xEB00, 0xEB03, 0xEB05,
    0xEB06, 0xEB08, 0xEB0A, 0xEB0C, 0xEB0E, 0xEB10, 0xEB12, 0xEB14, 0xEB15, 0xEB17, 0xEB18, 0xEB1A, 0xEB1C,
    0xEB1F, 0xEB21, 0xEB24, 0xEB26, 0xEB28, 0xEB2A, 0xEB2C, 0xEB2D, 0xEB2F, 0xEB31, 0xEB33, 0xEB35, 0xEB38,
    0xEB3A, 0xEB3D, 0xEB3F, 0xEB41, 0xEB43, 0xEB45, 0xEB47, 0xEB4A, 0xEB4C, 0xEB4F, 0xEB51, 0xEB53, 0xEB55,
    0xEB57, 0xEB58, 0xEB5A, 0xEB5C, 0xEB5E, 0xEB60, 0xEB62, 0xEB64, 0xEB66, 0xEB69, 0xEB6B, 0xEB6E, 0xEB70,
    0xEB72, 0xEB74, 0xEB76, 0xEB77, 0xEB7A, 0xEB7C, 0xEB7F, 0xEB81, 0xEB83, 0xEB85, 0xEB87, 0xEB89, 0xEB8B,
    0xEB8C, 0xEB8E, 0xEB8F, 0xEB91, 0xEB94, 0xEB95, 0xEB96, 0xEB97, 0xEB98, 0xEB99, 0xEB9A, 0xEB9B, 0xEB9C,
    0xEB9D, 0xEB9F, 0xEBA0, 0xEBA2, 0xEBA4, 0xEBA7, 0,
};

/* ---- Errors (E224) and "sense" request (F06C) ---- */

static void cd_error(Pce *pce)
{
    alu_cmp(pce, A, 0x06);
    int c6 = A == 0x06;
    if (!c6)
        alu_cmp(pce, A, 0x02);
    if (c6 || A != 0x02) {
        alu_setc(pce, 0);
        hle_cycles(pce, 20);
        hle_rts(pce);
        return;
    }
    Y = 9;
    for (;;) {
        A = m(pce, (uint16_t)(0x224C + Y));
        mw(pce, (uint16_t)(0x2291 + Y), A);
        Y = alu_nz(pce, (uint8_t)(Y - 1));
        if (Y == 0xFF)
            break;
    }
    hle_cycles(pce, 160);
    call(pce, 0xF06C, 0xE23A);
}

static void cd_error_sense(Pce *pce)
{
    static const uint8_t give_up[] = {0x0B, 0x0D, 0x1C, 0x1D, 0x20, 0x21, 0x22, 0x25, 0x2A, 0x2C};
    static const uint8_t order[] = {0x0B, 0x0D, 0x11, 0x15, 0x16, 0x1C, 0x1D, 0x20, 0x21, 0x22, 0x25, 0x2A, 0x2C};
    A = alu_nz(pce, m(pce, 0x227C));
    int set = 0;
    if (A) {
        alu_cmp(pce, A, 0x04);
        if (A == 0x04) {
            mw(pce, 0x2247, A);
        } else {
            for (unsigned i = 0; i < sizeof(order); i++) {
                alu_cmp(pce, A, order[i]);
                if (A == order[i])
                    break;
            }
            for (unsigned i = 0; i < sizeof(give_up); i++)
                if (A == give_up[i])
                    set = 1;
        }
    }
    alu_setc(pce, set);
    hle_cycles(pce, 60);
    hle_rts(pce);
}

static void cd_sense(Pce *pce)
{
    clear_reply(pce);
    bios_clear_command(pce);
    A = 0x03;
    mw(pce, 0x224C, A);
    A = 0x0A;
    mw(pce, 0x2250, A);
    hle_cycles(pce, 250);
    call(pce, SCSI_SEND, 0xF07F);
}

static void cd_sense_sent(Pce *pce)
{
    alu_cmp(pce, A, 0xC8);
    if (A == 0xC8) {
        zw(pce, 0xFA, 0x56);
        zw(pce, 0xFB, 0x22);
        zw(pce, 0xF8, 0x0A);
        zw(pce, 0xF9, 0x00);
        A = 0x0A;
        hle_cycles(pce, 30);
        call(pce, SCSI_READ_SLOW, 0xF094);
        return;
    }
    hle_cycles(pce, 2 + 4 + 7);
    call(pce, SCSI_STATUS, 0xF097);
}

static void cd_sense_status(Pce *pce)
{
    hle_cycles(pce, 7);
    call(pce, SCSI_STATUS, 0xF097);
}

static void cd_sense_done(Pce *pce)
{
    uint8_t a = A;
    alu_cmp(pce, a, 0);
    if (a == 0) {
        mw(pce, 0x227D, m(pce, 0x2258));
        mw(pce, 0x227C, m(pce, 0x225F));
    }
    A = alu_nz(pce, a);
    hle_cycles(pce, 40);
    hle_rts(pce);
}

/* ======== Routines ======== */

/* ---- cd_read ($E009): reads sectors. $20FC-$20FE sector (added to the base), $20FF destination:
 *   0 = RAM at ($20FA), $20F8/$20F9 bytes; 1 = RAM at ($20FA), $20F8 sectors;
 *   2-6 = memory banks in window $20FF starting from bank $20FA, $20F8 sectors;
 *   $FE/$FF = VRAM at ($20FA), $20F8 sectors (two ways of transferring). ---- */

static void cd_read_retry(Pce *pce);
static void cd_read_mpr(Pce *pce);
static void cd_read_mpr_window(Pce *pce);

static void cd_read(Pce *pce)
{
    mw(pce, 0x2273, 0);
    bios_save_args(pce);
    hle_cycles(pce, 157); /* JMP, STZ, JSR $F322 (saves $20F8-$20FF) */
    go(pce, 0xEC24);
}

static void cd_read_start(Pce *pce)
{
    bios_clear_command(pce);
    A = 0x08;
    mw(pce, 0x224C, A);
    bios_add_base(pce);
    bios_put3(pce, 4, 1);
    A = z(pce, 0xFF);
    alu_cmp(pce, A, 0x02);
    if (A >= 0x02) {
        alu_cmp(pce, A, 0xFF);
        if (A == 0xFF) {
            A = alu_nz(pce, z(pce, 0xF8));
            if (!A) {
                A = alu_nz(pce, 0x22);
                hle_cycles(pce, 330);
                hle_rts(pce);
                return;
            }
            mw(pce, 0x2250, A);
            hle_cycles(pce, 340);
            call(pce, SCSI_SEND, 0xEC9C);
            return;
        }
        alu_cmp(pce, A, 0xFE);
        if (A == 0xFE) {
            bytes_to_sectors(pce);
            A = alu_nz(pce, m(pce, 0x2280));
            if (!A) {
                A = alu_nz(pce, 0x22);
                hle_cycles(pce, 380);
                hle_rts(pce);
                return;
            }
            mw(pce, 0x2250, A);
            hle_cycles(pce, 390);
            call(pce, SCSI_SEND, 0xECE2);
            return;
        }
        cd_read_mpr(pce);
        return;
    }
    /* EC4A-EC5E: sectors ($20F8) or bytes (JSR $EDB8) */
    int t;
    A = alu_nz(pce, z(pce, 0xFF));
    if (A) {
        A = z(pce, 0xF8);
        mw(pce, 0x2280, A);
        A = alu_asl(pce, alu_asl(pce, alu_asl(pce, A)));
        zw(pce, 0xF9, A);
        zw(pce, 0xF8, 0);
        t = 4 + 2 + 4 + 5 + 3 * 2 + 4 + 4 + 4;
    } else {
        t = 4 + 4 + 7 + bytes_to_sectors(pce);
    }
    A = alu_nz(pce, m(pce, 0x2280));
    if (!A) {
        A = alu_nz(pce, 0x22);
        hle_cycles(pce, 262 + t);
        hle_rts(pce);
        return;
    }
    mw(pce, 0x2250, A);
    hle_cycles(pce, 272 + t); /* 305 with sectors (real BIOS) */
    call(pce, SCSI_SEND, 0xEC6A);
}

static void cd_read_sent(Pce *pce)
{
    alu_cmp(pce, A, 0xC8);
    if (A == 0xC8) {
        hle_cycles(pce, 11);
        call(pce, SCSI_READ_FAST, 0xEC71);
        return;
    }
    hle_cycles(pce, 2 + 4 + 7);
    call(pce, SCSI_STATUS, 0xEC7B);
}

static void cd_read_data_done(Pce *pce)
{
    alu_cmp(pce, A, 0xC8);
    if (A == 0xC8) {
        hle_cycles(pce, 11);
        call(pce, SCSI_DRAIN, 0xEC78);
        return;
    }
    hle_cycles(pce, 2 + 4 + 7);
    call(pce, SCSI_STATUS, 0xEC7B);
}

static void cd_read_status(Pce *pce)
{
    hle_cycles(pce, 7);
    call(pce, SCSI_STATUS, 0xEC7B);
}

/* Status received: error -> CD_ERROR; ok but data missing -> again; ok -> returns with A = 0 */
static void cd_read_check(Pce *pce, uint16_t error_ret)
{
    alu_cmp(pce, A, 0x00);
    if (A) {
        hle_cycles(pce, 11);
        call(pce, CD_ERROR, error_ret);
        return;
    }
    A = alu_nz(pce, (uint8_t)(z(pce, 0xF8) | z(pce, 0xF9)));
    if (A) {
        /* CMP, BEQ, LDA, ORA, BNE (at $EC89, the JMP $ED0A) */
        hle_cycles(pce, 2 + 4 + 4 + 4 + 4 + (error_ret == 0xEC82 ? 4 : 0));
        go(pce, 0xED0A);
        return;
    }
    hle_cycles(pce, 2 + 4 + 4 + 4 + 2 + 7);
    hle_rts(pce);
}

static void cd_read_checked(Pce *pce) { cd_read_check(pce, 0xEC82); }

static void cd_read_error_done(Pce *pce)
{
    /* BCC (taken: at $EC82, plus the JMP $ED0A) or RTS */
    hle_cycles(pce, alu_c(pce) ? 2 + 7 : 4 + (pce->cpu.pc == 0xEC82 ? 4 : 0));
    if (alu_c(pce))
        hle_rts(pce);
    else
        go(pce, 0xED0A);
}

static void cd_read_retry(Pce *pce)
{
    bios_restore_args(pce);
    A = m(pce, 0x227C);
    alu_cmp(pce, A, 0x10);
    if (A >= 0x10) {
        A = 0x01;
        mw(pce, 0x2273, A);
    }
    hle_cycles(pce, 150);
    go(pce, 0xEC24);
}

/* $20FF = $FF: VRAM, fast transfer */
static void cd_read_vram_sent(Pce *pce)
{
    alu_cmp(pce, A, 0xC8);
    if (A != 0xC8) {
        hle_cycles(pce, 2 + 4 + 7);
        call(pce, SCSI_STATUS, 0xECC3);
        return;
    }
    A = alu_asl(pce, alu_asl(pce, alu_asl(pce, z(pce, 0xF8))));
    zw(pce, 0xF9, A);
    zw(pce, 0xF8, 0);
    bios_cd_vdc_select(pce, 0x00);
    bios_vram_write_address(pce);
    bios_cd_vdc_select(pce, 0x02);
    hle_cycles(pce, 90);
    call(pce, SCSI_READ_VRAM, 0xECBD);
}

static void cd_read_vram_done(Pce *pce)
{
    mw(pce, 0x2272, 0);
    hle_cycles(pce, 12);
    call(pce, SCSI_STATUS, 0xECC3);
}

static void cd_read_vram_checked(Pce *pce) { cd_read_check(pce, 0xECCA); }

/* $20FF = $FE: VRAM, one byte at a time */
static void cd_read_vram_slow_sent(Pce *pce)
{
    alu_cmp(pce, A, 0xC8);
    if (A != 0xC8) {
        hle_cycles(pce, 2 + 4 + 7);
        call(pce, SCSI_STATUS, 0xED00);
        return;
    }
    bios_cd_vdc_select(pce, 0x00);
    bios_vram_write_address(pce);
    bios_cd_vdc_select(pce, 0x02);
    hle_cycles(pce, 70);
    call(pce, SCSI_READ_VRAM_SLOW, 0xECFA);
}

static void cd_read_vram_slow_done(Pce *pce)
{
    mw(pce, 0x2272, 0);
    hle_cycles(pce, 12);
    call(pce, SCSI_STATUS, 0xED00);
}

static void cd_read_vram_slow_checked(Pce *pce) { cd_read_check(pce, 0xED07); }

/* $20FF = 2..6: whole banks in window $20FF, starting from bank $20FA */
static void cd_read_mpr(Pce *pce)
{
    X = alu_nz(pce, z(pce, 0xF8));
    if (X) {
        alu_cmp(pce, A, 0x07);
    }
    if (!X || !(pce->cpu.p & HU_FLAG_N)) {
        A = alu_nz(pce, 0x22);
        hle_cycles(pce, 340);
        hle_rts(pce);
        return;
    }
    X = A;
    uint8_t bit = 1;
    for (uint8_t i = X; i; i--)
        bit = alu_asl(pce, bit);
    X = 0;
    A = bit;
    mw(pce, 0x228F, A);
    mw(pce, 0x228E, 0x43); /* TMA: the BIOS builds "TMA #bit; RTS" at $228E and calls it */
    mw(pce, 0x2290, 0x60);
    A = pce->cpu.mpr[bios_window_of_bit(bit)];
    pce->cpu.mpr_buffer = A;
    mw(pce, 0x2268, A);
    A = z(pce, 0xFA);
    zw(pce, 0xFE, A);
    A = z(pce, 0xF8);
    mw(pce, 0x2250, A);
    A = alu_lsr(pce, alu_lsr(pce, A));
    mw(pce, 0x2280, A);
    A = alu_nz(pce, (uint8_t)(z(pce, 0xF8) & 0x03));
    mw(pce, 0x2281, A);
    hle_cycles(pce, 420);
    call(pce, SCSI_SEND, 0xED59);
}

static void cd_read_mpr_done(Pce *pce);

static void cd_read_mpr_window(Pce *pce)
{
    A = alu_nz(pce, m(pce, 0x2280));
    if (A) {
        A = 0x20;
    } else {
        A = alu_nz(pce, m(pce, 0x2281));
        if (!A) {
            hle_cycles(pce, 20);
            call(pce, SCSI_STATUS, 0xED9E);
            return;
        }
        A = alu_asl(pce, alu_asl(pce, alu_asl(pce, A)));
    }
    zw(pce, 0xF9, A);
    zw(pce, 0xF8, 0);
    uint8_t w = z(pce, 0xFF);
    A = w;
    for (int i = 0; i < 5; i++)
        A = alu_asl(pce, A);
    zw(pce, 0xFB, A);
    zw(pce, 0xFA, 0);
    mw(pce, 0x228E, 0x53); /* TAM */
    A = z(pce, 0xFE);
    int win = bios_window_of_bit(m(pce, 0x228F));
    pce->cpu.mpr[win] = A;
    pce->cpu.mpr_buffer = A;
    hle_cycles(pce, 110);
    call(pce, SCSI_READ_FAST, 0xED8E);
}

static void cd_read_mpr_sent(Pce *pce)
{
    A = alu_nz(pce, (uint8_t)(A | 0x40));
    alu_cmp(pce, A, 0xC8);
    if (A != 0xC8) {
        hle_cycles(pce, 2 + 2 + 4 + 7); /* ORA, CMP, BNE, JSR */
        call(pce, SCSI_STATUS, 0xED9E);
        return;
    }
    cd_read_mpr_window(pce);
}

static void cd_read_mpr_data(Pce *pce)
{
    A = alu_nz(pce, (uint8_t)(A | 0x40));
    alu_cmp(pce, A, 0xC8);
    if (A == 0xC8) {
        zw(pce, 0xFE, (uint8_t)(z(pce, 0xFE) + 1));
        uint8_t n = alu_nz(pce, (uint8_t)(m(pce, 0x2280) - 1));
        mw(pce, 0x2280, n);
        if (!(n & 0x80)) {
            cd_read_mpr_window(pce);
            return;
        }
        hle_cycles(pce, 2 + 2 + 2 + 6 + 7 + 2 + 7); /* ORA, CMP, BNE, INC, DEC, BPL, JSR */
        call(pce, SCSI_STATUS, 0xED9E);
        return;
    }
    hle_cycles(pce, 2 + 2 + 4 + 7);
    call(pce, SCSI_STATUS, 0xED9E);
}

static void cd_read_mpr_done(Pce *pce)
{
    uint8_t a = A;
    mw(pce, 0x228E, 0x53);
    A = m(pce, 0x2268);
    int win = bios_window_of_bit(m(pce, 0x228F));
    pce->cpu.mpr[win] = A;
    pce->cpu.mpr_buffer = A;
    A = alu_nz(pce, a);
    hle_cycles(pce, 40);
    hle_rts(pce);
}

static void cd_read_mpr_status(Pce *pce)
{
    alu_cmp(pce, A, 0x00);
    if (A == 0) {
        cd_read_mpr_done(pce);
        return;
    }
    hle_cycles(pce, 11);
    call(pce, CD_ERROR, 0xEDA5);
}

static void cd_read_mpr_error(Pce *pce)
{
    if (alu_c(pce)) {
        cd_read_mpr_done(pce);
        return;
    }
    hle_cycles(pce, 4);
    go(pce, 0xED0A);
}

/* ---- cd_base ($E006): starting sector of the reads. $20FB = 0: sector in $20F8-$20FA; bit 6: minute,
 * second and frame (BCD) in $20F8-$20FA; otherwise: track $20F8 of the disc TOC.
 * $20FC bit 1 = 0: stores the 1st base ($2274-$2276); bit 0 = 0: the 2nd ($2277-$2279). ---- */

/* Times (CPU cycles) of the BIOS blocks; there is only RAM here, so the accesses spend no time of their own
 * (hle_ram_free) and each piece spends the total `t` at the end */

/* EBE0: stores the bases (time includes the RTS) */
static void cd_base_store(Pce *pce, int t)
{
    if (!(z(pce, 0xFC) & 0x02)) {
        for (int i = 0; i < 3; i++) {
            A = alu_nz(pce, z(pce, (uint8_t)(0xF8 + i)));
            mw(pce, (uint16_t)(0x2274 + i), A);
        }
        t += 6 + 3 * (4 + 5); /* BBS1 not taken, LDA/STA */
    } else {
        t += 8;
    }
    if (!(z(pce, 0xFC) & 0x01)) {
        for (int i = 0; i < 3; i++) {
            A = alu_nz(pce, z(pce, (uint8_t)(0xF8 + i)));
            mw(pce, (uint16_t)(0x2277 + i), A);
        }
        t += 6 + 3 * (4 + 5);
    } else {
        t += 8;
    }
    hle_cycles(pce, t + 7);
    hle_rts(pce);
}

/* E619 (BCD -> binary), with the JSR and the RTS: invalid digits exit earlier */
static int bcd_cycles(uint8_t v)
{
    if ((v & 0xF0) >= 0xA0)
        return 26 + 7;
    if ((v & 0x0F) >= 0x0A)
        return 54 + 7;
    return 62 + 7;
}

/* EBD3: bank 1 in window 6, CC89 (minute/second/frame -> sector), restores the window */
static void cd_base_convert(Pce *pce, int t)
{
    uint8_t old = pce->cpu.mpr[6]; /* the conversion lives in BIOS bank 1, in window 6 */
    for (int i = 0; i < 3; i++)
        t += 5 + bcd_cycles(m(pce, (uint16_t)(0x2256 + i))) + 5; /* LDA, JSR E619, STA */
    bios_msf_to_lba(pce);
    A = old;
    pce->cpu.mpr_buffer = old;
    alu_nz(pce, old);
    /* TMA PHA LDA# TAM JSR; the rest of CC89 (24-bit arithmetic via BSR, always the same time); PLA TAM */
    t += 4 + 3 + 2 + 5 + 7 + 965 + 4 + 5;
    cd_base_store(pce, t);
}

static void cd_base(Pce *pce)
{
    hle_ram_free(pce);
    A = alu_nz(pce, z(pce, 0xFB));
    if (!A) {
        cd_base_store(pce, 4 + 4 + 4); /* JMP, LDA, BEQ */
        return;
    }
    if (A & 0x40) {
        for (int i = 0; i < 3; i++) {
            A = z(pce, (uint8_t)(0xF8 + i));
            mw(pce, (uint16_t)(0x2256 + i), A);
        }
        cd_base_convert(pce, 4 + 4 + 2 + 8 + 3 * (4 + 5));
        return;
    }
    A = z(pce, 0xF8);
    zw(pce, 0xF9, A);
    zw(pce, 0xF8, 0x02);
    zw(pce, 0xFA, 0x56);
    zw(pce, 0xFB, 0x22);
    hle_cycles(pce, 4 + 4 + 2 + 6 + 4 + 4 + 3 * (2 + 4) + 7);
    call(pce, 0xF00A, 0xEBC2);
}

static void cd_base_toc_done(Pce *pce)
{
    hle_ram_free(pce);
    cd_base_convert(pce, 4); /* BRA */
}

/* F00A: disc TOC ($DE): type $20F8, track $20F9 -> 4 bytes at ($20FA) */
static void cd_toc(Pce *pce)
{
    bios_save_args(pce);
    hle_cycles(pce, T_SAVE_ARGS);
    go(pce, 0xF00D);
}

static void cd_toc_send(Pce *pce)
{
    bios_clear_command(pce);
    A = 0xDE;
    mw(pce, 0x224C, A);
    A = z(pce, 0xF8);
    mw(pce, 0x224D, A);
    A = z(pce, 0xF9);
    mw(pce, 0x224E, A);
    hle_cycles(pce, T_CLEAR_COMMAND + 2 + 5 + 4 + 5 + 4 + 5 + 7);
    call(pce, SCSI_SEND, 0xF022);
}

static void cd_toc_sent(Pce *pce)
{
    alu_cmp(pce, A, 0xC8);
    if (A == 0xC8) {
        A = 0x04;
        zw(pce, 0xF8, A);
        zw(pce, 0xF9, 0);
        hle_cycles(pce, 2 + 2 + 2 + 4 + 4 + 7);
        call(pce, SCSI_READ_SLOW, 0xF02F);
        return;
    }
    hle_cycles(pce, 2 + 4 + 7);
    call(pce, SCSI_STATUS, 0xF032);
}

static void cd_toc_status(Pce *pce)
{
    hle_cycles(pce, 7);
    call(pce, SCSI_STATUS, 0xF032);
}

static void cd_toc_done(Pce *pce)
{
    uint8_t a = A;
    bios_restore_args(pce);
    A = alu_nz(pce, a);
    alu_cmp(pce, A, 0x00);
    /* PHA, JSR F331, PLA, CMP, BEQ (taken: RTS / not taken: JSR E224) */
    if (A == 0) {
        hle_cycles(pce, 3 + T_SAVE_ARGS + 4 + 2 + 4 + 7);
        hle_rts(pce);
        return;
    }
    hle_cycles(pce, 3 + T_SAVE_ARGS + 4 + 2 + 2 + 7);
    call(pce, CD_ERROR, 0xF03E);
}

static void cd_toc_error(Pce *pce)
{
    if (alu_c(pce)) {
        hle_cycles(pce, 4 + 7);
        hle_rts(pce);
        return;
    }
    bios_restore_args(pce);
    hle_cycles(pce, 2 + T_SAVE_ARGS + 4);
    go(pce, 0xF00D);
}

/* ---- cd_play ($E012): plays CD audio. $20FB = start type ($00 sector, $40 min/sec/frame,
 * $80 track; $C0 = leaves the start unchanged) in $20F8-$20FA; $20FF = mode (bits 0-2: 1 = repeat... ,
 * bits 6-7 end type, in $20FC-$20FE; $C0 = end of disc). ---- */

static void cd_play_end(Pce *pce);

static void cd_play(Pce *pce)
{
    bios_save_args(pce);
    hle_cycles(pce, 145);
    go(pce, 0xEE2C);
}

static void cd_play_start(Pce *pce)
{
    bios_clear_command(pce);
    A = alu_nz(pce, (uint8_t)(z(pce, 0xFB) & 0xC0));
    alu_cmp(pce, A, 0xC0);
    if (A == 0xC0) {
        hle_cycles(pce, 100);
        cd_play_end(pce);
        return;
    }
    alu_cmp(pce, A, 0x80);
    if (A == 0x80) {
        zw(pce, 0xF9, 0);
        zw(pce, 0xFA, 0);
    }
    mw(pce, 0x2255, A);
    A = 0xD8;
    mw(pce, 0x224C, A);
    A = alu_nz(pce, z(pce, 0xFB));
    bios_put3(pce, 0, A ? 2 : 3);
    hle_cycles(pce, 190);
    call(pce, SCSI_SEND, 0xEE5C);
}

static void cd_play_start_sent(Pce *pce)
{
    hle_cycles(pce, 7);
    call(pce, SCSI_STATUS, 0xEE5F);
}

static void cd_play_start_status(Pce *pce)
{
    alu_cmp(pce, A, 0x00);
    if (A == 0) {
        hle_cycles(pce, 6);
        cd_play_end(pce);
        return;
    }
    hle_cycles(pce, 11);
    call(pce, CD_ERROR, 0xEE66);
}

static void cd_play_start_error(Pce *pce)
{
    if (alu_c(pce)) {
        hle_cycles(pce, 4);
        hle_rts(pce);
        return;
    }
    bios_restore_args(pce);
    hle_cycles(pce, 145);
    go(pce, 0xEE2C);
}

/* EF1B: one frame earlier (BCD) in $226F-$2271 */
static uint8_t bcd_dec(Pce *pce, uint16_t addr, uint8_t limit, uint8_t wrap)
{
    uint8_t v = m(pce, addr);
    int bin = (v >> 4) * 10 + (v & 0x0F) - 1;
    alu_setc(pce, bin >= 0);
    if (bin < 0)
        bin += 100;
    v = alu_nz(pce, (uint8_t)(((bin / 10) << 4) | (bin % 10)));
    mw(pce, addr, v);
    alu_cmp(pce, v, limit);
    if (v >= limit) {
        mw(pce, addr, wrap);
        return 1;
    }
    return 0;
}

static void frame_before(Pce *pce)
{
    uint8_t a = A;
    if (bcd_dec(pce, 0x2271, 0x75, 0x74) && bcd_dec(pce, 0x2270, 0x60, 0x59))
        bcd_dec(pce, 0x226F, 0xFF, 0);
    pce->cpu.p &= (uint8_t)~HU_FLAG_D;
    A = alu_nz(pce, a);
}

static void cd_play_end(Pce *pce)
{
    A = alu_nz(pce, (uint8_t)(z(pce, 0xFB) & 0x01));
    if (A) {
        A = 0;
        hle_cycles(pce, 20);
        hle_rts(pce);
        return;
    }
    bios_clear_command(pce);
    A = 0xD9;
    mw(pce, 0x224C, A);
    A = (uint8_t)(z(pce, 0xFF) & 0x07);
    mw(pce, 0x224D, A);
    A = alu_nz(pce, (uint8_t)(z(pce, 0xFF) & 0xC0));
    mw(pce, 0x2255, A);
    if (!A) {
        bios_put3(pce, 4, 3);
    } else {
        alu_cmp(pce, A, 0xC0);
        if (A == 0xC0) {
            mw(pce, 0x226F, m(pce, 0x226C));
            mw(pce, 0x2270, m(pce, 0x226D));
            mw(pce, 0x2271, m(pce, 0x226E));
            frame_before(pce);
            mw(pce, 0x224E, m(pce, 0x226F));
            mw(pce, 0x224F, m(pce, 0x2270));
            A = m(pce, 0x2271);
            mw(pce, 0x2250, A);
            A = 0x40;
            mw(pce, 0x2255, A);
        } else {
            alu_cmp(pce, A, 0x80);
            if (A == 0x80) {
                zw(pce, 0xFD, 0);
                zw(pce, 0xFE, 0);
            }
            bios_put3(pce, 4, 2);
        }
    }
    hle_cycles(pce, 220);
    call(pce, SCSI_SEND, 0xEEDA);
}

static void cd_play_end_status_call(Pce *pce)
{
    A = m(pce, 0x224D);
    alu_cmp(pce, A, 0x04);
    if (A == 0x04)
        mw(pce, 0x229C, A);
    hle_cycles(pce, 20);
    call(pce, SCSI_STATUS, 0xEF0B);
}

static void cd_play_end_sent(Pce *pce)
{
    alu_cmp(pce, A, 0xD8);
    if (A == 0xD8) {
        cd_play_end_status_call(pce);
        return;
    }
    A = m(pce, 0x224D);
    alu_cmp(pce, A, 0x04);
    if (A == 0x04)
        A = m(pce, 0x229C);
    mw(pce, 0x229C, A);
    alu_cmp(pce, A, 0x01);
    if (A == 0x01) {
        A = 0;
        hle_cycles(pce, 35);
        hle_rts(pce);
        return;
    }
    alu_cmp(pce, A, 0x02);
    if (A == 0x02) {
        mw(pce, 0x22A4, 0);
        bios_tsb(pce, 0x1802, 0x20);
        A = 0;
        hle_cycles(pce, 60);
        hle_rts(pce);
        return;
    }
    cd_play_end_status_call(pce);
}

static void cd_play_end_status(Pce *pce)
{
    alu_cmp(pce, A, 0x00);
    if (A == 0) {
        hle_cycles(pce, 13);
        hle_rts(pce);
        return;
    }
    hle_cycles(pce, 11);
    call(pce, CD_ERROR, 0xEF12);
}

static void cd_play_end_error(Pce *pce)
{
    if (alu_c(pce)) {
        hle_cycles(pce, 4);
        hle_rts(pce);
        return;
    }
    bios_restore_args(pce);
    hle_cycles(pce, 145);
    go(pce, 0xEE6E);
}

static void cd_play_end_point(Pce *pce) { cd_play_end(pce); }

/* ---- cd_subq (EFD8): Q subchannel (10 bytes) at ($20FA) ---- */

static void cd_subq(Pce *pce)
{
    bios_save_args(pce);
    hle_cycles(pce, 140);
    go(pce, 0xEFDB);
}

static void cd_subq_send(Pce *pce)
{
    bios_clear_command(pce);
    A = 0xDD;
    mw(pce, 0x224C, A);
    A = 0x0A;
    mw(pce, 0x224D, A);
    hle_cycles(pce, 150);
    call(pce, SCSI_SEND, 0xEFEB);
}

static void cd_subq_sent(Pce *pce)
{
    alu_cmp(pce, A, 0xC8);
    if (A == 0xC8) {
        A = 0x0A;
        zw(pce, 0xF8, A);
        zw(pce, 0xF9, 0);
        hle_cycles(pce, 22);
        call(pce, SCSI_READ_SLOW, 0xEFF8);
        return;
    }
    hle_cycles(pce, 2 + 4 + 7);
    call(pce, SCSI_STATUS, 0xEFFB);
}

static void cd_subq_status(Pce *pce)
{
    hle_cycles(pce, 7);
    call(pce, SCSI_STATUS, 0xEFFB);
}

static void cd_subq_done(Pce *pce)
{
    alu_cmp(pce, A, 0x00);
    if (A == 0) {
        hle_cycles(pce, 13);
        hle_rts(pce);
        return;
    }
    hle_cycles(pce, 11);
    call(pce, CD_ERROR, 0xF002);
}

static void cd_subq_error(Pce *pce)
{
    if (alu_c(pce)) {
        hle_cycles(pce, 4);
        hle_rts(pce);
        return;
    }
    bios_restore_args(pce);
    hle_cycles(pce, 140);
    go(pce, 0xEFDB);
}

/* ---- cd_pause ($E018): pauses the CD audio (if playing: Q subchannel, byte 0 = 0) ---- */

static void cd_pause(Pce *pce)
{
    zw(pce, 0xFA, 0x56);
    zw(pce, 0xFB, 0x22);
    hle_cycles(pce, 24);
    call(pce, 0xEFD8, 0xEFB7);
}

static void cd_pause_subq_done(Pce *pce)
{
    A = m(pce, 0x2256);
    alu_cmp(pce, A, 0x00);
    if (A) {
        A = 0;
        hle_cycles(pce, 20);
        hle_rts(pce);
        return;
    }
    bios_clear_command(pce);
    A = 0xDA;
    mw(pce, 0x224C, A);
    hle_cycles(pce, 130);
    call(pce, SCSI_SEND, 0xEFCB);
}

static void cd_pause_sent(Pce *pce)
{
    hle_cycles(pce, 7);
    call(pce, SCSI_STATUS, 0xEFCE);
}

static void cd_pause_status(Pce *pce)
{
    alu_cmp(pce, A, 0x00);
    if (A == 0) {
        hle_cycles(pce, 13);
        hle_rts(pce);
        return;
    }
    hle_cycles(pce, 11);
    call(pce, CD_ERROR, 0xEFD5);
}

static void cd_pause_error(Pce *pce)
{
    hle_cycles(pce, 4);
    if (alu_c(pce))
        hle_rts(pce);
    else
        go(pce, 0xE018);
}

/* ---- cd_fade ($E02D): CD/ADPCM volume fade (A = mode at $180F) ---- */

static void cd_fade(Pce *pce)
{
    A = alu_nz(pce, (uint8_t)(A & 0x0F));
    mw(pce, 0x180F, A);
    hle_cycles(pce, 18);
    hle_rts(pce);
}

const HleRoutine hle_cd_routines[] = {
    {0xE006, "cd_base", cd_base, {{0xEBC2, cd_base_toc_done}}},
    {0xE009, "cd_read", cd_read,
     {{0xEC24, cd_read_start}, {0xEC6A, cd_read_sent}, {0xEC71, cd_read_data_done}, {0xEC78, cd_read_status},
      {0xEC7B, cd_read_checked}, {0xEC82, cd_read_error_done}, {0xED0A, cd_read_retry}}},
    {0xE012, "cd_play", cd_play,
     {{0xEE2C, cd_play_start}, {0xEE5C, cd_play_start_sent}, {0xEE5F, cd_play_start_status},
      {0xEE66, cd_play_start_error}, {0xEE6E, cd_play_end_point}, {0xEEDA, cd_play_end_sent},
      {0xEF0B, cd_play_end_status}, {0xEF12, cd_play_end_error}}},
    {0xE018, "cd_pause", cd_pause,
     {{0xEFB7, cd_pause_subq_done}, {0xEFCB, cd_pause_sent}, {0xEFCE, cd_pause_status}, {0xEFD5, cd_pause_error}}},
    {0xE02D, "cd_fade", cd_fade, {{0}}},
    /* cd_read: the other destinations (VRAM, banks) */
    {0xEC9C, "cd_read_vram", cd_read_vram_sent,
     {{0xECBD, cd_read_vram_done}, {0xECC3, cd_read_vram_checked}, {0xECCA, cd_read_error_done},
      {0xECE2, cd_read_vram_slow_sent}, {0xECFA, cd_read_vram_slow_done}, {0xED00, cd_read_vram_slow_checked},
      {0xED07, cd_read_error_done}}, 1},
    {0xED59, "cd_read_mpr", cd_read_mpr_sent,
     {{0xED8E, cd_read_mpr_data}, {0xED9E, cd_read_mpr_status}, {0xEDA5, cd_read_mpr_error}}, 1},
    /* Internal parts */
    {0xEFD8, "cd_subq", cd_subq,
     {{0xEFDB, cd_subq_send}, {0xEFEB, cd_subq_sent}, {0xEFF8, cd_subq_status}, {0xEFFB, cd_subq_done},
      {0xF002, cd_subq_error}}, 1},
    {0xF00A, "cd_toc", cd_toc,
     {{0xF00D, cd_toc_send}, {0xF022, cd_toc_sent}, {0xF02F, cd_toc_status}, {0xF032, cd_toc_done},
      {0xF03E, cd_toc_error}}, 1},
    {0xE224, "cd_error", cd_error, {{0xE23A, cd_error_sense}}, 1},
    {0xF06C, "cd_sense", cd_sense, {{0xF07F, cd_sense_sent}, {0xF094, cd_sense_status}, {0xF097, cd_sense_done}}, 1},
    /* The conversation with the drive, one instruction per pseudo-instruction (scsi_op; the names are the entries') */
    {0xE8FA, "scsi_reset", scsi_op, {{0}}, 1, scsi_ops},
    {SCSI_SEND, "scsi_send", scsi_op, {{0}}, 1},
    {0xE9A1, "scsi_sent", scsi_op, {{0}}, 1},
    {SCSI_STATUS, "scsi_status", scsi_op, {{0}}, 1},
    {0xEA3E, "scsi_ack", scsi_op, {{0}}, 1},
    {SCSI_READ_SLOW, "scsi_read_slow", scsi_op, {{0}}, 1},
    {SCSI_READ_FAST, "scsi_read_fast", scsi_op, {{0}}, 1},
    {SCSI_READ_VRAM, "scsi_read_vram", scsi_op, {{0}}, 1},
    {SCSI_READ_VRAM_SLOW, "scsi_vram_slow", scsi_op, {{0}}, 1},
    {SCSI_DRAIN, "scsi_drain", scsi_op, {{0}}, 1},
    {0, NULL, NULL, {{0}}, 0},
};
