/*
 * bios_check: check of the BIOS routines rewritten in C (runtime/bios) against the real BIOS.
 *
 * Replays a session (button script) with the real BIOS, in the interpreter only. Each time the game
 * calls a routine that has a C version (and is in the list), the console is copied ("snapshot"); the real BIOS
 * continues until the routine returns to the game; then the copy runs the C version from the snapshot, until it
 * returns too, and the two results are compared:
 *   - registers (A, X, Y, P, S, PC, MPR);
 *   - work RAM (in the stack page, only the part in use, above S), CD RAM, save memory,
 *     ADPCM RAM;
 *   - VRAM, sprite table, VDC registers, VCE palette;
 *   - the sequence of hardware writes (I/O page and ST0-2), in order.
 * The time spent (cycles) is compared for information only (average and, for calls without an interrupt in the
 * middle, how many took exactly the same): a different time does not fail, but it changes when the game draws
 * (e.g. a dialog window switch).
 *
 * Usage: bios_check [--input <session>] [--bram <save>] [--routines E069,E07B|all] [--max N]
 *                   --bios <system card> <image.cue> <frames>
 *      --max: at most N checks per routine (default 2000); the rest are only counted.
 *      --timings N: lists the first N calls of each routine with a different time (frame, registers).
 *      --pieces: the first sound driver piece with a different duration.
 *      --detail E0D8:11: everything about the 11th call of $E0D8.
 * Output: one summary per routine; exit code 0 = all equal, 1 = some difference.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bios/hle.h"
#include "bios/routines.h"
#include "cd/cdrom.h"
#include "input_script.h"
#include "pce/pce.h"

#define MAX_ROUTINES 128
#define REF_LIMIT 50000000ull /* real BIOS instructions in one call before giving up */

typedef struct Stat {
    uint16_t entry;
    const char *name;
    int selected;
    uint64_t calls, checked, equal, different, with_irq, unfinished, sensitive;
    double ref_cycles, our_cycles;
    /* Time, only for calls without an interrupt in the middle: how many took the same as the BIOS, the smallest and
     * largest difference (ours - BIOS, master cycles) and the first call with a different time */
    uint64_t time_checked, time_same, first_time_call;
    int64_t dmin, dmax, first_ref, first_our;
    uint64_t piece_checked, piece_same; /* --pieces */
    char piece_diff[3][256];
    int piece_diff_count;
    char first_diff[3][512];
    int diff_count;
} Stat;

static Stat g_stats[MAX_ROUTINES];
static int g_stat_count;
static int g_max = 2000;

static Pce g_pce;           /* reference: real BIOS */
static InputScript g_input;
static Hle g_all;           /* only to know which addresses have a C version */
static IoLog g_ref_log, g_our_log;

/* Check in progress */
static int g_pending;
static Stat *g_pending_stat;
static Pce g_clone;
static uint8_t g_entry_s;
static uint32_t g_entry_irqs;
static uint64_t g_entry_master, g_ref_steps;
static int g_prev_game; /* the previous instruction belonged to the game (not the BIOS) */

static Stat *stat_for(uint16_t entry)
{
    for (int i = 0; i < g_stat_count; i++)
        if (g_stats[i].entry == entry)
            return &g_stats[i];
    return NULL;
}

static int bios_code(const Pce *pce)
{
    return pce->cpu.mpr[pce->cpu.pc >> 13] < 0x80;
}

static int returned(const Pce *pce, uint8_t entry_s)
{
    return !bios_code(pce) && (int8_t)(pce->cpu.s - entry_s) > 0;
}

/* ---- Comparison ---- */

static int diff_bytes(char *out, size_t n, const char *what, const uint8_t *a, const uint8_t *b, size_t size,
                      uint32_t base)
{
    for (size_t i = 0; i < size; i++) {
        if (a[i] != b[i]) {
            snprintf(out, n, "%s differs at $%04X: BIOS %02X, ours %02X", what, (unsigned)(base + i), a[i], b[i]);
            return 1;
        }
    }
    return 0;
}

static int diff_words(char *out, size_t n, const char *what, const uint16_t *a, const uint16_t *b, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (a[i] != b[i]) {
            snprintf(out, n, "%s differs at %zu: BIOS %04X, ours %04X", what, i, a[i], b[i]);
            return 1;
        }
    }
    return 0;
}

/* with_irq: there was an interrupt in the middle of the routine (in the real BIOS). Then the order of the hardware
 * writes depends on when the interrupt came in, and only the final state is compared. */
/* Bytes the check does not compare in this call (work RAM and CD RAM, by physical address in the
 * page). Only ex_getfnt uses it: the glyph shapes come from our font (bios/font.c), not from the BIOS
 * font, and the temporary variables hold positions in the font. */
