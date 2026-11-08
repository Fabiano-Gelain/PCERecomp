/*
 * C BIOS: registration, dispatch and utilities. See hle.h.
 */
#include "bios/hle.h"

#include <stdio.h>
#include <string.h>

#include "bios/internal.h"
#include "bios/routines.h"
#include "pce/pce.h"

static void add(Hle *hle, uint16_t only, const HleRoutine *r, uint16_t addr, HleFn fn)
{
    if (only && only != r->entry && !r->internal)
        return;
    hle->fn[addr - 0xE000] = fn;
}

void hle_init(Hle *hle, uint16_t only)
{
    memset(hle, 0, sizeof(*hle));
    for (int g = 0; hle_groups[g]; g++) {
        for (const HleRoutine *r = hle_groups[g]; r->entry; r++) {
            add(hle, only, r, r->entry, r->fn);
            for (int i = 0; i < HLE_MAX_POINTS && r->points[i].addr; i++)
                add(hle, only, r, r->points[i].addr, r->points[i].fn);
            for (const uint16_t *a = r->ops; a && *a; a++)
                add(hle, only, r, *a, r->fn);
        }
    }
}

extern const HleRoutine hle_boot_routines[];

void hle_enable_synthetic(Hle *hle)
{
    hle->synthetic = 1;
    for (const HleRoutine *r = hle_boot_routines; r->entry; r++) {
        hle->fn[r->entry - 0xE000] = r->fn;
        for (int i = 0; i < HLE_MAX_POINTS && r->points[i].addr; i++)
            hle->fn[r->points[i].addr - 0xE000] = r->points[i].fn;
    }
}

/* Synthetic BIOS address with no C routine: warns once and stays stuck there */
static void missing_routine(Pce *pce)
{
    if (!pce->hle->missing) {
        pce->hle->missing = pce->cpu.pc;
        fprintf(stderr, "Synthetic BIOS: the game called routine $%04X, which does not exist in C\n", pce->cpu.pc);
    }
    hle_cycles(pce, 4);
}

HleFn hle_find(const Hle *hle, const Pce *pce)
{
    const HuC6280 *c = &pce->cpu;
    if (c->pc < 0xE000 || c->mpr[7] != 0x00)
        return NULL;
    HleFn fn = hle->fn[c->pc - 0xE000];
    if (!fn && hle->synthetic && c->pc != 0xE8F7) /* $E8F7: the hooks' RTI */
        return missing_routine;
    return fn;
}

/* Each access happens and THEN spends the time of the instruction that would do it in the BIOS (LDA/STA in
 * the zero page: 4 cycles; absolute address: 5; the VDC/VCE waits come separately, from the bus), as on the
 * CPU (the hardware access lands at the start of the instruction). That way the hardware accesses land on
 * the cycle where the real BIOS would make them, provided the instructions with no access (LDA #, TAX...)
 * also spend theirs beforehand (the VRAM waits and the moment the game draws depend on it) -- and this
 * matters: e.g. if the game left a VRAM read pending and the routine changes the increment mode, the final
 * address depends on which of the two happens first, as on the console. */
static int access_cycles(uint16_t addr)
{
    return (addr >= 0x2000 && addr < 0x2100) ? 4 : 5;
}

/* Memory (RAM, ROM, CD RAM...) and not the hardware page ($FF) */
static int is_memory(const Pce *pce, uint16_t addr)
{
    return pce->cpu.mpr[addr >> 13] != 0xFF;
}

static void spend(Pce *pce, uint16_t addr)
{
    if (pce->hle && pce->hle->ram_free && is_memory(pce, addr))
        return;
    huc6280_add_cycles(&pce->cpu, access_cycles(addr));
}

void hle_ram_free(Pce *pce)
{
    if (pce->hle)
        pce->hle->ram_free = 1;
}

uint8_t hle_rd(Pce *pce, uint16_t addr)
{
    HuC6280 *c = &pce->cpu;
    uint8_t v = c->read(c->ctx, huc6280_phys(c, addr));
    spend(pce, addr);
    return v;
}

void hle_wr(Pce *pce, uint16_t addr, uint8_t v)
{
    HuC6280 *c = &pce->cpu;
    c->write(c->ctx, huc6280_phys(c, addr), v);
    spend(pce, addr);
}

