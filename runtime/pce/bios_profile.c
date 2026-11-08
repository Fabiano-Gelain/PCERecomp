/*
 * Profile of the BIOS routines. See bios_profile.h.
 */
#include "pce/bios_profile.h"

#include <stdio.h>
#include <string.h>

#include "pce/pce.h"

static void push(BiosProfile *bp, char kind, uint16_t address, uint8_t s);

void bios_profile_init(BiosProfile *bp, struct Pce *pce)
{
    memset(bp, 0, sizeof(*bp));
    bp->irq_count = pce->cpu.irq_count;
    /* Initialization (from reset until the first game instruction) counts as routine 'B'. */
    push(bp, 'B', pce->cpu.pc, pce->cpu.s);
}

static BiosRoutine *slot(BiosProfile *bp, char kind, uint16_t address)
{
    for (int i = 0; i < BIOS_PROFILE_SLOTS; i++) {
        BiosRoutine *r = &bp->routines[i];
        if (r->kind == kind && r->address == address)
            return r;
        if (r->kind == 0) {
            r->kind = kind;
            r->address = address;
            return r;
        }
    }
    return NULL; /* full: unlikely (the table has ~80 entries) */
}

static void push(BiosProfile *bp, char kind, uint16_t address, uint8_t s)
{
    BiosRoutine *r = slot(bp, kind, address);
    if (!r || bp->depth >= BIOS_PROFILE_DEPTH)
        return;
    r->calls++;
    bp->stack[bp->depth].r = r;
    bp->stack[bp->depth].s = s;
    bp->depth++;
}

void bios_profile_before(BiosProfile *bp, struct Pce *pce)
{
    const HuC6280 *c = &pce->cpu;
    int bios_code = c->mpr[c->pc >> 13] < 0x80; /* banks $00-$7F = the BIOS (in place of the HuCard) */

    /* Back in game code with the stack above where it was on entry: the routine has finished. */
    while (bp->depth > 0 && !bios_code &&
           ((int8_t)(c->s - bp->stack[bp->depth - 1].s) > 0 || bp->stack[bp->depth - 1].r->kind == 'B'))
        bp->depth--;

    if (c->irq_count != bp->irq_count) { /* just entered an interrupt */
        bp->irq_count = c->irq_count;
        if (bios_code)
            push(bp, 'I', c->pc, c->s);
    } else if (bios_code && !bp->in_bios && c->pc >= 0xE000 && c->pc < 0xE100) {
        push(bp, 'C', c->pc, c->s); /* the game called a table entry */
    }

    bp->in_bios = bios_code;
    if (bp->depth > 0 && bios_code)
        bp->stack[bp->depth - 1].r->instructions++;
    memcpy(bp->mpr_before, c->mpr, 8);
}

void bios_profile_after(BiosProfile *bp, struct Pce *pce)
{
    if (bp->depth == 0 || !bp->in_bios)
        return;
    BiosRoutine *r = bp->stack[bp->depth - 1].r;
    for (int i = 0; i < 8; i++)
        if (pce->cpu.mpr[i] != bp->mpr_before[i])
            r->mpr |= (uint8_t)(1u << i);
}

void bios_profile_write(BiosProfile *bp, uint32_t phys, uint8_t value)
{
    if (bp->depth == 0 || !bp->in_bios)
        return;
    BiosRoutine *r = bp->stack[bp->depth - 1].r;
    uint8_t bank = (uint8_t)(phys >> 13);
    uint16_t off = (uint16_t)(phys & 0x1FFF);
    if (bank >= PCE_RAM_BANK && bank <= 0xFB) {
        r->ram[off >> 3] |= (uint8_t)(1u << (off & 7));
    } else if (bank >= 0x80 && bank <= 0x87) {
        r->cdram++;
    } else if (bank == 0xF7) {
        r->bram++;
    } else if (bank == PCE_IO_BANK) {
        switch (off & 0x1C00) {
        case 0x0000:
            if ((off & 3) == 0)
                bp->vdc_selected = value & 0x1F;
            else if ((off & 3) >= 2)
                r->vdc_regs |= 1u << bp->vdc_selected;
            break;
        case 0x0400: r->vce++; break;
        case 0x0800: r->psg++; break;
        case 0x0C00: r->timer++; break;
        case 0x1000: r->pad++; break;
        case 0x1400: r->irq++; break;
        case 0x1800: r->cd_regs |= (uint16_t)(1u << (off & 0x0F)); break;
        default: break;
        }
    }
}

void bios_profile_vdc(BiosProfile *bp, int port, uint8_t value)
{
    if (bp->depth == 0 || !bp->in_bios)
        return;
    if (port == 0)
        bp->vdc_selected = value & 0x1F;
    else
        bp->stack[bp->depth - 1].r->vdc_regs |= 1u << bp->vdc_selected;
}

static void write_ranges(FILE *f, const uint8_t *bits)
{
    int first = 1;
    for (int i = 0; i < 0x2000; i++) {
        if (!((bits[i >> 3] >> (i & 7)) & 1))
            continue;
        int j = i;
        while (j + 1 < 0x2000 && ((bits[(j + 1) >> 3] >> ((j + 1) & 7)) & 1))
            j++;
        fprintf(f, "%s%04X", first ? "" : ",", 0x2000 + i);
        if (j > i)
            fprintf(f, "-%04X", 0x2000 + j);
        first = 0;
        i = j;
    }
    if (first)
        fputc('-', f);
}

static void write_bits(FILE *f, uint32_t bits, int count)
{
    int first = 1;
    for (int i = 0; i < count; i++)
        if (bits & (1u << i)) {
            fprintf(f, "%s%02X", first ? "" : ",", i);
            first = 0;
        }
    if (first)
        fputc('-', f);
}

int bios_profile_save(const BiosProfile *bp, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "# PCERecomp BIOS profile v1 (runtime/pce/bios_profile.h)\n");
    for (int i = 0; i < BIOS_PROFILE_SLOTS; i++) {
        const BiosRoutine *r = &bp->routines[i];
        if (!r->kind)
            continue;
        fprintf(f, "R %c %04X calls=%llu instructions=%llu ram=", r->kind, r->address,
                (unsigned long long)r->calls, (unsigned long long)r->instructions);
        write_ranges(f, r->ram);
        fprintf(f, " cdram=%llu bram=%llu vdc=", (unsigned long long)r->cdram, (unsigned long long)r->bram);
        write_bits(f, r->vdc_regs, 20);
        fprintf(f, " vce=%llu psg=%llu timer=%llu joypad=%llu irq=%llu cd=", (unsigned long long)r->vce,
                (unsigned long long)r->psg, (unsigned long long)r->timer, (unsigned long long)r->pad,
                (unsigned long long)r->irq);
        write_bits(f, r->cd_regs, 16);
        fprintf(f, " mpr=");
        write_bits(f, r->mpr, 8);
        fputc('\n', f);
    }
    return fclose(f) == 0 ? 0 : -1;
}