#define MAX_IGNORE 8
typedef struct IgnoreRange {
    int cd;
    uint32_t from, to;
} IgnoreRange;
static IgnoreRange g_ignore[MAX_IGNORE];
static int g_ignore_count;

static void ignore_logical(const Pce *p, uint16_t addr, int len)
{
    for (int i = 0; i < len && g_ignore_count < MAX_IGNORE; i++) {
        uint16_t a = (uint16_t)(addr + i);
        uint8_t bank = p->cpu.mpr[a >> 13];
        int cd = bank >= 0x80 && bank <= 0x87;
        if (bank != 0xF8 && !cd)
            continue;
        uint32_t phys = cd ? (uint32_t)(bank - 0x80) * 0x2000 + (a & 0x1FFF) : (uint32_t)(a & 0x1FFF);
        if (g_ignore_count && g_ignore[g_ignore_count - 1].cd == cd && g_ignore[g_ignore_count - 1].to == phys)
            g_ignore[g_ignore_count - 1].to = phys + 1;
        else
            g_ignore[g_ignore_count++] = (IgnoreRange){cd, phys, phys + 1};
    }
}

static void apply_ignore(uint8_t *ra, uint8_t *rb, int cd)
{
    for (int i = 0; i < g_ignore_count; i++)
        if (g_ignore[i].cd == cd)
            for (uint32_t k = g_ignore[i].from; k < g_ignore[i].to; k++)
                ra[k] = rb[k] = 0;
}

static int compare(char *out, size_t n, const Pce *r, const Pce *o, int with_irq)
{
    const HuC6280 *a = &r->cpu, *b = &o->cpu;
    if (a->a != b->a || a->x != b->x || a->y != b->y || a->p != b->p || a->s != b->s || a->pc != b->pc ||
        memcmp(a->mpr, b->mpr, 8) != 0) {
        snprintf(out, n,
                 "registers: BIOS A=%02X X=%02X Y=%02X P=%02X S=%02X PC=%04X M=%02X..%02X | "
                 "ours A=%02X X=%02X Y=%02X P=%02X S=%02X PC=%04X M=%02X..%02X",
                 a->a, a->x, a->y, a->p, a->s, a->pc, a->mpr[0], a->mpr[7], b->a, b->x, b->y, b->p, b->s, b->pc,
                 b->mpr[0], b->mpr[7]);
        return 1;
    }
    /* Work RAM: stack page only above S (below is garbage from pushes). */
    uint8_t ra[0x2000], rb[0x2000];
    memcpy(ra, r->ram, sizeof(ra));
    memcpy(rb, o->ram, sizeof(rb));
    for (int i = 0x100; i <= 0x100 + a->s; i++)
        ra[i] = rb[i] = 0;
    apply_ignore(ra, rb, 0);
    if (diff_bytes(out, n, "RAM", ra, rb, sizeof(ra), 0x2000))
        return 1;
    if (r->cd && o->cd) {
        static uint8_t ca[sizeof(r->cd->ram)], cb[sizeof(r->cd->ram)];
        memcpy(ca, r->cd->ram, sizeof(ca));
        memcpy(cb, o->cd->ram, sizeof(cb));
        apply_ignore(ca, cb, 1);
        if (diff_bytes(out, n, "CD RAM", ca, cb, sizeof(ca), 0) ||
            diff_bytes(out, n, "save memory", r->cd->bram, o->cd->bram, sizeof(r->cd->bram), 0))
            return 1;
        /* ADPCM DMA in progress (background copy from the CD): the bytes between the two write
         * addresses are "in transit" — one side already wrote them, the other writes them next. */
        static uint8_t aa[0x10000], ab[0x10000];
        memcpy(aa, r->cd->adpcm.ram, sizeof(aa));
        memcpy(ab, o->cd->adpcm.ram, sizeof(ab));
        if ((r->cd->adpcm.dma & 3) || (o->cd->adpcm.dma & 3)) {
            uint16_t lo = r->cd->adpcm.write_address, hi = o->cd->adpcm.write_address;
            if (lo > hi) {
                uint16_t t = lo;
                lo = hi;
                hi = t;
            }
            for (uint32_t i = lo; i < hi; i++)
                aa[i] = ab[i] = 0;
        }
        if (diff_bytes(out, n, "ADPCM RAM", aa, ab, sizeof(aa), 0))
            return 1;
    }
    if (diff_words(out, n, "VRAM", r->vdc.vram, o->vdc.vram, sizeof(r->vdc.vram) / 2) ||
        diff_words(out, n, "sprite table", r->vdc.sat, o->vdc.sat, sizeof(r->vdc.sat) / 2) ||
        diff_words(out, n, "VDC register", r->vdc.reg, o->vdc.reg, 20) ||
        diff_words(out, n, "palette", r->vce.color, o->vce.color, 512))
        return 1;
    if (with_irq)
        return 0;
    uint32_t cnt = g_ref_log.count < g_our_log.count ? g_ref_log.count : g_our_log.count;
    if (cnt > IO_LOG_MAX)
        cnt = IO_LOG_MAX;
    for (uint32_t i = 0; i < cnt; i++) {
        if (g_ref_log.addr[i] != g_our_log.addr[i] || g_ref_log.value[i] != g_our_log.value[i]) {
            snprintf(out, n, "hardware write no. %u: BIOS $%04X=%02X, ours $%04X=%02X", i, g_ref_log.addr[i],
                     g_ref_log.value[i], g_our_log.addr[i], g_our_log.value[i]);
            return 1;
        }
    }
    if (g_ref_log.count != g_our_log.count) {
        snprintf(out, n, "number of hardware writes: BIOS %u, ours %u", g_ref_log.count, g_our_log.count);
        return 1;
    }
    return 0;
}

