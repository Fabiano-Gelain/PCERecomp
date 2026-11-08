/*
 * Coverage recorder. See coverage.h.
 */
#include "pce/coverage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bios/hle.h"
#include "cd/origin.h"
#include "pce/pce.h"

#define EXEC_BITS (8u << 21)
#define EDGE_CAPACITY (1u << 20)

/* Key of a point: logical page (3 bits) + physical address (21 bits) = 24 bits. */
static uint32_t point_key(const HuC6280 *c, uint16_t pc)
{
    return ((uint32_t)(pc >> 13) << 21) | huc6280_phys(c, pc);
}

/* Edge: type (8 bits) | source (24) | destination (24). Types are letters, never 0. */
static uint64_t edge_key(char kind, uint32_t src, uint32_t dst)
{
    return ((uint64_t)(uint8_t)kind << 48) | ((uint64_t)src << 24) | dst;
}

static uint64_t hash64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33;
    return x;
}

static void add_edge(Coverage *cov, char kind, uint32_t src, uint32_t dst)
{
    uint64_t key = edge_key(kind, src, dst);
    size_t mask = cov->edge_capacity - 1;
    for (size_t i = hash64(key) & mask;; i = (i + 1) & mask) {
        if (cov->edges[i] == key)
            return;
        if (cov->edges[i] == 0) {
            if (cov->edge_count + 1 >= cov->edge_capacity / 2)
                return; /* table full: unlikely (~500 thousand distinct edges) */
            cov->edges[i] = key;
            cov->edge_count++;
            return;
        }
    }
}

/* Pair (RAM point, disc position); position ORIGIN_NONE = no origin. The key is never 0
 * because the RAM point has bank >= $80. */
static int insert_key(uint64_t *table, size_t capacity, size_t *count, uint64_t key);

static void add_origin(Coverage *cov, uint32_t point, uint32_t pos)
{
    insert_key(cov->origins, cov->origin_capacity, &cov->origin_count, ((uint64_t)point << 32) | pos);
}

static int insert_key(uint64_t *table, size_t capacity, size_t *count, uint64_t key)
{
    size_t mask = capacity - 1;
    for (size_t i = hash64(key) & mask;; i = (i + 1) & mask) {
        if (table[i] == key)
            return 0;
        if (table[i] == 0) {
            if (*count + 1 >= capacity / 2)
                return 0;
            table[i] = key;
            (*count)++;
            return 1;
        }
    }
}

/* The load (a RAM span with consecutive bytes from the disc) that contains byte `idx` of origin->ram. */
static void add_load(Coverage *cov, int idx)
{
    const uint32_t *ram = cov->origin->ram;
    uint32_t tag = ram[idx];
    uint32_t pos = tag - 1;
    if (cov->last_load_len && (uint32_t)idx >= cov->last_load_start &&
        (uint32_t)idx < cov->last_load_start + cov->last_load_len &&
        pos - cov->last_load_pos == (uint32_t)idx - cov->last_load_start &&
        ram[cov->last_load_start] == cov->last_load_pos + 1)
        return; /* same load as last time (and still intact at the start) */
    int lo_limit = idx < 0x10000 ? 0 : 0x10000;
    int hi_limit = idx < 0x10000 ? 0x10000 : ORIGIN_RAM_SIZE;
    int start = idx, end = idx + 1;
    while (start > lo_limit && ram[start - 1] != 0 && ram[start - 1] + 1 == ram[start])
        start--;
    while (end < hi_limit && ram[end] != 0 && ram[end] == ram[end - 1] + 1)
        end++;
    cov->last_load_start = (uint32_t)start;
    cov->last_load_len = (uint32_t)(end - start);
    cov->last_load_pos = ram[start] - 1;
    uint64_t key = ((uint64_t)start << 47) | ((uint64_t)(end - start) << 30) | (ram[start] - 1);
    insert_key(cov->loads, cov->load_capacity, &cov->load_count, key);
}