uint8_t hle_peek(Pce *pce, uint16_t addr)
{
    HuC6280 *c = &pce->cpu;
    return c->read(c->ctx, huc6280_phys(c, addr));
}

void hle_poke(Pce *pce, uint16_t addr, uint8_t v)
{
    HuC6280 *c = &pce->cpu;
    int before = c->instr_cycles;
    c->write(c->ctx, huc6280_phys(c, addr), v);
    if (c->instr_cycles != before) /* VDC wait: on the CPU, the interrupt then counts the requests from after it */
        hle_irq_sample(pce);
}

void hle_next(Pce *pce, int cpu_cycles, uint16_t pc)
{
    huc6280_add_cycles(&pce->cpu, cpu_cycles);
    pce->cpu.pc = pc;
}

uint16_t hle_rd16(Pce *pce, uint16_t addr)
{
    return (uint16_t)(hle_rd(pce, addr) | (hle_rd(pce, (uint16_t)(addr + 1)) << 8));
}

void hle_st(Pce *pce, int port, uint8_t v)
{
    HuC6280 *c = &pce->cpu;
    /* ST0/ST1/ST2: 4 cycles; the VDC advances the first 2 before the write (cpu_vdc_write), as on the
     * CPU, and the other 2 come after */
    c->instr_cycles += 2;
    if (c->vdc_write)
        c->vdc_write(c->ctx, port, v);
    huc6280_add_cycles(c, 2);
}

void hle_cycles(Pce *pce, int cpu_cycles)
{
    /* Total time of this pseudo-instruction: tops up what the accesses have not yet spent. */
    int rest = cpu_cycles - pce->cpu.instr_cycles;
    /* In small steps, like instructions: the CD and ADPCM handle one event per step. */
    while (rest > 0) {
        int step = rest > 4 ? 4 : rest;
        huc6280_add_cycles(&pce->cpu, step);
        rest -= step;
    }
}

void hle_idle(Pce *pce, int cpu_cycles)
{
    huc6280_add_cycles(&pce->cpu, cpu_cycles);
}

void hle_irq_sample(Pce *pce)
{
    pce->cpu.hle_irq_snap = 1;
    pce->cpu.hle_irq_req = pce->cpu.irq_request;
}

void hle_instr(Pce *pce, int cpu_cycles)
{
    if (!pce->cpu.hle_irq_snap)
        hle_irq_sample(pce);
    hle_cycles(pce, cpu_cycles);
}

/* Stack: direct access, spending no time of its own (the PHA/PLA/RTS time is part of the routine's total). */
void hle_push(Pce *pce, uint8_t v)
{
    HuC6280 *c = &pce->cpu;
    c->write(c->ctx, huc6280_phys(c, (uint16_t)(0x2100 | c->s)), v);
    c->s--;
}

uint8_t hle_pull(Pce *pce)
{
    HuC6280 *c = &pce->cpu;
    c->s++;
    return c->read(c->ctx, huc6280_phys(c, (uint16_t)(0x2100 | c->s)));
}

void hle_rts(Pce *pce)
{
    uint8_t lo = hle_pull(pce);
    uint8_t hi = hle_pull(pce);
    pce->cpu.pc = (uint16_t)(((hi << 8) | lo) + 1);
}

void hle_rti(Pce *pce)
{
    HuC6280 *c = &pce->cpu;
    c->p = (uint8_t)(hle_pull(pce) & ~HU_FLAG_B);
    uint8_t lo = hle_pull(pce);
    uint8_t hi = hle_pull(pce);
    c->pc = (uint16_t)((hi << 8) | lo);
}

void hle_nz(Pce *pce, uint8_t v)
{
    HuC6280 *c = &pce->cpu;
    c->p = (uint8_t)((c->p & ~(HU_FLAG_N | HU_FLAG_Z)) | (v & 0x80) | (v == 0 ? HU_FLAG_Z : 0));
}

void hle_carry(Pce *pce, int set)
{
    HuC6280 *c = &pce->cpu;
    c->p = (uint8_t)(set ? (c->p | HU_FLAG_C) : (c->p & ~HU_FLAG_C));
}

void bios_call(Pce *pce, uint16_t target, uint16_t ret)
{
    uint16_t pushed = (uint16_t)(ret - 1); /* like JSR/BSR: RTS adds 1 */
    hle_push(pce, (uint8_t)(pushed >> 8));
    hle_push(pce, (uint8_t)pushed);
    pce->cpu.pc = target;
}