/* Moments (master cycles since the entry) at which the game code started running in the middle of the call
 * (hooks, interrupts): for --detail */
#define MAX_MARKS 8
static int64_t g_ref_marks[MAX_MARKS], g_our_marks[MAX_MARKS];
static int g_ref_mark_count, g_our_mark_count, g_ref_in_game;

static void mark(int64_t *marks, int *count, int64_t when)
{
    if (*count < MAX_MARKS)
        marks[(*count)++] = when;
}

/* --pieces: the sound driver (hle_psg.c) is split into pieces; each one starts at a point that in the real BIOS is
 * a driver address (in bank 2 in window 6, or in bank 0) and in ours a private point ($FECx music,
 * $FEEx effects) or the same address; the VDC interrupt too, at its wait points. Records the instant (master
 * cycles since the entry) at which each piece starts, on both sides, and shows the first piece whose duration
 * differs. The wave (P_WAVE) goes 8 bytes per piece: in the real BIOS only the loop iteration with Y a multiple
 * of 8 counts. */
typedef struct Piece {
    uint16_t real, ours;
    const char *name;
} Piece;
static const Piece k_pieces[] = {
    {0xE6E8, 0xE6E8, "step"},        {0xE707, 0xE707, "step:effects done"}, {0xE713, 0xE713, "step:music"},
    {0xE71D, 0xE71D, "step:music done"}, {0xE729, 0xE729, "step:end"},
    {0xE8CA, 0xE8CA, "vdc:controls"}, {0xE8CD, 0xE8CD, "vdc:hook"}, {0xE8D2, 0xE8D2, "vdc:line"},
    {0xE8EF, 0xE8EF, "vdc:end"},
    {0xC2EF, 0xFEC0, "m.start"},     {0xC30B, 0xFEC1, "m.channel"}, {0xC3F8, 0xFEC2, "m.sequence"},
    {0xC85A, 0xFEC3, "m.slides"},    {0xC953, 0xFEC4, "m.drums"},   {0xCA25, 0xFEC5, "m.envelope"},
    {0xCB13, 0xFEC6, "m.pitch"},     {0xC322, 0xFEC7, "m.next"},    {0xCC52, 0xFEC8, "m.output"},
    {0xCCB9, 0xFEC9, "m.wave"},
    {0xCD12, 0xFEE0, "e.start"},     {0xCD20, 0xFEE1, "e.channel"}, {0xCE07, 0xFEE2, "e.sequence"},
    {0xD257, 0xFEE3, "e.slides"},    {0xD350, 0xFEE4, "e.drums"},   {0xD422, 0xFEE5, "e.envelope"},
    {0xD510, 0xFEE6, "e.pitch"},     {0xCD37, 0xFEE7, "e.next"},    {0xD64F, 0xFEE8, "e.output"},
    {0xD6BB, 0xFEE9, "e.wave"},
};
#define PIECE_COUNT ((int)(sizeof(k_pieces) / sizeof(k_pieces[0])))
#define MAX_PIECE_MARKS 1024
typedef struct PieceLog {
    int count;
    uint16_t piece[MAX_PIECE_MARKS]; /* real BIOS address where the piece starts */
    int64_t when[MAX_PIECE_MARKS];
} PieceLog;
static int g_pieces_on;
static int g_list_times; /* --timings N: lists the first N calls of each routine with a different time */
static PieceLog g_ref_pieces, g_our_pieces;

/* Address (in the real BIOS) of the piece that starts here, or 0. Besides the table, any BIOS point (bank 0)
 * that has a routine in C, outside the entry table. */