int coverage_init(Coverage *cov)
{
    memset(cov, 0, sizeof(*cov));
    cov->exec = (uint8_t *)calloc(EXEC_BITS / 8, 1);
    cov->edges = (uint64_t *)calloc(EDGE_CAPACITY, sizeof(uint64_t));
    cov->edge_capacity = EDGE_CAPACITY;
    cov->origins = (uint64_t *)calloc(EDGE_CAPACITY, sizeof(uint64_t));
    cov->origin_capacity = EDGE_CAPACITY;
    cov->loads = (uint64_t *)calloc(4096, sizeof(uint64_t));
    cov->load_capacity = 4096;
    if (!cov->exec || !cov->edges || !cov->origins || !cov->loads) {
        coverage_free(cov);
        return -1;
    }
    return 0;
}

void coverage_free(Coverage *cov)
{
    free(cov->exec);
    free(cov->edges);
    free(cov->origins);
    free(cov->loads);
    free(cov->origin);
    cov->loads = NULL;
    cov->exec = NULL;
    cov->edges = NULL;
    cov->origins = NULL;
    cov->origin = NULL;
}

/* Reads a byte without side effects: any bank except the I/O one (which returns $FF). Includes
 * CD RAM and save RAM, where CD games also execute code. */
static uint8_t peek(Pce *pce, uint32_t phys)
{
    if ((phys >> 13) != PCE_IO_BANK)
        return pce_bus_read(pce, phys);
    return 0xFF;
}

void coverage_before(Coverage *cov, Pce *pce)
{
    const HuC6280 *c = &pce->cpu;
    cov->src_key = point_key(c, c->pc);
    cov->exec[cov->src_key >> 3] |= (uint8_t)(1u << (cov->src_key & 7));
    cov->opcode = peek(pce, huc6280_phys(c, c->pc));
    cov->irq_count = c->irq_count;
    if (pce->cd) {
        /* CD game: turns on origin tracking of the RAM bytes at the first instruction. */
        if (!cov->origin) {
            cov->origin = (Origin *)malloc(sizeof(Origin));
            if (cov->origin)
                origin_init(cov->origin);
            pce->origin = cov->origin;
            cov->synthetic_bios = pce->hle && pce->hle->synthetic;
        }
        if (cov->origin) {
            int idx = origin_index(huc6280_phys(c, c->pc));
            if (idx >= 0) {
                uint32_t pos = origin_of_instruction(cov->origin, pce, c->pc, huc6280_instruction_size(cov->opcode));
                if (pos == ORIGIN_NONE) {
                    cov->ram_without_origin++;
                } else {
                    cov->ram_with_origin++;
                    add_load(cov, idx);
                }
                add_origin(cov, cov->src_key, pos);
            }
            origin_before(cov->origin, pce, cov->opcode);
        }
    }
    /* An instruction has up to 7 bytes: if it starts near the end of the window, record the next bank. */
    if ((c->pc & 0x1FFF) >= 0x1FFA) {
        uint16_t next = (uint16_t)((c->pc | 0x1FFF) + 1);
        add_edge(cov, 'S', cov->src_key, point_key(c, next));
    }
    cov->instructions++;
}

void coverage_after(Coverage *cov, Pce *pce)
{
    const HuC6280 *c = &pce->cpu;
    /* If there was an interrupt, the instruction's own destination is the PC that was pushed. */
    int irq = c->irq_count != cov->irq_count;
    uint16_t own_dst = irq ? c->irq_return_pc : c->pc;

    char kind = 0;
    switch (cov->opcode) {
    case 0x20: case 0x44: kind = 'C'; break;
    case 0x4C: kind = 'J'; break;
    case 0x6C: case 0x7C: kind = 'N'; break;
    case 0x60: kind = 'R'; break;
    case 0x40: kind = 'T'; break;
    case 0x00: kind = 'K'; break;
    default: break;
    }
    if (kind)
        add_edge(cov, kind, cov->src_key, point_key(c, own_dst));
    if (irq)
        add_edge(cov, 'I', cov->src_key, point_key(c, c->pc));
    if (cov->origin)
        origin_after(cov->origin, pce);
}

size_t coverage_exec_count(const Coverage *cov)
{
    size_t n = 0;
    for (size_t i = 0; i < EXEC_BITS / 8; i++)
        for (uint8_t b = cov->exec[i]; b; b &= (uint8_t)(b - 1))
            n++;
    return n;
}

