/*
 * Table of the BIOS routines rewritten in C (see hle.h). Each routine has its entry (address in the
 * $E000-$E0FF table, or the interrupt vector) and the other points where it may be when the
 * dispatcher calls it again (wait loops, return from game code).
 */
#ifndef PCE_BIOS_ROUTINES_H
#define PCE_BIOS_ROUTINES_H

#include "bios/hle.h"

#define HLE_MAX_POINTS 16

typedef struct HlePoint {
    uint16_t addr;
    HleFn fn;
} HlePoint;

typedef struct HleRoutine {
    uint16_t entry;
    const char *name;
    HleFn fn;
    HlePoint points[HLE_MAX_POINTS];
    /* 1 = internal part of the BIOS (e.g. sending commands to the CD), called only by the C routines:
     * always registered, even when the verification picks a single routine. */
    int internal;
    /* Other addresses (0-terminated) handled by the same function `fn`, one instruction each (hle_cd.c) */
    const uint16_t *ops;
} HleRoutine;

/* One group per file (each table ends with entry = 0); hle_groups ends with NULL. */
extern const HleRoutine hle_ex_routines[];
extern const HleRoutine hle_math_routines[];
extern const HleRoutine hle_irq_routines[];
extern const HleRoutine hle_cd_routines[];
extern const HleRoutine hle_adpcm_routines[];
extern const HleRoutine hle_bram_routines[];
extern const HleRoutine hle_font_routines[];
extern const HleRoutine hle_psg_routines[];
extern const HleRoutine *const hle_groups[];

/* Name of the routine with this entry, or NULL. */
const char *hle_routine_name(uint16_t entry);

#endif