static uint16_t piece_at(const Pce *pce, int ours)
{
    const HuC6280 *c = &pce->cpu;
    uint16_t pc = c->pc;
    for (int i = 0; i < PIECE_COUNT; i++) {
        uint16_t a = ours ? k_pieces[i].ours : k_pieces[i].real;
        if (pc != a)
            continue;
        if (a >= 0xE000 ? c->mpr[7] != 0x00 : c->mpr[6] != 0x02)
            return 0;
        if (!ours && (a == 0xCCB9 || a == 0xD6BB) && (c->y & 7))
            return 0;
        return k_pieces[i].real;
    }
    if (pc >= 0xE100 && c->mpr[7] == 0x00 && g_all.fn[pc - 0xE000])
        return pc;
    return 0;
}

static const char *piece_name(uint16_t a, char *buf, size_t n)
{
    for (int i = 0; i < PIECE_COUNT; i++)
        if (k_pieces[i].real == a)
            return k_pieces[i].name;
    snprintf(buf, n, "$%04X", a);
    return buf;
}

static void piece_mark(PieceLog *log, const Pce *pce, int ours, int64_t when)
{
    uint16_t p = piece_at(pce, ours);
    if (p && log->count < MAX_PIECE_MARKS) {
        log->piece[log->count] = p;
        log->when[log->count++] = when;
    }
}

/* First piece with a different duration (or a different sequence); 0 = all equal */
static int piece_compare(char *out, size_t n)
{
    const PieceLog *r = &g_ref_pieces, *o = &g_our_pieces;
    int count = r->count < o->count ? r->count : o->count;
    char b1[16], b2[16], b3[16];
    for (int i = 0; i < count; i++) {
        if (r->piece[i] != o->piece[i]) {
            snprintf(out, n, "piece %d: BIOS %s, ours %s (after %s)", i, piece_name(r->piece[i], b1, 16),
                     piece_name(o->piece[i], b2, 16), i ? piece_name(r->piece[i - 1], b3, 16) : "-");
            return 1;
        }
        if (r->when[i] != o->when[i]) {
            int64_t rd = i ? r->when[i] - r->when[i - 1] : r->when[i];
            int64_t od = i ? o->when[i] - o->when[i - 1] : o->when[i];
            snprintf(out, n, "piece %d: %s -> %s took BIOS %lld, ours %lld (master cycles)", i,
                     i ? piece_name(r->piece[i - 1], b1, 16) : "entry", piece_name(r->piece[i], b2, 16),
                     (long long)rd, (long long)od);
            return 1;
        }
    }
    if (r->count != o->count) {
        snprintf(out, n, "pieces: BIOS %d, ours %d", r->count, o->count);
        return 1;
    }
    return 0;
}

/* --detail ENTRY:N — shows everything about the N-th call of this routine */
static uint16_t g_detail_entry;
static uint64_t g_detail_call;
static char g_entry_info[512];

static void describe(char *out, size_t n, const Pce *p)
{
    const Vdc *v = &p->vdc;
    snprintf(out, n,
             "VDC: AR=%02X MAWR=%04X MARR=%04X CR=%04X pending read=%d pending write=%d | "
             "ADPCM: write=%04X read=%04X len=%05X dma=%02X ctrl=%02X | SCSI phase=%d data=%u/%u | "
             "master=%llu | IRQ requested=%02X serviced=%u P=%02X line=%d",
             v->ar, v->reg[0], v->reg[1], v->reg[5], v->pending_read, v->pending_write,
             p->cd ? p->cd->adpcm.write_address : 0, p->cd ? p->cd->adpcm.read_address : 0,
             p->cd ? p->cd->adpcm.length : 0, p->cd ? p->cd->adpcm.dma : 0, p->cd ? p->cd->adpcm.control : 0,
             p->cd ? (int)p->cd->scsi.phase : 0, p->cd ? p->cd->scsi.data_offset : 0,
             p->cd ? p->cd->scsi.data_size : 0, (unsigned long long)p->master_cycles, p->cpu.irq_request,
             p->cpu.irq_count, p->cpu.p, v->vpos);
}

static void print_log(const char *who, const IoLog *log)
{
    printf("  writes %s (%u):", who, log->count);
    for (uint32_t i = 0; i < log->count && i < 60; i++)
        printf(" %04X=%02X", log->addr[i], log->value[i]);
    printf("\n");
}

static Pce g_snapshot; /* the console at the routine's entry (defined below) */