int coverage_has_exec(const Coverage *cov, int page, uint32_t phys)
{
    uint32_t key = ((uint32_t)page << 21) | (phys & 0x1FFFFF);
    return (cov->exec[key >> 3] >> (key & 7)) & 1;
}

int coverage_has_edge(const Coverage *cov, char kind, int src_page, uint32_t src_phys, int dst_page, uint32_t dst_phys)
{
    uint64_t key = edge_key(kind, ((uint32_t)src_page << 21) | src_phys, ((uint32_t)dst_page << 21) | dst_phys);
    for (size_t i = 0; i < cov->edge_capacity; i++)
        if (cov->edges[i] == key)
            return 1;
    return 0;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void write_point(FILE *f, uint32_t key)
{
    fprintf(f, " %X %02X %04X", key >> 21, (key >> 13) & 0xFF, key & 0x1FFF);
}

int coverage_write(const Coverage *cov, const char *path, uint32_t rom_crc32)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "# pcerecomp coverage %s\n# rom-crc32 %08X\n# executed-instructions %llu\n",
            cov->origin ? "v2" : "v1", rom_crc32, (unsigned long long)cov->instructions);
    if (cov->synthetic_bios)
        fprintf(f, "# synthetic-bios\n");
    if (cov->origin)
        fprintf(f, "# ram-instruction-origin %llu from disc, %llu without\n"
                   "# origin-bytes-checked %llu, differing from disc %llu\n",
                (unsigned long long)cov->ram_with_origin, (unsigned long long)cov->ram_without_origin,
                (unsigned long long)cov->origin->checked, (unsigned long long)cov->origin->mismatches);

    for (uint32_t key = 0; key < EXEC_BITS; key++) {
        if (!cov->exec[key >> 3]) {
            key |= 7; /* whole byte empty: skip */
            continue;
        }
        if ((cov->exec[key >> 3] >> (key & 7)) & 1) {
            fputc('X', f);
            write_point(f, key);
            fputc('\n', f);
        }
    }

    uint64_t *sorted = (uint64_t *)malloc(cov->edge_count * sizeof(uint64_t) + 1);
    size_t n = 0;
    for (size_t i = 0; i < cov->edge_capacity; i++)
        if (cov->edges[i])
            sorted[n++] = cov->edges[i];
    qsort(sorted, n, sizeof(uint64_t), cmp_u64);
    for (size_t i = 0; i < n; i++) {
        fprintf(f, "E %c", (char)(sorted[i] >> 48));
        write_point(f, (uint32_t)(sorted[i] >> 24) & 0xFFFFFF);
        write_point(f, (uint32_t)sorted[i] & 0xFFFFFF);
        fputc('\n', f);
    }
    free(sorted);

    if (cov->origin_count) {
        sorted = (uint64_t *)malloc(cov->origin_count * sizeof(uint64_t));
        n = 0;
        for (size_t i = 0; i < cov->origin_capacity; i++)
            if (cov->origins[i])
                sorted[n++] = cov->origins[i];
        qsort(sorted, n, sizeof(uint64_t), cmp_u64);
        for (size_t i = 0; i < n; i++) {
            uint32_t pos = (uint32_t)sorted[i];
            fputc('O', f);
            write_point(f, (uint32_t)(sorted[i] >> 32));
            if (pos == ORIGIN_NONE)
                fputs(" -\n", f);
            else
                fprintf(f, " %X\n", pos);
        }
        free(sorted);
    }
    if (cov->load_count) {
        sorted = (uint64_t *)malloc(cov->load_count * sizeof(uint64_t));
        n = 0;
        for (size_t i = 0; i < cov->load_capacity; i++)
            if (cov->loads[i])
                sorted[n++] = cov->loads[i];
        qsort(sorted, n, sizeof(uint64_t), cmp_u64);
        for (size_t i = 0; i < n; i++)
            fprintf(f, "L %X %X %X\n", (unsigned)(sorted[i] >> 47), (unsigned)((sorted[i] >> 30) & 0x1FFFF),
                    (unsigned)(sorted[i] & 0x3FFFFFFF));
        free(sorted);
    }
    fclose(f);
    return 0;
}
