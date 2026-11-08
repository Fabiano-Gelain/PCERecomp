/*
 * HuC6280 CPU interpreter (65C02 + Hudson extensions).
 *
 * The CPU sees 64 KB of "logical" address space divided into 8 bank windows of 8 KB. Each window
 * points to an 8 KB physical bank via the MPR registers (21 bits = 2 MB).
 * The interpreter does this translation and calls the bus with the PHYSICAL address.
 *
 * Timing: huc6280_step returns the instruction's CPU cycles. If `clock` is
 * set, the CPU also advances the rest of the hardware, in master clock cycles
 * (12 per cycle in CSL, 3 in CSH), in the same order as Geargrafx: the instruction's base cycles
 * are delivered to the hardware at the END of it, but VDC/VCE accesses and
 * VRAM waits advance the hardware immediately (see huc6280_add_cycles etc.).
 * This matters for the exact moment an interrupt is noticed.
 *
 * Interrupts: sampled right after the opcode fetch (and again whenever the
 * hardware advances in the middle of the instruction); serviced at the end of the instruction.
 */
#ifndef PCE_HUC6280_H
#define PCE_HUC6280_H

#include <stdint.h>

#define HU_FLAG_C 0x01
#define HU_FLAG_Z 0x02
#define HU_FLAG_I 0x04
#define HU_FLAG_D 0x08
#define HU_FLAG_B 0x10
#define HU_FLAG_T 0x20
#define HU_FLAG_V 0x40
#define HU_FLAG_N 0x80

/* Interrupt lines (irq_request / irq_disable) */
#define HU_IRQ2 0x01  /* CD-ROM / BRK, vetor $FFF6 */
#define HU_IRQ1 0x02  /* VDC, vetor $FFF8 */
#define HU_TIQ  0x04  /* timer, vetor $FFFA */

/* Bus: 21-bit physical addresses (0x000000-0x1FFFFF). */
typedef uint8_t (*HuReadFn)(void *ctx, uint32_t addr);
typedef void (*HuWriteFn)(void *ctx, uint32_t addr, uint8_t value);
/* ST0/ST1/ST2: direct write to the VDC (port = 0, 2 or 3). On the console this is equivalent to
 * writing to physical address $1FE000 + port, regardless of the MPRs. */
typedef void (*HuVdcWriteFn)(void *ctx, int port, uint8_t value);
/* Advances the rest of the hardware by N master clock cycles. */
typedef void (*HuClockFn)(void *ctx, uint32_t master_cycles);

typedef struct HuC6280 {
    uint8_t a, x, y, s, p;
    uint16_t pc;
    uint8_t mpr[8];
    uint8_t mpr_buffer; /* last value passed by TAM/TMA (read by TMA #0) */
    uint8_t speed;      /* 0 = slow (CSL, 1.79 MHz), 1 = fast (CSH, 7.16 MHz) */
    uint64_t cycles;    /* total CPU cycles executed */

    /* Interrupts */
    uint8_t irq_request; /* active lines (HU_IRQ2 | HU_IRQ1 | HU_TIQ) */
    uint8_t irq_disable; /* register $1402: 1 = line disabled */
    uint8_t irq_pending; /* sample used at the end of the instruction */
    uint32_t irq_count;  /* interrupts serviced since reset (for diagnostics/coverage) */
    uint16_t irq_return_pc; /* PC pushed on the last interrupt */

    /* Timing of the current instruction */
    int instr_cycles;        /* CPU cycles counted so far */
    uint32_t extra_master;   /* extra master cycles (VRAM waits) */
    uint32_t clocked_master; /* master cycles already delivered to the hardware */
    uint8_t in_block_transfer;
    uint8_t hle_masked; /* C BIOS pseudo-instruction started with interrupts disabled */
    uint8_t hle_irq_snap; /* hle_instr: the interrupt is decided with hle_irq_req (requests at its start) */
    uint8_t hle_irq_req;

    void *ctx;
    HuReadFn read;
    HuWriteFn write;
    HuVdcWriteFn vdc_write; /* can be NULL */
    HuClockFn clock;        /* can be NULL (instruction tests) */

    /* Called before (after = 0) and after (after = 1) each instruction, by the interpreter and by the
     * recompiled code (trace, coverage). Can be NULL. */
    void (*hook)(void *ctx, int after);
    /* Request for the recompiled code to return control to the dispatcher at the end of the current
     * instruction (end of frame, trace instruction limit...). */
    uint8_t yield;
} HuC6280;

/* Reset state: reads the vector at $FFFE (MPR7 = 0 points to ROM bank 0). */
void huc6280_reset(HuC6280 *cpu);

/* Executes one whole instruction (block transfers included) and, if there is a
 * pending interrupt, enters it. Returns the number of CPU cycles spent. */
int huc6280_step(HuC6280 *cpu);

/* For the bus, during an instruction: */
/* adds N CPU cycles to the instruction and advances the hardware by them now (VDC/VCE wait) */
void huc6280_add_cycles(HuC6280 *cpu, int cpu_cycles);
/* advances the hardware by N CPU cycles already counted in the instruction (ST0-2) */
void huc6280_clock_counted(HuC6280 *cpu, int cpu_cycles);
/* advances the hardware until it catches up with the cycles counted so far (before writing to the PSG) */
void huc6280_clock_pending(HuC6280 *cpu);
/* 3 master cycle wait (CPU stalled waiting for VRAM) */
void huc6280_stall_fast_cycle(HuC6280 *cpu);
/* resamples the interrupts (call after changing irq_request/irq_disable) */
void huc6280_check_irqs(HuC6280 *cpu);
/* Size in bytes of the instruction with this opcode (1 to 7). */
int huc6280_instruction_size(uint8_t opcode);

/* Pseudo-instruction implemented in C (rewritten BIOS routines, runtime/bios/hle.h): begin sets up like
 * an instruction; the C code reads/writes through the CPU bus (c->read/c->write, which already spend the
 * VDC/VCE waits); end adds `cycles` CPU cycles, advances the hardware and services a pending interrupt,
 * as at the end of a normal instruction. Returns the pseudo-instruction's cycles. */
void huc6280_hle_begin(HuC6280 *cpu);
int huc6280_hle_end(HuC6280 *cpu, int cycles);

/* Logical -> physical translation through the MPRs. */
static inline uint32_t huc6280_phys(const HuC6280 *cpu, uint16_t addr)
{
    return ((uint32_t)cpu->mpr[addr >> 13] << 13) | (addr & 0x1FFFu);
}

#endif
