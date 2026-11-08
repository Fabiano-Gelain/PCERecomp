/*
 * Signature of the recompiled code that runs from RAM (CD games). See huc6280_recomp.h.
 */
#ifndef PCE_RECOMP_SIG_H
#define PCE_RECOMP_SIG_H

#include <stdint.h>

/* ---- Code that runs from RAM (CD games) ----
 * A function generated from a disc load is only valid if RAM still holds exactly those
 * bytes (the game swaps loads and sometimes modifies its own code). The function starts by checking
 * its "signature": the bytes of all its instructions, by physical address. If it does not match, it returns
 * without executing anything (the dispatcher tries another load or uses the interpreter).
 * To avoid comparing on every call, the result is cached along with the sum of the write counters
 * of the 256-byte RAM chunks involved (pce.h, ram_gen): if the sum changed, it compares again. */
typedef struct HuRamRun {
    uint32_t phys;          /* physical address (21 bits) */
    uint16_t len;
    const uint8_t *bytes;
} HuRamRun;

typedef struct HuRamSig {
    const HuRamRun *runs;
    uint16_t run_count;
    const uint16_t *pages;  /* 256-byte RAM chunks (index into ram_gen) */
    uint16_t page_count;
    uint64_t checked_sum;   /* sum of the counters at the last comparison (0 = never) */
    int checked_ok;
} HuRamSig;

/* Implemented by the console (pce.c): 1 if RAM holds the signature's bytes. */
int recomp_ram_sig_ok(void *ctx, HuRamSig *sig);
/* 1 if any RAM chunk of the signature was written since the last check. The generated code
 * calls this after every instruction that writes to memory: the game may be modifying its own
 * code (some games do); then the function returns to the dispatcher, which checks again. */
int recomp_ram_sig_changed(void *ctx, const HuRamSig *sig);
/* RAM byte at phys (no side effects): operand of an instruction the game modifies, read on the spot */
uint8_t recomp_ram_byte(void *ctx, uint32_t phys);

#endif
