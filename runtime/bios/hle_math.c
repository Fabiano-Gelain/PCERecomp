/*
 * C BIOS: arithmetic (ma_*). See hle.h.
 *
 * Inputs in zero page: $20F8/$20F9 (first operand), $20FA/$20FB (second). Outputs: $20FC/$20FD
 * (result) and, for divisions, $20FE/$20FF (remainder). $20EC/$20ED hold the divisor during the operation.
 * In the BIOS these routines live in bank 3, which the entry code maps into window 6 and then restores: the bank
 * that was there is saved in $22BB. The registers and flags on return are those left at the end of the operation.
 */
#include "bios/alu.h"
#include "bios/hle.h"
#include "bios/routines.h"

#define A (pce->cpu.a)
#define X (pce->cpu.x)
#define Y (pce->cpu.y)

static uint8_t z(Pce *pce, uint8_t n) { return hle_rd(pce, ZP(n)); }
static void zw(Pce *pce, uint8_t n, uint8_t v) { hle_wr(pce, ZP(n), v); }

/* Common entry and exit: bank 3 in window 6 during the operation. */
/* Common timing: table JMP, BSR FDC2 (bank 3 in window 6), JSR to the operation, BRA FDCE (restores the bank) */
#define MATH_WRAP (4 + 8 + 30 + 7 + 4 + 31)

static void enter(Pce *pce)
{
    hle_push(pce, A);
    pce->cpu.mpr_buffer = pce->cpu.mpr[6];
    hle_wr(pce, 0x22BB, pce->cpu.mpr[6]);
    pce->cpu.mpr[6] = 0x03;
    pce->cpu.mpr_buffer = 0x03;
    hle_pull(pce);
}

static void leave(Pce *pce)
{
    hle_push(pce, (uint8_t)(pce->cpu.p | HU_FLAG_B));
    hle_push(pce, A);
    uint8_t bank = hle_rd(pce, 0x22BB);
    pce->cpu.mpr[6] = bank;
    pce->cpu.mpr_buffer = bank;
    hle_pull(pce);
    hle_pull(pce);
}

/* ma_mul8u: $FC:$FD = $F8 x $FA (unsigned), by shift and add */
static void ma_mul8u(Pce *pce)
{
    hle_ram_free(pce);
    int cycles = MATH_WRAP + 16 + 8 * 14 + 8 * 4 + 7 * 2 + 15; /* CD3A, loop CD43/CD4F, CD52 */
    enter(pce);
    A = z(pce, 0xF8);
    X = z(pce, 0xFA);
    zw(pce, 0xFC, A);
    uint8_t m = A;
    A = 0;
    for (Y = 8; Y; ) {
        A = alu_asl(pce, A);
        uint8_t t = A;
        A = X;
        X = t;
        A = alu_rol(pce, A);
        t = A;
        A = X;
        X = t;
        if (alu_c(pce)) {
            cycles += 8; /* CD49, CD4E */
            alu_setc(pce, 0);
            A = alu_adc(pce, A, m);
            if (alu_c(pce))
                X++;
        }
        Y--;
        alu_nz(pce, Y);
    }
    zw(pce, 0xFD, X);
    zw(pce, 0xFC, A);
    leave(pce);
    hle_cycles(pce, cycles);
    hle_rts(pce);
}

/* Unsigned division core: ($FC:$FD already shifted by 1) / ($EC:$ED); quotient in $FC:$FD, remainder in
 * $FE:$FF. 16 steps of "shift, compare, subtract". */
/* CE4B; returns the timing (including the RTS) */
static int divide_core(Pce *pce)
{
    int cycles = 10 + 7;
    uint8_t fc = z(pce, 0xFC), fd = z(pce, 0xFD), ec = z(pce, 0xEC), ed = z(pce, 0xED);
    uint8_t fe = 0, ff = 0;
    zw(pce, 0xFE, 0);
    zw(pce, 0xFF, 0);
    for (X = 0x10; X; ) {
        fe = alu_rol(pce, fe);
        ff = alu_rol(pce, ff);
        A = ff;
        alu_cmp(pce, A, ed);
        cycles += 22 + 2 + 16 + (X > 1 ? 2 : 0); /* CE51 (desvio), CE6D */
        if (A == ed) {
            cycles += 8 - 2; /* CE5B */
            A = fe;
            alu_cmp(pce, A, ec);
        }
        cycles += alu_c(pce) ? 2 + 24 : 4; /* CE5F, CE61 */
        if (alu_c(pce)) {
            A = alu_sbc(pce, fe, ec);
            fe = A;
            A = alu_sbc(pce, ff, ed);
            ff = A;
        }
        fc = alu_rol(pce, fc);
        fd = alu_rol(pce, fd);
        X--;
        alu_nz(pce, X);
    }
    zw(pce, 0xFE, fe);
    zw(pce, 0xFF, ff);
    zw(pce, 0xFC, fc);
    zw(pce, 0xFD, fd);
    return cycles;
}

