/*
 * C BIOS: console initialization with the synthetic BIOS (synth.c), without the System Card.
 *
 * Does what the BIOS does at power-on, minus the System Card title screen: clears the RAM, sets the
 * initial values of the BIOS variables, sets up CD, ADPCM, sound, video and controls; reads the disc's
 * TOC, finds the first data track, reads its first 2 sectors into $2800 and calls the disc's own boot
 * program ($2B26), which loads the game and jumps to it.
 *
 * The steps that wait (on the CD drive) are calls to the C routines, with the return at private points
 * ($FE90-$FE9F, a BIOS area that is never executed).
 */
#include <stdio.h>
#include <string.h>

#include "bios/alu.h"
#include "bios/hle.h"
#include "bios/internal.h"
#include "bios/routines.h"
#include "bios/synth.h"

#define A (pce->cpu.a)
#define X (pce->cpu.x)
#define Y (pce->cpu.y)

static uint8_t m(Pce *pce, uint16_t a) { return hle_rd(pce, a); }
static void mw(Pce *pce, uint16_t a, uint8_t v) { hle_wr(pce, a, v); }
static void zw(Pce *pce, uint8_t n, uint8_t v) { hle_wr(pce, ZP(n), v); }
static void call(Pce *pce, uint16_t target, uint16_t ret) { bios_call(pce, target, ret); }

#define P_BOOT 0xFE90

static void tam(Pce *pce, int win, uint8_t v)
{
    pce->cpu.mpr[win] = v;
    pce->cpu.mpr_buffer = v;
}

/* Something went wrong (disc with no data track, read error, disc boot refused): stay stuck */
static void boot_fail(Pce *pce)
{
    if (pce->hle && !pce->hle->missing) {
        pce->hle->missing = 0xE0F3;
        fprintf(stderr, "Synthetic BIOS: the disc failed to initialize (step $%04X)\n", pce->cpu.pc);
    }
    hle_cycles(pce, 100);
    pce->cpu.pc = P_BOOT + 15;
}

static void boot_reset(Pce *pce)
{
    HuC6280 *c = &pce->cpu;
    c->p = (uint8_t)((c->p | HU_FLAG_I) & ~HU_FLAG_D);
    c->speed = 1;
    c->s = 0xFF;
    tam(pce, 0, 0xFF);
    tam(pce, 1, 0xF8);
    for (int w = 2; w <= 6; w++)
        tam(pce, w, (uint8_t)(0x80 + w - 2)); /* CD RAM in windows 2-6, like the BIOS */
    memset(pce->ram, 0, 0x100);
    memset(pce->ram + 0x200, 0, 0x1E00);
    /* Initial values: game reset = this initialization; interrupt hooks = RTI */
    mw(pce, 0x2284, 0xF3);
    mw(pce, 0x2285, 0xE0);
    zw(pce, 0xE7, 0x80);
    for (int i = 0; i < 12; i += 2) {
        mw(pce, (uint16_t)(0x2200 + i), 0xF7);
        mw(pce, (uint16_t)(0x2201 + i), 0xE8);
    }
    /* CD and ADPCM */
    mw(pce, 0x1802, 0x00);
    mw(pce, 0x180F, 0x00);
    mw(pce, 0x180D, 0x80);
    mw(pce, 0x180D, 0x00);
    mw(pce, 0x180B, 0x00);
    bios_trb(pce, 0x1802, 0x6F);
    mw(pce, 0x180E, 0x00);
    /* Sound, timer, interrupts, controller */
    mw(pce, 0x0800, 0x00);
    mw(pce, 0x0801, 0x00);
    mw(pce, 0x0C00, 0x00);
    mw(pce, 0x0C01, 0x00);
    mw(pce, 0x1403, 0x00);
    mw(pce, 0x1000, 0x00);
    mw(pce, 0x1402, 0x00);
    /* Video: MWR, sprite table at $1000, controls that accept reset */
    mw(pce, 0x2242, 0x10);
    mw(pce, 0x2214, 0x00);
    mw(pce, 0x2215, 0x10);
    mw(pce, 0x2227, 0x1F);
    /* VRAM $0000-$0FFF filled with $0200, like the BIOS (the rest is left as is) */
    mw(pce, 0x0000, 0x00);
    mw(pce, 0x0002, 0x00);
    mw(pce, 0x0003, 0x00);
    mw(pce, 0x0000, 0x02);
    for (int i = 0; i < 0x1000; i++) {
        mw(pce, 0x0002, 0x00);
        mw(pce, 0x0003, 0x02);
    }
    hle_cycles(pce, 200);
    call(pce, 0xE8FA, P_BOOT); /* restarts the CD drive */
}

static void boot_video(Pce *pce)
{
    A = 0x00;
    X = 0x20;
    Y = 0x1E;
    hle_cycles(pce, 10);
    call(pce, 0xE06F, P_BOOT + 1); /* ex_scrmod: 256 x 240 */
}

static void boot_scrsiz(Pce *pce)
{
    A = 0x00;
    hle_cycles(pce, 10);
    call(pce, 0xE069, P_BOOT + 2); /* ex_scrsiz */
}

static void boot_satclr(Pce *pce)
{
    hle_cycles(pce, 10);
    call(pce, 0xE0A2, P_BOOT + 3); /* ex_satclr */
}

