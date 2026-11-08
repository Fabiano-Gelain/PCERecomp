/*
 * Lockstep execution: one console running the recompiled code ("test") and another running only the
 * interpreter ("reference") run together, with the same buttons. After each instruction of the
 * test console, the reference executes one instruction and the CPU states are compared; at the end of each
 * frame, RAM, VRAM, sprite attribute table, VDC registers, palette, image and sound are compared too (and, in
 * CD games, CD RAM, save RAM and ADPCM RAM; the sound includes CD music and ADPCM).
 * On the first difference, the lockstep stops (asks the test console to yield) and builds a report.
 *
 * Usage:
 *   lockstep_init(&ls, &test, &reference);   (both already with the ROM loaded and reset)
 *   every frame: copy the buttons to both, pce_run_frame(&test, ...), lockstep_end_frame(&ls)
 *   ls.diverged != 0 -> ls.report has the description
 */
#ifndef PCE_LOCKSTEP_H
#define PCE_LOCKSTEP_H

#include <stdint.h>

#include "pce/pce.h"

#define LOCKSTEP_HISTORY 16

typedef struct LockstepCpu {
    uint16_t pc;
    uint8_t a, x, y, s, p, speed;
    uint8_t mpr[8];
    uint8_t irq_request;
    uint64_t cycles;
    uint64_t master_cycles;
} LockstepCpu;

typedef struct Lockstep {
    Pce *test;
    Pce *ref;
    uint64_t instructions; /* instructions compared */
    uint64_t frames;       /* frames compared */
    int diverged;
    char report[8192];

    LockstepCpu before;                      /* test console state before the current instruction */
    LockstepCpu history[LOCKSTEP_HISTORY];   /* last instructions (state before each one) */
    int history_pos;
} Lockstep;

void lockstep_init(Lockstep *ls, Pce *test, Pce *ref);
/* Call after each frame of the test console. Returns 0 if everything matches. */
int lockstep_end_frame(Lockstep *ls);
/* Writes the report to a file (if there was a divergence). */
int lockstep_write_report(const Lockstep *ls, const char *path);

#endif