/* ma_div16u: $FC:$FD = $F8:$F9 / $FA:$FB, remainder in $FE:$FF (divisor 0: result 0) */
static void ma_div16u(Pce *pce)
{
    hle_ram_free(pce);
    int cycles = MATH_WRAP + 10;
    enter(pce);
    A = alu_nz(pce, (uint8_t)(z(pce, 0xFA) | z(pce, 0xFB)));
    if (A == 0) {
        cycles += 15; /* CE34 */
        zw(pce, 0xFC, 0);
        zw(pce, 0xFD, 0);
    } else {
        cycles += 2 + 36; /* CE39 */
        zw(pce, 0xEC, z(pce, 0xFA));
        zw(pce, 0xED, A = alu_nz(pce, z(pce, 0xFB)));
        A = alu_asl(pce, z(pce, 0xF8));
        zw(pce, 0xFC, A);
        A = alu_rol(pce, z(pce, 0xF9));
        zw(pce, 0xFD, A);
        cycles += divide_core(pce);
    }
    leave(pce);
    hle_cycles(pce, cycles);
    hle_rts(pce);
}

static void negate16(Pce *pce, uint8_t lo, uint8_t hi)
{
    alu_setc(pce, 1);
    zw(pce, lo, A = alu_sbc(pce, 0, z(pce, lo)));
    zw(pce, hi, A = alu_sbc(pce, 0, z(pce, hi)));
}

/* ma_div16s: signed division; the quotient is negative if the signs differ, the remainder has the sign of the
 * dividend. (No divisor 0 check, as in the BIOS.) */
static void ma_div16s(Pce *pce)
{
    hle_ram_free(pce);
    int cycles = MATH_WRAP + 32 + 18 + 20 + 6 + 6 + 7; /* CDDB, CDF6, CE0B, CE11, CE1F, RTS */
    enter(pce);
    A = alu_nz(pce, z(pce, 0xF9));
    uint8_t p1 = pce->cpu.p; /* PHP: flags with the dividend's sign */
    hle_push(pce, (uint8_t)(p1 | HU_FLAG_B));
    zw(pce, 0xFD, A);
    A = alu_nz(pce, (uint8_t)(A ^ z(pce, 0xFB)));
    uint8_t p2 = pce->cpu.p; /* PHP: flags with "signs differ" */
    hle_push(pce, (uint8_t)(p2 | HU_FLAG_B));
    zw(pce, 0xFC, A = alu_nz(pce, z(pce, 0xF8)));
    A = alu_nz(pce, z(pce, 0xF9));
    cycles += (A & 0x80) ? 22 : 2; /* CDEB */
    if (A & 0x80) {
        alu_setc(pce, 1);
        zw(pce, 0xFC, A = alu_sbc(pce, 0, z(pce, 0xF8)));
        zw(pce, 0xFD, A = alu_sbc(pce, 0, z(pce, 0xF9)));
    }
    zw(pce, 0xEC, A = alu_nz(pce, z(pce, 0xFA)));
    zw(pce, 0xED, A = alu_nz(pce, z(pce, 0xFB)));
    cycles += (A & 0x80) ? 22 : 2; /* CE00 */
    if (A & 0x80) {
        alu_setc(pce, 1);
        zw(pce, 0xEC, A = alu_sbc(pce, 0, z(pce, 0xFA)));
        zw(pce, 0xED, A = alu_sbc(pce, 0, z(pce, 0xFB)));
    }
    zw(pce, 0xFC, alu_asl(pce, z(pce, 0xFC)));
    zw(pce, 0xFD, alu_rol(pce, z(pce, 0xFD)));
    cycles += divide_core(pce);
    /* The two PLPs restore the flags saved at the start; the N of each decides the sign correction. */
    pce->cpu.p = (uint8_t)(hle_pull(pce) & ~HU_FLAG_B);
    cycles += (pce->cpu.p & HU_FLAG_N) ? 22 : 2; /* CE14 */
    if (pce->cpu.p & HU_FLAG_N)
        negate16(pce, 0xFC, 0xFD);
    pce->cpu.p = (uint8_t)(hle_pull(pce) & ~HU_FLAG_B);
    cycles += (pce->cpu.p & HU_FLAG_N) ? 22 : 2; /* CE22 */
    if (pce->cpu.p & HU_FLAG_N)
        negate16(pce, 0xFE, 0xFF);
    leave(pce);
    hle_cycles(pce, cycles);
    hle_rts(pce);
}

const HleRoutine hle_math_routines[] = {
    {0xE0BD, "ma_mul8u", ma_mul8u, {{0}}},
    {0xE0C6, "ma_div16s", ma_div16s, {{0}}},
    {0xE0C9, "ma_div16u", ma_div16u, {{0}}},
    {0, NULL, NULL, {{0}}},
};
