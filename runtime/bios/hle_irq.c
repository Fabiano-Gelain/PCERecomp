/*
 * C BIOS: interrupt handling (BIOS vectors). See hle.h.
 *
 * $20F5 tells what the game wants: bit 0/1/2 = the game handles the CD/VDC/timer interrupt itself (the BIOS
 * just jumps to the hook at $2200/$2202/$2204); bit 4 = call the VBlank hook ($2208); bit 5 = skip the
 * VBlank work (scrolling, palettes, controls); bit 6 = call the line hook ($220A);
 * bit 7 = do not change the scroll on the line interrupt.
 * $20E6 locks the timer against reentry; $20E7 selects who plays the chip's music: 0 = timer, 1 = VBlank.
 */
#include "bios/alu.h"
#include "bios/hle.h"
#include "bios/internal.h"
#include "bios/routines.h"

#define A (pce->cpu.a)
#define X (pce->cpu.x)
#define Y (pce->cpu.y)

static uint8_t m(Pce *pce, uint16_t a) { return hle_rd(pce, a); }
static void mw(Pce *pce, uint16_t a, uint8_t v) { hle_wr(pce, a, v); }
static uint8_t z(Pce *pce, uint8_t n) { return hle_rd(pce, ZP(n)); }
static void zw(Pce *pce, uint8_t n, uint8_t v) { hle_wr(pce, ZP(n), v); }

#define PSG_TICK 0xE6E8 /* one step of the sound driver (hle_psg.c) */

static void save_registers(Pce *pce)
{
    hle_push(pce, A);
    hle_push(pce, X);
    hle_push(pce, Y);
}

static void restore_and_return(Pce *pce)
{
    Y = hle_pull(pce);
    X = hle_pull(pce);
    A = hle_pull(pce);
    hle_rti(pce);
}

/* TII of len bytes from RAM to the VDC, as on the CPU (huc6280_exec.h, block_transfer): 14 setup cycles;
 * per byte, the write (with the VDC wait) and 6 cycles; 3 at the end */
static void tii_to_vdc(Pce *pce, uint16_t src, uint16_t dst, int len)
{
    HuC6280 *c = &pce->cpu;
    hle_idle(pce, 14);
    c->in_block_transfer = 1;
    for (int i = 0; i < len; i++) {
        c->write(c->ctx, huc6280_phys(c, (uint16_t)(dst + i)), c->read(c->ctx, huc6280_phys(c, (uint16_t)(src + i))));
        hle_idle(pce, 6);
    }
    c->in_block_transfer = 0;
    hle_idle(pce, 3);
}

/* Scroll: VDC registers 7 (BXR) and 8 (BYR) from 4 bytes of RAM (ST0; TII; ST0; TII). */
static void write_scroll(Pce *pce, uint16_t bxr, uint16_t byr)
{
    hle_st(pce, 0, 0x07);
    tii_to_vdc(pce, bxr, 0x0002, 2);
    hle_st(pce, 0, 0x08);
    tii_to_vdc(pce, byr, 0x0002, 2);
}

/* ---- VDC (IRQ1): VBlank and line interrupt ---- */
/* Memory costs no separate time (hle_ram_free): the timing is that of the BIOS instructions, in order, and
 * VDC accesses cost their own (plus the wait) at the moment they happen. */

static void vdc_line(Pce *pce);

static void irq_vdc(Pce *pce)
{
    hle_ram_free(pce);
    if (z(pce, 0xF5) & 0x02) { /* the game handles it itself */
        hle_idle(pce, 6 + 2 + 7); /* BBS1, JMP ($2202) */
        pce->cpu.pc = hle_rd16(pce, 0x2202);
        return;
    }
    hle_idle(pce, 6 + 9); /* BBS1, PHA, PHX, PHY */
    save_registers(pce);
    uint8_t status = m(pce, 0x0000); /* reading the status acknowledges the VDC interrupts */
    zw(pce, 0xF6, status);
    hle_idle(pce, 4 + 6); /* STA $F6, BBR5 */
    if (!(status & 0x20)) {
        hle_idle(pce, 2);
        vdc_line(pce);
        return;
    }
    hle_st(pce, 0, 0x05);
    hle_idle(pce, 4); /* LDA $F3 */
    mw(pce, 0x0002, z(pce, 0xF3));
    hle_idle(pce, 4); /* LDA $F4 */
    mw(pce, 0x0003, z(pce, 0xF4));
    mw(pce, 0x2241, (uint8_t)(m(pce, 0x2241) + 1));
    hle_idle(pce, 7 + 6); /* INC $2241, BBS5 */
    if (z(pce, 0xF5) & 0x20) {
        hle_idle(pce, 2);
        pce->cpu.pc = 0xE8CD; /* skip the VBlank work */
        return;
    }
    write_scroll(pce, 0x220C, 0x2210);
    hle_idle(pce, 7); /* JSR E522 */
    bios_colorcmd(pce);
    mw(pce, 0x2249, (uint8_t)(m(pce, 0x2249) + 1));
    hle_idle(pce, 7 + 8); /* INC $2249, LDA $E7, CMP, BNE */
    A = z(pce, 0xE7);
    alu_cmp(pce, A, 0x01);
    if (A == 0x01) {
        hle_idle(pce, 7); /* JSR E6E8 */
        bios_call(pce, PSG_TICK, 0xE8CA);
        return;
    }
    hle_idle(pce, 2);
    pce->cpu.pc = 0xE8CA;
}

