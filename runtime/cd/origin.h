/*
 * Origin of RAM bytes (CD games): for each byte of CD RAM ($80-$87) and work RAM ($F8),
 * which disc position it came from, if it arrived intact. This lets coverage
 * (coverage.h) identify the code the game loads from the CD by its position on the disc -- stable,
 * unlike the RAM address, where different parts of the game take turns.
 *
 * How the bytes travel:
 *  - read from the drive ($1801/$1808 during the DATA IN phase): origin = sector * 2048 + byte;
 *  - registers A, X, Y keep the origin of the value they loaded (LDA/LDX/LDY, PLA/PLX/PLY,
 *    transfers TAX/TXA/...); STA/STX/STY/PHA/PHX/PHY store that origin into RAM;
 *  - block copies (TII, TDD, TIN, TIA, TAI): the byte read carries the origin to the following write;
 *  - any other write, or a register modified on its own (ADC, INC...), erases the origin.
 * An origin only holds while the register value is still the one that was loaded.
 *
 * Built-in check: when executing an instruction from RAM that has an origin, the RAM bytes are
 * compared with those of the disc at that position (failures counted in `mismatches`).
 */
#ifndef PCE_ORIGIN_H
#define PCE_ORIGIN_H

#include <stdint.h>

struct Pce;

#define ORIGIN_NONE 0xFFFFFFFFu   /* no origin on the disc (modified or created by the program) */
#define ORIGIN_RAM_SIZE (0x10000 + 0x2000)

typedef struct Origin {
    uint32_t ram[ORIGIN_RAM_SIZE]; /* origin + 1 per byte (0 = no origin): CD RAM, then work RAM */

    /* Instruction in progress */
    uint8_t opcode;
    int store_reg;            /* 0 = A, 1 = X, 2 = Y, -1 = the instruction does not store a register */
    int has_read;
    uint8_t read_value;
    uint32_t read_tag;
    uint8_t reg_value[3];
    uint32_t reg_tag[3];      /* origin + 1 of the value in A, X, Y (0 = none) */

    /* Check against the disc (one cached sector) */
    uint32_t cached_lba;
    int cached_valid;
    uint8_t sector[2048];
    uint64_t checked, mismatches;
} Origin;

void origin_init(Origin *o);
/* Index of the byte in ram[] for a physical address, or -1 if it is not tracked RAM. */
int origin_index(uint32_t phys);
void origin_before(Origin *o, struct Pce *pce, uint8_t opcode);
void origin_after(Origin *o, struct Pce *pce);
void origin_read(Origin *o, struct Pce *pce, uint32_t phys, uint8_t value);
void origin_write(Origin *o, struct Pce *pce, uint32_t phys, uint8_t value);
/* C BIOS routines (bios/hle.h), which do not go through the registers: the origin + 1 of the byte that
 * was just read from the CD data port (0 = not sector data) and the store of it into RAM at phys */
uint32_t origin_data_tag(struct Pce *pce);
void origin_mark(Origin *o, uint32_t phys, uint32_t tag);
/* Origin (position on the disc) of the instruction at pc, or ORIGIN_NONE; verifies the bytes against the disc. */
uint32_t origin_of_instruction(Origin *o, struct Pce *pce, uint16_t pc, int size);

#endif