static void boot_sprdma(Pce *pce)
{
    hle_cycles(pce, 10);
    call(pce, 0xE09F, P_BOOT + 4); /* ex_sprdma */
}

static void boot_irqon(Pce *pce)
{
    pce->cpu.p &= (uint8_t)~HU_FLAG_I;
    hle_cycles(pce, 10);
    call(pce, 0xE084, P_BOOT + 5); /* ex_irqon: VBlank */
}

/* Disc TOC: first and last track ($226A/$226B), end of disc ($226C-$226E) */
static void boot_toc(Pce *pce)
{
    zw(pce, 0xF8, 0x00);
    zw(pce, 0xFA, 0x56);
    zw(pce, 0xFB, 0x22);
    hle_cycles(pce, 30);
    call(pce, 0xF00A, P_BOOT + 6);
}

static void boot_toc_tracks(Pce *pce)
{
    if (A) {
        boot_fail(pce);
        return;
    }
    mw(pce, 0x226A, m(pce, 0x2256));
    mw(pce, 0x226B, m(pce, 0x2257));
    zw(pce, 0xF8, 0x01);
    zw(pce, 0xFA, 0x56);
    zw(pce, 0xFB, 0x22);
    hle_cycles(pce, 50);
    call(pce, 0xF00A, P_BOOT + 7);
}

static void boot_toc_end(Pce *pce)
{
    if (A) {
        boot_fail(pce);
        return;
    }
    for (int i = 0; i < 3; i++)
        mw(pce, (uint16_t)(0x226C + i), m(pce, (uint16_t)(0x2256 + i)));
    mw(pce, 0x2269, m(pce, 0x226A));
    hle_cycles(pce, 50);
    pce->cpu.pc = P_BOOT + 8;
}

/* Looks for the first data track (bit 2 of the 4th byte of the TOC entry) */
static void boot_track(Pce *pce)
{
    zw(pce, 0xF9, m(pce, 0x2269));
    zw(pce, 0xF8, 0x02);
    zw(pce, 0xFA, 0x56);
    zw(pce, 0xFB, 0x22);
    hle_cycles(pce, 40);
    call(pce, 0xF00A, P_BOOT + 9);
}

static void boot_track_checked(Pce *pce)
{
    if (A) {
        boot_fail(pce);
        return;
    }
    if (!(m(pce, 0x2259) & 0x04)) {
        uint8_t t = m(pce, 0x2269);
        t = (uint8_t)((t & 0x0F) == 9 ? (t & 0xF0) + 0x10 : t + 1); /* BCD + 1 */
        if (t > m(pce, 0x226B)) {
            boot_fail(pce);
            return;
        }
        mw(pce, 0x2269, t);
        hle_cycles(pce, 30);
        pce->cpu.pc = P_BOOT + 8;
        return;
    }
    /* Track start = base of the reads; 2 sectors from its beginning into $2800 */
    bios_msf_to_lba(pce);
    for (int i = 0; i < 3; i++) {
        uint8_t v = m(pce, ZP((uint8_t)(0xF8 + i)));
        mw(pce, (uint16_t)(0x2274 + i), v);
        mw(pce, (uint16_t)(0x2277 + i), v);
    }
    zw(pce, 0xFC, 0x00);
    zw(pce, 0xFD, 0x00);
    zw(pce, 0xFE, 0x00);
    zw(pce, 0xFA, 0x00);
    zw(pce, 0xFB, 0x28);
    zw(pce, 0xFF, 0x01);
    zw(pce, 0xF8, 0x02);
    zw(pce, 0xF9, 0x00);
    hle_cycles(pce, 600);
    call(pce, 0xE009, P_BOOT + 10);
}

/* The disc's boot program checks the license text ($3020, 74 bytes) against the BIOS: the synthetic
 * BIOS supplies the text from the disc itself. Bank $1F in window 6, as the BIOS leaves it. */
static void boot_start_disc(Pce *pce)
{
    if (A) {
        boot_fail(pce);
        return;
    }
    if (pce->rom && pce->rom_size >= BIOS_SYNTH_SIZE)
        bios_synth_copyright(pce->rom, pce->ram + 0x1020, 0x4A);
    tam(pce, 6, 0x1F);
    hle_cycles(pce, 40);
    call(pce, 0x2B26, P_BOOT + 11);
}

static void boot_disc_returned(Pce *pce) { boot_fail(pce); }

static void boot_halt(Pce *pce)
{
    hle_cycles(pce, 100);
    pce->cpu.pc = P_BOOT + 15;
}

const HleRoutine hle_boot_routines[] = {
    {0xE0F3, "boot", boot_reset,
     {{P_BOOT, boot_video}, {P_BOOT + 1, boot_scrsiz}, {P_BOOT + 2, boot_satclr}, {P_BOOT + 3, boot_sprdma},
      {P_BOOT + 4, boot_irqon}, {P_BOOT + 5, boot_toc}, {P_BOOT + 6, boot_toc_tracks},
      {P_BOOT + 7, boot_toc_end}}},
    {P_BOOT + 8, "boot_track", boot_track,
     {{P_BOOT + 9, boot_track_checked}, {P_BOOT + 10, boot_start_disc}, {P_BOOT + 11, boot_disc_returned},
      {P_BOOT + 15, boot_halt}}},
    {0, NULL, NULL, {{0}}, 0},
};
