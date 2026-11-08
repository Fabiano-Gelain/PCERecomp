/*
 * Profile of the BIOS (System Card) routines that a CD game uses (to support rewriting those routines
 * in C). While the game runs, it tracks:
 *  - each call from the game code to an entry of the BIOS table ($E000-$E0FF), until it returns;
 *  - each interrupt serviced by the BIOS, until the RTI;
 * and, for each routine, records what the BIOS's own instructions did during the call (game code
 * called from inside it, e.g. interrupt hooks, does not count): how many times it ran, how many
 * instructions, which work RAM bytes it wrote, how much it wrote to CD RAM and to save RAM, which
 * hardware registers it used (VDC, VCE, PSG, timer, pad, interrupts, CD) and which windows (MPR) it
 * switched.
 *
 * Output (text; one line per routine, only addresses and counts - none of the BIOS code):
 *   R <type> <address> calls=N instructions=N ram=<ranges> cdram=N bram=N vdc=<regs> vce=N psg=N
 *     timer=N joypad=N irq=N cd=<regs> mpr=<windows>
 *   type: C = call through the table, I = interrupt, B = initialization (from reset until the first
 *   game instruction). RAM ranges: "2000-2003,2240".
 * tools/recomp/bios_report.py merges the files and adds the routine names.
 */
#ifndef PCE_BIOS_PROFILE_H
#define PCE_BIOS_PROFILE_H

#include <stdint.h>

struct Pce;

#define BIOS_PROFILE_SLOTS 64
#define BIOS_PROFILE_DEPTH 8

typedef struct BiosRoutine {
    char kind;          /* 'C', 'I' or 'B'; 0 = free */
    uint16_t address;
    uint64_t calls, instructions;
    uint8_t ram[0x2000 / 8];   /* work RAM bytes written (1 bit each) */
    uint64_t cdram, bram, vce, psg, timer, pad, irq;
    uint32_t vdc_regs;         /* VDC registers written (bit n = register n) */
    uint16_t cd_regs;          /* registers $1800-$180F written */
    uint8_t mpr;               /* windows switched */
} BiosRoutine;

typedef struct BiosProfile {
    BiosRoutine routines[BIOS_PROFILE_SLOTS];
    int depth;
    struct { BiosRoutine *r; uint8_t s; } stack[BIOS_PROFILE_DEPTH];
    uint32_t irq_count;
    uint8_t mpr_before[8];
    uint8_t vdc_selected;      /* selected VDC register ($0000) */
    int in_bios;               /* the current instruction belongs to the BIOS */
} BiosProfile;

void bios_profile_init(BiosProfile *bp, struct Pce *pce);
void bios_profile_before(BiosProfile *bp, struct Pce *pce);
void bios_profile_after(BiosProfile *bp, struct Pce *pce);
void bios_profile_write(BiosProfile *bp, uint32_t phys, uint8_t value);
void bios_profile_vdc(BiosProfile *bp, int port, uint8_t value);
int bios_profile_save(const BiosProfile *bp, const char *path);

#endif