static void print_detail(const Pce *ref, const Pce *ours)
{
    char s[512];
    printf("DETAIL of call %llu of $%04X\n  at entry: %s\n", (unsigned long long)g_detail_call, g_detail_entry,
           g_entry_info);
    const Pce *e = &g_snapshot;
    printf("  registers at entry: A=%02X X=%02X Y=%02X P=%02X F8-FF=%02X %02X %02X %02X %02X %02X %02X %02X\n",
           e->cpu.a, e->cpu.x, e->cpu.y, e->cpu.p, e->ram[0xF8], e->ram[0xF9], e->ram[0xFA], e->ram[0xFB],
           e->ram[0xFC], e->ram[0xFD], e->ram[0xFE], e->ram[0xFF]);
    describe(s, sizeof(s), ref);
    printf("  BIOS on return: %s\n", s);
    describe(s, sizeof(s), ours);
    printf("  ours on return: %s\n", s);
    print_log("BIOS", &g_ref_log);
    print_log("ours", &g_our_log);
    printf("  game code started at (master since entry): BIOS");
    for (int i = 0; i < g_ref_mark_count; i++)
        printf(" %lld", (long long)g_ref_marks[i]);
    printf(" | ours");
    for (int i = 0; i < g_our_mark_count; i++)
        printf(" %lld", (long long)g_our_marks[i]);
    printf("\n");
    printf("  differing RAM (address: BIOS/ours):");
    for (int i = 0, shown = 0; i < 0x2000 && shown < 48; i++) {
        if (i >= 0x100 && i < 0x200)
            continue; /* stack */
        if (ref->ram[i] != ours->ram[i]) {
            printf(" %04X:%02X/%02X", 0x2000 + i, ref->ram[i], ours->ram[i]);
            shown++;
        }
    }
    printf("\n");
    if (ref->cd && ours->cd) {
        int diffs = 0, first = -1, last = -1;
        for (int i = 0; i < 0x10000; i++)
            if (ref->cd->adpcm.ram[i] != ours->cd->adpcm.ram[i]) {
                diffs++;
                if (first < 0)
                    first = i;
                last = i;
            }
        printf("  ADPCM RAM: %d differing bytes (from $%04X to $%04X)\n", diffs, first, last);
    }
}

/* Script buttons after `frames` frames (without touching the reference's script cursor). */
static uint8_t buttons_at(uint64_t frames, uint8_t current)
{
    for (int i = 0; i < g_input.count && g_input.frame[i] <= (long)frames; i++)
        current = g_input.buttons[i];
    return current;
}

/* Runs the REAL BIOS from the entry snapshot, starting a few cycles later (including the time difference
 * between our version and the BIOS), and compares with OUR result. 1 = some delay makes the BIOS itself
 * produce exactly our result: the difference is only of timing (a race with the VDC, DMA or an
 * interrupt), not of logic. */
static Pce g_snapshot;

static int bios_timing_sensitive(const Pce *ours, int64_t our_extra_master)
{
    int delays[7] = {0, 3, 6, 12, 24, 48, 96};
    if (our_extra_master > 0)
        delays[0] = (int)(our_extra_master / 3);
    static Pce variant, ref_copy;
    const Pce *ref = ours;
    for (int d = 0; d < 7; d++) {
        if (d > 0 && delays[d] == 0)
            continue;
        if (pce_clone(&variant, &g_snapshot) != 0)
            return 0;
        pce_advance_hardware(&variant, (uint64_t)delays[d] * 3);
        int done = 0;
        IoLog *saved = variant.io_log;
        variant.io_log = NULL;
        for (uint64_t i = 0; i < REF_LIMIT && !done; i++) {
            pce_step(&variant);
            if (variant.frame_ready) {
                variant.frame_ready = 0;
                variant.audio_count = psg_end_frame(&variant.psg, variant.audio);
                variant.pad_buttons = buttons_at(variant.vce.frame_count, variant.pad_buttons);
            }
            done = returned(&variant, g_entry_s);
        }
        variant.io_log = saved;
        int differs = 1;
        if (done) {
            const Pce *a = ref, *b = &variant;
            int copied = 0;
            if (b->master_cycles < a->master_cycles) {
                pce_advance_hardware(&variant, a->master_cycles - b->master_cycles);
            } else if (a->master_cycles < b->master_cycles && pce_clone(&ref_copy, ref) == 0) {
                pce_advance_hardware(&ref_copy, b->master_cycles - a->master_cycles);
                a = &ref_copy;
                copied = 1;
            }
            char what[512];
            differs = compare(what, sizeof(what), a, b, 1);
            if (copied)
                pce_free_clone(&ref_copy);
        }
        pce_free_clone(&variant);
        if (!differs)
            return 1;
    }
    return 0;
}

