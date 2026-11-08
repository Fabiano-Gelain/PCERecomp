/*
 * Arithmetic with the same flag effects as the CPU instructions (so the C BIOS routines return
 * A, X, Y and the flags like the real BIOS; the game sometimes tests the carry on return).
 */
#ifndef PCE_BIOS_ALU_H
#define PCE_BIOS_ALU_H

#include "pce/pce.h"

#define CPU(pce) (&(pce)->cpu)

static inline int alu_c(Pce *pce)
{
    return (CPU(pce)->p & HU_FLAG_C) != 0;
}

static inline void alu_setc(Pce *pce, int c)
{
    CPU(pce)->p = (uint8_t)(c ? (CPU(pce)->p | HU_FLAG_C) : (CPU(pce)->p & ~HU_FLAG_C));
}

static inline uint8_t alu_nz(Pce *pce, uint8_t v)
{
    HuC6280 *c = CPU(pce);
    c->p = (uint8_t)((c->p & ~(HU_FLAG_N | HU_FLAG_Z)) | (v & 0x80) | (v == 0 ? HU_FLAG_Z : 0));
    return v;
}

/* CMP/CPX/CPY */
static inline void alu_cmp(Pce *pce, uint8_t reg, uint8_t m)
{
    alu_setc(pce, reg >= m);
    alu_nz(pce, (uint8_t)(reg - m));
}

/* ADC (binary mode; the BIOS does not use decimal mode in these routines) */
static inline uint8_t alu_adc(Pce *pce, uint8_t a, uint8_t m)
{
    unsigned r = (unsigned)a + m + (unsigned)alu_c(pce);
    HuC6280 *c = CPU(pce);
    int v = (~(a ^ m) & (a ^ r) & 0x80) != 0;
    c->p = (uint8_t)(v ? (c->p | HU_FLAG_V) : (c->p & ~HU_FLAG_V));
    alu_setc(pce, r > 0xFF);
    return alu_nz(pce, (uint8_t)r);
}

static inline uint8_t alu_sbc(Pce *pce, uint8_t a, uint8_t m)
{
    unsigned r = (unsigned)a - m - (unsigned)(1 - alu_c(pce));
    HuC6280 *c = CPU(pce);
    int v = ((a ^ m) & (a ^ r) & 0x80) != 0;
    c->p = (uint8_t)(v ? (c->p | HU_FLAG_V) : (c->p & ~HU_FLAG_V));
    alu_setc(pce, r < 0x100);
    return alu_nz(pce, (uint8_t)r);
}

static inline uint8_t alu_asl(Pce *pce, uint8_t v)
{
    alu_setc(pce, v & 0x80);
    return alu_nz(pce, (uint8_t)(v << 1));
}

static inline uint8_t alu_lsr(Pce *pce, uint8_t v)
{
    alu_setc(pce, v & 0x01);
    return alu_nz(pce, (uint8_t)(v >> 1));
}

static inline uint8_t alu_rol(Pce *pce, uint8_t v)
{
    int c = alu_c(pce);
    alu_setc(pce, v & 0x80);
    return alu_nz(pce, (uint8_t)((v << 1) | c));
}

static inline uint8_t alu_ror(Pce *pce, uint8_t v)
{
    int c = alu_c(pce);
    alu_setc(pce, v & 0x01);
    return alu_nz(pce, (uint8_t)((v >> 1) | (c << 7)));
}

#endif