/* E8CA: reads the controls (may turn into a game reset): JSR E4B3 and the same timing as ex_joysns */
static void irq_vdc_pads(Pce *pce)
{
    if (bios_joysns(pce)) {
        hle_cycles(pce, 7 + 696);
        return;
    }
    hle_cycles(pce, 7 + 696);
    pce->cpu.pc = 0xE8CD;
}

/* E8CD: the game's VBlank hook, if requested; it returns (RTS) to E8D2 */
static void irq_vdc_vsync_hook(Pce *pce)
{
    hle_ram_free(pce);
    if (z(pce, 0xF5) & 0x10) {
        hle_idle(pce, 6 + 8 + 7); /* BBR4, BSR E883, JMP ($2208) */
        bios_call(pce, hle_rd16(pce, 0x2208), 0xE8D2);
        return;
    }
    hle_idle(pce, 6 + 2);
    pce->cpu.pc = 0xE8D2;
}

/* E8D2: line interrupt: scroll for the bottom part of the screen and line hook (returns to E8EF) */
static void vdc_line(Pce *pce)
{
    hle_ram_free(pce);
    hle_idle(pce, 6); /* BBR2 $F6 */
    if (z(pce, 0xF6) & 0x04) {
        hle_idle(pce, 6); /* BBS7 $F5 */
        if (!(z(pce, 0xF5) & 0x80))
            write_scroll(pce, 0x220E, 0x2212);
        else
            hle_idle(pce, 2);
        hle_idle(pce, 6); /* BBR6 $F5 */
        if (z(pce, 0xF5) & 0x40) {
            hle_idle(pce, 8 + 7); /* BSR E880, JMP ($220A) */
            bios_call(pce, hle_rd16(pce, 0x220A), 0xE8EF);
            return;
        }
    }
    hle_idle(pce, 2);
    pce->cpu.pc = 0xE8EF;
}

/* E8EF: restores the VDC register select and returns from the interrupt */
static void irq_vdc_end(Pce *pce)
{
    A = z(pce, 0xF7);
    mw(pce, 0x0000, A);
    restore_and_return(pce);
    hle_cycles(pce, 4 + 6 + 12 + 7); /* LDA $F7, STA $0000 (e a espera), PLY, PLX, PLA, RTI */
}

/* ---- Timer ---- */

static void irq_timer(Pce *pce)
{
    if (z(pce, 0xF5) & 0x04) { /* the game handles it itself */
        hle_cycles(pce, 15);
        pce->cpu.pc = hle_rd16(pce, 0x2204);
        return;
    }
    save_registers(pce);
    mw(pce, 0x1403, A); /* acknowledge the timer interrupt */
    if (z(pce, 0xE6)) { /* already playing (interrupt inside the interrupt) */
        restore_and_return(pce);
        hle_cycles(pce, 47);
        return;
    }
    zw(pce, 0xE6, (uint8_t)(z(pce, 0xE6) + 1));
    pce->cpu.p &= (uint8_t)~HU_FLAG_I; /* CLI: the VDC may interrupt the music driver */
    if (z(pce, 0xE7) == 0) {
        hle_cycles(pce, 48);
        bios_call(pce, PSG_TICK, 0xE6E2);
        return;
    }
    hle_cycles(pce, 42);
    pce->cpu.pc = 0xE6E2;
}

/* E6E2: end of the music step */
static void irq_timer_end(Pce *pce)
{
    zw(pce, 0xE6, 0);
    restore_and_return(pce);
    hle_cycles(pce, 23);
}

const HleRoutine hle_irq_routines[] = {
    {0xE889, "irq_vdc", irq_vdc,
     {{0xE8CA, irq_vdc_pads}, {0xE8CD, irq_vdc_vsync_hook}, {0xE8D2, vdc_line}, {0xE8EF, irq_vdc_end}}},
    {0xE6CC, "irq_timer", irq_timer, {{0xE6E2, irq_timer_end}}},
    {0, NULL, NULL, {{0}}},
};