/* The real BIOS finished the routine: run the C version on the copy and compare. */
static void finish_check(void)
{
    Stat *st = g_pending_stat;
    g_pce.io_log = NULL;
    static Hle hle;
    hle_init(&hle, st->entry);
    g_clone.hle = &hle;
    g_clone.io_log = &g_our_log;
    g_our_log.count = 0;
    uint64_t start = g_clone.master_cycles;
    int done = 0;
    int in_game = 0;
    g_our_mark_count = 0;
    g_our_pieces.count = 0;
    for (uint64_t i = 0; i < REF_LIMIT * 2 && !done; i++) {
        int game = !bios_code(&g_clone);
        if (game && !in_game)
            mark(g_our_marks, &g_our_mark_count, (int64_t)(g_clone.master_cycles - start));
        in_game = game;
        if (g_pieces_on)
            piece_mark(&g_our_pieces, &g_clone, 1, (int64_t)(g_clone.master_cycles - start));
        pce_step(&g_clone);
        if (g_clone.frame_ready) {
            g_clone.frame_ready = 0;
            pce_end_frame_audio(&g_clone);
            /* The same buttons the reference gets at the end of this frame */
            g_clone.pad_buttons = buttons_at(g_clone.vce.frame_count, g_clone.pad_buttons);
        }
        done = returned(&g_clone, g_entry_s);
    }
    st->checked++;
    int64_t ref_elapsed = (int64_t)(g_pce.master_cycles - g_entry_master);
    int64_t our_elapsed = (int64_t)(g_clone.master_cycles - start);
    st->ref_cycles += (double)ref_elapsed;
    st->our_cycles += (double)our_elapsed;
    if (g_pce.cpu.irq_count != g_entry_irqs || g_clone.cpu.irq_count != g_entry_irqs) {
        if (g_pce.cpu.irq_count != g_entry_irqs)
            st->with_irq++;
    } else if (done) {
        int64_t d = our_elapsed - ref_elapsed;
        if (!st->time_checked || d < st->dmin)
            st->dmin = d;
        if (!st->time_checked || d > st->dmax)
            st->dmax = d;
        st->time_checked++;
        if (d != 0 && g_list_times && st->time_checked - st->time_same <= (uint64_t)g_list_times) {
            const Pce *e = &g_snapshot;
            printf("time %s call %llu frame %llu line %d: A=%02X X=%02X Y=%02X F8-FF=%02X %02X %02X %02X %02X "
                   "%02X %02X %02X  BIOS %lld, ours %lld\n",
                   st->name, (unsigned long long)st->calls, (unsigned long long)e->vce.frame_count, e->vdc.vpos,
                   e->cpu.a, e->cpu.x, e->cpu.y, e->ram[0xF8], e->ram[0xF9], e->ram[0xFA], e->ram[0xFB],
                   e->ram[0xFC], e->ram[0xFD], e->ram[0xFE], e->ram[0xFF], (long long)ref_elapsed,
                   (long long)our_elapsed);
        }
        if (d == 0) {
            st->time_same++;
        } else if (!st->first_time_call) {
            st->first_time_call = st->calls;
            st->first_ref = ref_elapsed;
            st->first_our = our_elapsed;
        }
        if (g_pieces_on) {
            char what_piece[256];
            st->piece_checked++;
            if (!piece_compare(what_piece, sizeof(what_piece)))
                st->piece_same++;
            else if (st->piece_diff_count < 3)
                snprintf(st->piece_diff[st->piece_diff_count++], sizeof(st->piece_diff[0]), "call %llu: %s",
                         (unsigned long long)st->calls, what_piece);
        }
    }
    /* Same instant on both: the one that finished first lets the hardware run (ADPCM DMA, VDC...)
     * until it catches up with the other, with the CPU stopped at the return point. */
    /* (The reference session cannot be altered: if it is the one behind, a copy is advanced.) */
    static Pce ref_copy;
    const Pce *ref = &g_pce;
    int copied = 0;
    if (done && g_clone.master_cycles < g_pce.master_cycles) {
        pce_advance_hardware(&g_clone, g_pce.master_cycles - g_clone.master_cycles);
    } else if (done && g_pce.master_cycles < g_clone.master_cycles && pce_clone(&ref_copy, &g_pce) == 0) {
        pce_advance_hardware(&ref_copy, g_clone.master_cycles - g_pce.master_cycles);
        ref = &ref_copy;
        copied = 1;
    }
    g_ignore_count = 0;
    if (st->entry == 0xE060) {
        const Pce *e = &g_snapshot;
        ignore_logical(e, 0x20EC, 3);
        ignore_logical(e, 0x20F8, 2);
        ignore_logical(e, 0x22BD, 1);
        ignore_logical(e, (uint16_t)(e->ram[0xFA] | (e->ram[0xFB] << 8)), 32);
    }
    char what[512];
    if (!done) {
        snprintf(what, sizeof(what), "the C version did not return to the game");
        st->different++;
    } else if (compare(what, sizeof(what), ref, &g_clone,
                       g_pce.cpu.irq_count != g_entry_irqs || g_clone.cpu.irq_count != g_entry_irqs)) {
        if (bios_timing_sensitive(&g_clone, our_elapsed - ref_elapsed)) {
            st->sensitive++; /* the BIOS itself, a few cycles late, also gives another result */
            what[0] = 0;
        } else {
            st->different++;
        }
    } else {
        st->equal++;
        what[0] = 0;
    }
    if (g_detail_entry == st->entry && g_detail_call == st->calls) {
        print_detail(ref, &g_clone);
        printf("  time spent (master): BIOS %lld, ours %lld\n", (long long)ref_elapsed, (long long)our_elapsed);
    }
    if (what[0] && st->diff_count < 3) {
        snprintf(st->first_diff[st->diff_count], sizeof(st->first_diff[0]), "call %llu (frame %llu)%s: %s",
                 (unsigned long long)st->calls, (unsigned long long)g_pce.vce.frame_count,
                 g_pce.cpu.irq_count != g_entry_irqs ? " [with an interrupt in the middle]" : "", what);
        st->diff_count++;
    }
    if (copied)
        pce_free_clone(&ref_copy);
    g_clone.hle = NULL;
    pce_free_clone(&g_clone);
    pce_free_clone(&g_snapshot);
    g_pending = 0;
}

