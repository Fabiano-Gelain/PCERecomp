/*
 * Coverage recorder: records what the game actually executed, so the ROM analysis
 * (tools/recomp) knows for sure what is code and where the jumps that cannot be
 * deduced by reading the code go.
 *
 * It keeps:
 *  - each executed instruction: logical page (0-7 = the 8 KB window it was in),
 *    physical bank and offset within the bank;
 *  - each "interesting" control transfer, with source and destination:
 *      C = JSR/BSR   J = JMP abs   N = JMP (abs) / JMP (abs,X)   R = RTS   T = RTI
 *      K = BRK       I = interrupt (source = interrupted instruction)
 *  - S = instruction that starts in the last 6 bytes of a window and may continue into the next
 *    window: the destination says which bank was there (page + 1, bank, offset 0).
 *
 * File format (text, one line per item, numbers in hexadecimal):
 *   # pcerecomp coverage v1
 *   # rom-crc32 5F6F3C2A
 *   X <page> <bank> <offset>
 *   E <type> <page> <bank> <offset> <page> <bank> <offset>     (source -> destination)
 * Addresses only: the file does not contain any ROM byte.
 *
 * CD games (v2): the code that runs in RAM was loaded from the disc, and the same RAM address
 * receives different parts of the game at different times. Each instruction executed in RAM
 * ($80-$87 and $F8) also gets its origin on the disc (cd/origin.h):
 *   O <page> <bank> <offset> <disc position>   (position = sector * 2048 + byte, hex)
 *   O <page> <bank> <offset> -                 (bytes that did not come intact from the disc)
 * and the header counts how many RAM instructions had an origin and how many did not, and how many
 * bytes were checked against the disc (# ram-instruction-origin, # origin-bytes-checked). And each
 * load (a RAM span with consecutive
 * bytes from the disc) where code ran:
 *   L <start in RAM> <size> <disc position of the start>
 * Recorded without the BIOS file (C routines, bios/hle.h), the header has "# synthetic-bios": the ROM is the
 * synthetic BIOS, and the recompiler only uses the game code (the points in RAM).
 * (start in RAM: CD RAM 0-$FFFF, work RAM from $10000, as in cd/origin.h).
 */
#ifndef PCE_COVERAGE_H
#define PCE_COVERAGE_H

#include <stddef.h>
#include <stdint.h>

struct Pce;

typedef struct Coverage {
    uint8_t *exec;   /* 1 bit per (page, 21-bit physical address): 8 x 2 MB bits */
    uint64_t *edges; /* hash table of edges (0 = empty) */
    size_t edge_capacity, edge_count;
    uint64_t instructions;

    /* State of the instruction in progress */
    uint32_t src_key; /* page << 21 | physical address */
    uint8_t opcode;
    uint32_t irq_count;

    /* CD games: disc origin of the instructions executed in RAM (cd/origin.h) */
    struct Origin *origin;
    uint64_t *origins; /* hash table: point (24 bits) << 32 | disc position (0 = empty) */
    size_t origin_capacity, origin_count;
    uint64_t ram_with_origin, ram_without_origin; /* instructions executed in RAM */
    int synthetic_bios; /* recorded without the BIOS file (bios/synth.h): the ROM points belong to the synthetic BIOS */
    uint64_t *loads;   /* loads: start in RAM << 47 | size << 30 | disc position (0 = empty) */
    size_t load_capacity, load_count;
    uint32_t last_load_start, last_load_len, last_load_pos; /* last load found (avoids rescanning) */
} Coverage;

int coverage_init(Coverage *cov);
void coverage_free(Coverage *cov);

/* Call before and after each instruction. */
void coverage_before(Coverage *cov, struct Pce *pce);
void coverage_after(Coverage *cov, struct Pce *pce);

/* Writes the file; returns 0 on success. */
int coverage_write(const Coverage *cov, const char *path, uint32_t rom_crc32);

/* Counts (for tests and messages) */
size_t coverage_exec_count(const Coverage *cov);
int coverage_has_exec(const Coverage *cov, int page, uint32_t phys);
int coverage_has_edge(const Coverage *cov, char kind, int src_page, uint32_t src_phys, int dst_page, uint32_t dst_phys);

#endif
