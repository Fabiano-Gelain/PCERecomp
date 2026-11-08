/*
 * CD-ROM² System Card BIOS rewritten in C.
 *
 * The routines the game uses are replaced by C functions that do the same work and touch the same RAM
 * variables (the game reads BIOS variables directly). When the PC reaches a BIOS address that has a C
 * function (an entry of the $E000-$E0FF table, an interrupt vector, or a routine's wait point), the
 * dispatcher (pce_step) calls the function instead of executing the instruction.
 *
 * Each call is a "pseudo-instruction" (huc6280_hle_begin/end): the function reads and writes through the
 * CPU bus, adds up the cycles it spent (hle_cycles) and leaves the PC where the BIOS would be:
 *  - finished: does the RTS/RTI (hle_rts/hle_rti) and the PC returns to the game;
 *  - going to wait (e.g. for the next frame): leaves the PC at the address of the BIOS wait loop, which
 *    also has a C function; interrupts happen normally between one pseudo-instruction and the next;
 *  - going to call game code (interrupt hooks): pushes the return address of a BIOS address that has a
 *    C function and sets the PC to the game.
 * The addresses used are those of the real BIOS, so the stack comes out the same.
 *
 * Verification: tools/trace/bios_check.c runs the recorded sessions with the real BIOS and, at each call
 * of a routine from here, also runs the C version from the same state and compares everything.
 *
 * Behavior reference: the real BIOS code (System Card 2.0, US), studied instruction by instruction;
 * none of it is copied -- what is here is the C description of what each routine does.
 */
#ifndef PCE_BIOS_HLE_H
#define PCE_BIOS_HLE_H

#include <stdint.h>

struct Pce;

typedef void (*HleFn)(struct Pce *pce);

typedef struct Hle {
    HleFn fn[0x2000];   /* by logical address $E000-$FFFF (with BIOS bank 0 in window 7) */
    int cycles;         /* cycles of the pseudo-instruction in progress */
    int ram_free;       /* 1 = in this pseudo-instruction, memory accesses (not hardware) cost no time
                         * on their own: the routine adds the BIOS time with hle_cycles (see hle_psg.c) */
    uint64_t calls;     /* pseudo-instructions executed (diagnostics) */
    int synthetic;      /* synthetic BIOS (synth.c): BIOS address without a C routine = error */
    uint16_t missing;   /* first requested routine that does not exist in C (0 = none) */
    /* Time (CPU cycles) the BIOS spends in the body of the save routine in progress (hle_bram.c): with the
     * slow CPU, the body can take more than a frame, and that decides which VBlank the routine waits for at
     * the end. Kept here, not in a global variable, so two consoles in the same program (lockstep) do not
     * get mixed up */
    int bram_t;
    /* Time (CPU cycles) of the sound driver chunk in progress (hle_psg.c), summed block by block as in the
     * real BIOS */
    int psg_t;
    uint8_t font_fa; /* hle_font.c: low byte of the destination at the start of the 12x12 copy (timing depends on it) */
} Hle;

/* Registers all the rewritten routines. If `only` != 0, registers only the routine (and its wait
 * points) that starts at that address -- used by the verification, one routine at a time. */
void hle_init(Hle *hle, uint16_t only);
/* With the synthetic BIOS: also registers the initialization (hle_boot.c) and treats a BIOS address with
 * no C routine as an error (warns once and stops there). */
void hle_enable_synthetic(Hle *hle);
/* C function for the current PC, or NULL. */
HleFn hle_find(const Hle *hle, const struct Pce *pce);

/* ---- For the routines ---- */
uint8_t hle_rd(struct Pce *pce, uint16_t addr);            /* logical read, like the CPU (I/O has effects) */
void hle_wr(struct Pce *pce, uint16_t addr, uint8_t v);    /* logical write, like the CPU */
uint16_t hle_rd16(struct Pce *pce, uint16_t addr);
void hle_st(struct Pce *pce, int port, uint8_t v);         /* ST0 (port 0), ST1 (2), ST2 (3) */
/* Total time of the pseudo-instruction in CPU cycles (each access already spends its own; this tops it up). */
/* Memory accesses (not hardware) with no time of their own in this pseudo-instruction (Hle.ram_free). */
void hle_ram_free(struct Pce *pce);
void hle_cycles(struct Pce *pce, int cpu_cycles);
/* Immediately spends the time of instructions with no memory access (LDA #, ASL A, CLY, branches...), so
 * the following hardware accesses land on the real BIOS's cycle (VRAM waits). */
void hle_idle(struct Pce *pce, int cpu_cycles);
/* The pseudo-instruction is ONE BIOS instruction, of `cpu_cycles` cycles: finishes the timing and decides the
 * interrupt like the CPU (with the requests from the start of the instruction, or from the last hardware
 * access), not with those from the end. For wait loops done one instruction at a time (the interrupt
 * enters at the exact point of the loop). */
void hle_instr(struct Pce *pce, int cpu_cycles);
/* Same, with the current requests (before an access that already spends the instruction's time, e.g. TSB) */
void hle_irq_sample(struct Pce *pce);
/* Loops done one instruction per pseudo-instruction, like the CPU: hle_irq_sample at the start; accesses
 * that spend no time of their own (peek/poke; the VDC wait stays, and the interrupt then counts the requests
 * from after it); at the end, hle_next spends the instruction's time at once (the CD and ADPCM handle one
 * event per clock step, as on the CPU) and sets the PC to the next instruction. */
uint8_t hle_peek(struct Pce *pce, uint16_t addr);
void hle_poke(struct Pce *pce, uint16_t addr, uint8_t v);
void hle_next(struct Pce *pce, int cpu_cycles, uint16_t pc);
void hle_push(struct Pce *pce, uint8_t v);
uint8_t hle_pull(struct Pce *pce);
void hle_rts(struct Pce *pce);                              /* return from the call (JSR) */
void hle_rti(struct Pce *pce);                              /* return from the interrupt */
/* N and Z flags according to v (like LDA/CMP...). */
void hle_nz(struct Pce *pce, uint8_t v);
void hle_carry(struct Pce *pce, int set);

/* Glyph of a character from the C font (Shift-JIS), same as what ex_getfnt hands to the game: 16 rows
 * of 16 dots (bit 15 = left); small = 12x12. Returns 0 if the font has no such character (rows
 * empty). Also used by the PC version's options menu (same letters as the dialogs). */
int hle_font_render(uint16_t sjis, int small, uint16_t rows[16]);

/* BIOS variables in the zero page ($2000-$20FF) */
#define ZP(n) ((uint16_t)(0x2000 + (n)))

#endif