static void on_instruction(Pce *pce, int after)
{
    if (after)
        return;
    int game = !bios_code(pce);
    if (g_pending) {
        g_ref_steps++;
        if (game && !g_ref_in_game)
            mark(g_ref_marks, &g_ref_mark_count, (int64_t)(pce->master_cycles - g_entry_master));
        g_ref_in_game = game;
        if (g_pieces_on)
            piece_mark(&g_ref_pieces, pce, 0, (int64_t)(pce->master_cycles - g_entry_master));
        if (returned(pce, g_entry_s)) {
            finish_check();
        } else if (g_ref_steps > REF_LIMIT) {
            g_pending_stat->unfinished++;
            g_pce.io_log = NULL;
            pce_free_clone(&g_clone);
            pce_free_clone(&g_snapshot);
            g_pending = 0;
        }
    }
    /* Start of a routine: a game call to a table entry, or an interrupt that entered a BIOS vector
     * while the game was running. */
    static uint32_t irq_seen;
    int irq_entry = pce->cpu.irq_count != irq_seen;
    irq_seen = pce->cpu.irq_count;
    int table_call = pce->cpu.pc >= 0xE000 && pce->cpu.pc < 0xE100;
    if (!g_pending && g_prev_game && !game && pce->cpu.mpr[7] == 0 && pce->cpu.pc >= 0xE000 &&
        g_all.fn[pce->cpu.pc - 0xE000] && (table_call || irq_entry)) {
        Stat *st = stat_for(pce->cpu.pc);
        if (st && st->selected) {
            st->calls++;
            if (st->checked + st->unfinished < (uint64_t)g_max && pce_clone(&g_clone, pce) == 0 &&
                pce_clone(&g_snapshot, pce) == 0) {
                g_pending = 1;
                g_pending_stat = st;
                g_entry_s = pce->cpu.s;
                g_entry_irqs = pce->cpu.irq_count;
                g_entry_master = pce->master_cycles;
                g_ref_steps = 0;
                g_ref_mark_count = 0;
                g_ref_pieces.count = 0;
                g_ref_in_game = 0;
                g_ref_log.count = 0;
                pce->io_log = &g_ref_log;
                if (g_detail_entry == st->entry && g_detail_call == st->calls)
                    describe(g_entry_info, sizeof(g_entry_info), pce);
            }
        }
    }
    g_prev_game = game;
}

int main(int argc, char **argv)
{
    const char *input = NULL, *bram = NULL, *bios = NULL, *only = "all";
    const char *pos[2];
    int npos = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: %s [--input <session>] [--bram <save>] [--routines E069,E07B|all] [--max N] [--timings N]\n"
                   "         [--pieces] [--detail ENTRY:N] --bios <system card> <image.cue> <frames>\n"
                   "  checks the BIOS routines rewritten in C (runtime/bios) against the real BIOS, call by call,\n"
                   "  replaying the session: registers, RAMs, VRAM, VDC, VCE and the hardware writes\n"
                   "  --max N      at most N checks per routine (default 2000); the rest are only counted\n"
                   "  --timings N  lists the first N calls of each routine with a different time\n"
                   "  output: one summary per routine; exit code 0 = all equal, 1 = some difference\n",
                   argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc)
            input = argv[++i];
        else if (strcmp(argv[i], "--bram") == 0 && i + 1 < argc)
            bram = argv[++i];
        else if (strcmp(argv[i], "--bios") == 0 && i + 1 < argc)
            bios = argv[++i];
        else if (strcmp(argv[i], "--routines") == 0 && i + 1 < argc)
            only = argv[++i];
        else if (strcmp(argv[i], "--max") == 0 && i + 1 < argc)
            g_max = atoi(argv[++i]);
        else if (strcmp(argv[i], "--pieces") == 0)
            g_pieces_on = 1;
        else if (strcmp(argv[i], "--timings") == 0 && i + 1 < argc)
            g_list_times = atoi(argv[++i]);
        else if (strcmp(argv[i], "--detail") == 0 && i + 1 < argc) {
            unsigned e = 0;
            unsigned long long n = 0;
            if (sscanf(argv[++i], "%X:%llu", &e, &n) == 2) {
                g_detail_entry = (uint16_t)e;
                g_detail_call = n;
            }
        }
        else if (npos < 2)
            pos[npos++] = argv[i];
    }
    if (!bios || npos < 2) {
        printf("usage: %s [--input <session>] [--bram <save>] [--routines E069,...|all] [--max N] "
               "--bios <system card> <image.cue> <frames>\n", argv[0]);
        return 2;
    }
    if (input && input_script_load(&g_input, input) != 0) {
        printf("could not read %s\n", input);
        return 2;
    }
    if (pce_load_cd(&g_pce, bios, pos[0]) != 0)
        return 2;
    pce_reset(&g_pce);
    if (bram && pce_load_bram(&g_pce, bram) != 0) {
        printf("could not read the save %s\n", bram);
        return 2;
    }
    hle_init(&g_all, 0);
    for (int g = 0; hle_groups[g]; g++) {
        for (const HleRoutine *r = hle_groups[g]; r->entry && g_stat_count < MAX_ROUTINES; r++) {
            Stat *st = &g_stats[g_stat_count++];
            memset(st, 0, sizeof(*st));
            st->entry = r->entry;
            st->name = r->name;
            char hex[8];
            snprintf(hex, sizeof(hex), "%04X", r->entry);
            st->selected = strcmp(only, "all") == 0 || strstr(only, hex) != NULL;
        }
    }
    g_pce.on_instruction = on_instruction;

    long frames = strtol(pos[1], NULL, 10);
    g_pce.pad_buttons = input_script_update(&g_input, 0, 0);
    for (long f = 0; f < frames; f++) {
        if (!pce_run_frame(&g_pce, 50000000)) {
            printf("frame %ld did not finish\n", f + 1);
            break;
        }
        g_pce.pad_buttons = input_script_update(&g_input, f + 1, g_pce.pad_buttons);
    }

    int bad = 0;
    printf("%-12s %-6s %10s %10s %10s %9s %8s %8s  cycles BIOS/ours\n", "routine", "entry", "calls",
           "checked", "different", "sensitive", "w/ IRQ", "no end");
    printf("(sensitive = result differs from the BIOS one, but the BIOS itself, starting a few cycles later, produces "
           "exactly our result: a timing race with the VDC, the DMA or an interrupt)\n");
    for (int i = 0; i < g_stat_count; i++) {
        Stat *st = &g_stats[i];
        if (!st->selected || !st->calls)
            continue;
        printf("%-12s $%04X  %10llu %10llu %10llu %9llu %8llu %8llu  %.0f/%.0f\n", st->name, st->entry,
               (unsigned long long)st->calls, (unsigned long long)st->checked, (unsigned long long)st->different,
               (unsigned long long)st->sensitive, (unsigned long long)st->with_irq, (unsigned long long)st->unfinished,
               st->checked ? st->ref_cycles / st->checked : 0.0, st->checked ? st->our_cycles / st->checked : 0.0);
        if (st->time_checked && st->time_same != st->time_checked)
            printf("    time: %llu of %llu without interrupt equal, difference %lld to %lld; 1st different: call %llu "
                   "(BIOS %lld, ours %lld)\n",
                   (unsigned long long)st->time_same, (unsigned long long)st->time_checked, (long long)st->dmin,
                   (long long)st->dmax, (unsigned long long)st->first_time_call, (long long)st->first_ref,
                   (long long)st->first_our);
        if (st->piece_checked && st->piece_same != st->piece_checked) {
            printf("    pieces: %llu of %llu without interrupt with all pieces at the same time\n",
                   (unsigned long long)st->piece_same, (unsigned long long)st->piece_checked);
            for (int d = 0; d < st->piece_diff_count; d++)
                printf("      %s\n", st->piece_diff[d]);
        }
        for (int d = 0; d < st->diff_count; d++)
            printf("    %s\n", st->first_diff[d]);
        if (st->different)
            bad = 1;
    }
    printf(bad ? "RESULT: THERE ARE DIFFERENCES\n" : "RESULT: all equal\n");
    pce_free(&g_pce);
    return bad;
}
