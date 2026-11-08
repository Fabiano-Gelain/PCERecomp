/*
 * pce_trace: trace of the emulated console (interpreter or recompiled code), in the same format as gg_trace:
 *   E000 A:00 X:00 Y:00 S:00 P:04 V:0 M:00,00,00,00,00,00,00,00
 * one line per instruction, with the state BEFORE executing it; or, with --frame, runs frames and saves the last one
 * as an image, with the checks and investigations of the options below. Options: see TRACE_HELP (pce_trace --help).
 *
 * A game project can add its own options and checks (pce_trace.h: TraceExt).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bios/hle.h"
#include "cd/cdrom.h"
#include "input_script.h"
#include "pce_trace.h"
#include "profiler.h"
#include "util/u8file.h"
#include "pce/bios_profile.h"
#include "pce/coverage.h"
#include "pce/lockstep.h"
#include "pce/mix.h"
#include "pce/pce.h"
#ifdef PCE_RECOMP
#include "cpu/huc6280_recomp.h" /* build with the game's recompiled code (the game project builds it this way) */
#endif

static const char TRACE_HELP[] =
    "usage: %s [options] <rom> <instructions> <output>\n"
    "       %s --frame [options] <rom> <frames> <output.ppm>\n"
    "\n"
    "Without --frame: writes <instructions> trace lines (gg_trace format) to <output>.\n"
    "With --frame: runs <frames> frames and saves the last one as PPM (RGB).\n"
    "<rom>: HuCard (.pce) or, with --bios or on its own, a CD image (.cue; without --bios, with the BIOS in C).\n"
    "\n"
    "General:\n"
    "  --input <script>       buttons of a recorded session (tools/trace/input_script.h)\n"
    "  --bios <system card>   console with CD-ROM2; <rom> becomes the CD image (.cue)\n"
    "  --hle                  uses the BIOS routines rewritten in C (runtime/bios/hle.h) instead of the real BIOS ones\n"
    "  --hle-only E009,E012   only these routines in C\n"
    "  --bram <file>          starts with this CD save memory (2 KB), like gg_trace --bram\n"
    "  --bram-out <file>      writes the CD save memory at the end\n"
    "  --gg-fader             CD volume fade like Geargrafx (to compare the sound with it; cd/cdrom.h)\n"
    "  --interpret            ignores the recompiled code (only in the build with recompiled code)\n"
    "  --coverage <file>      writes the coverage (runtime/pce/coverage.h)\n"
    "  --profile <file>       measures where the PC's time goes (profiler.h), writes the function list and shows the time\n"
    "  --interp-log <file>    ranges (bank:start-end) that went through the interpreter the most\n"
    "  --help, -h             this help\n"
    "\n"
    "Without --frame:\n"
    "  --from-frame Q         only starts writing after Q frames (<instructions> counts only the written ones)\n"
    "  --cycles               appends \" C:<master-clock>\" to each line (compare_hle_trace.py)\n"
    "\n"
    "With --frame:\n"
    "  --lockstep             (build with recompiled code) runs the interpreter alongside and compares everything at\n"
    "                         each instruction and frame (runtime/pce/lockstep.h); stops at the first difference\n"
    "  --audio <file>         sound of all frames: signed 16-bit, interleaved stereo, 44100 Hz\n"
    "  --hashes <file>        one line per frame with the CRC of the image and the sound (same format as gg_trace)\n"
    "  --every N DIR          also saves one frame every N in DIR/qNNNNNN.ppm (--every-from FROM: starting at FROM)\n"
    "  --volumes G,CD,CHIP,EF,VZ[,F]  Mednafen mix with these volumes in %% (master, CD music, chip music,\n"
    "                         effects, voices; F = 1: chip filter) instead of the Geargrafx one; --audio comes out\n"
    "                         with it and the end shows how much of the chip sound was effects\n"
    "  --watch-vram <hex>     reports each write to this VRAM word, with the last 64 instructions (bank:address)\n"
    "  --watch-ram <hex>      same, for a RAM byte ($2000-$3FFF)\n"
    "  --watch-vce FROM TO    in frames FROM..TO, who writes to the VCE colors and who asks the BIOS for colors ($221F),\n"
    "                         with the 64 instructions before the 1st write to each address\n"
    "  --bat-writers <file>   per frame, who wrote to the background map and to the sprite table in VRAM (3 routines\n"
    "                         of the call stack, instruction, no. of writes, rectangle of cells); PCE_BW_DETAIL=FROM-TO\n"
    "                         lists each map write in those frames\n"
    "  --bat-routine BB:EEEE  (with --bat-writers) counts the frames in which this routine wrote to the background map\n"
    "  --state-log <file>     per frame: the RAM (8 KB), the sprite table (512 B) and, for each image line,\n"
    "                         BXR and the map line (16 bits each; FFFF = line outside the VDC)\n"
    "  --dump-state <file>    at the end: VRAM (64 KB), SAT (512 B), VCE colors (1 KB), VDC registers (40 B),\n"
    "                         RAM (8 KB) and, with CD, the CD RAM (64 KB); words in 16-bit little-endian\n"
    "  --bios-profile <file>  profile of the BIOS routines used (runtime/pce/bios_profile.h)\n"
    "  --log-calls E060,E0D8  lists (on stderr) each call to these BIOS entries, with A, X, Y and\n"
    "                         $20F8-$20FF; BANK:ADDRESS (e.g. 81:61F2) = game routine in that bank\n"
    "  --bios-reads <file>    the BIOS bytes (banks $00-$1F) that the game code read as data\n";

static InputScript g_input;
static const char *g_audio_path;
static const char *g_coverage_path;
static Coverage g_coverage;
static Pce g_pce;
static Pce g_ref;         /* lockstep reference (interpreter) */
static Lockstep g_lockstep;
static int g_lockstep_on;
static const char *g_bios_path; /* with --bios, <rom> is the CD image (.cue) */
static const TraceExt *g_ext;   /* the game project's extension (or NULL) */
static TraceRun g_run;
static VdcLineLog g_line_log;   /* TraceRun.line_log */
static long g_sfx_samples, g_chip_samples, g_first_sfx_frame = -1; /* --volumes: music vs. effects summary */

static int load_media(Pce *pce, const char *path)
{
    if (g_bios_path)
        return pce_load_cd(pce, g_bios_path, path);
    size_t n = strlen(path);
    if (n > 4 && (strcmp(path + n - 4, ".cue") == 0 || strcmp(path + n - 4, ".CUE") == 0))
        return pce_load_cd(pce, NULL, path); /* without --bios: synthetic BIOS, routines in C */
    return pce_load_rom_file(pce, path);
}

static const char *g_hashes_path;
static long g_every = 0; /* --every N DIR: saves one frame every N */
static const char *g_every_dir;
static long g_every_from = 0; /* --every-from FROM: only from frame FROM on */
static long g_watch_vram = -1; /* --watch-vram: watched VRAM word */
static long g_watch_ram = -1;  /* --watch-ram: watched RAM byte (0-1FFF = $2000-$3FFF) */
static long g_watch_vce_from = -1, g_watch_vce_to = -1; /* --watch-vce FROM TO: who writes to the VCE colors */
static FILE *g_state_log;      /* --state-log: RAM, sprites and scroll of each line, frame by frame */

/* --bat-writers FILE: who writes to the background map (BAT) and to the sprite table in VRAM, per frame. The call
 * stack is followed through the stack pointer: JSR/BSR push the called routine, an interrupt pushes "IRQ", and
 * when the stack rises above the entry (RTS, RTI, PLA of the return address) it is popped. Each write carries the 3
 * innermost routines and the instruction address; at the end of the frame, one line per (routine, instruction) with
 * the number of writes and the rectangle of map cells (column, row) */
static FILE *g_batw;
typedef struct { uint8_t sp; uint8_t irq; uint32_t target; } BwCall; /* target = bank << 16 | address */
#define BW_DEPTH 64
#define BW_KEYS 512
static BwCall g_bw_stack[BW_DEPTH];
static int g_bw_depth;
static uint32_t g_bw_pc; /* bank << 16 | address of the instruction being run */
typedef struct {
    uint32_t r0, r1, r2, pc; /* innermost routine, the two outer ones, instruction (0xFFFFFFFF = VRAM-VRAM copy) */
    uint8_t sat;             /* 0 = map, 1 = sprite table in VRAM */
    int count, x0, y0, x1, y1;
} BwKey;
static BwKey g_bw_keys[BW_KEYS];
static int g_bw_nkeys, g_bw_lost;
static Pce *g_bw_pce;
static long g_bw_frame = 1, g_bw_detail_from = -1, g_bw_detail_to = -2; /* PCE_BW_DETAIL=FROM-TO: each write */
static uint32_t g_bw_routine; /* --bat-routine: bank << 16 | address (0 = none) */
static long g_bw_routine_frames, g_bw_routine_first = -1; /* frames in which it wrote to the map */

static uint32_t bw_routine(int depth_from_top)
{
    int i = g_bw_depth - 1 - depth_from_top;
    if (i < 0)
        return 0;
    return g_bw_stack[i].irq ? 0xFF000000u | g_bw_stack[i].target : g_bw_stack[i].target;
}

static void bw_hook(void *ctx, uint16_t addr, uint16_t value, int dma)
{
    (void)ctx;
    (void)value;
    const Vdc *v = &g_bw_pce->vdc;
    int mwr = v->reg[9];
    int bw = (mwr >> 4 & 3) == 0 ? 32 : (mwr >> 4 & 3) == 1 ? 64 : 128;
    int bh = (mwr & 0x40) ? 64 : 32;
    int sat = addr >= v->reg[0x13] && addr < v->reg[0x13] + 256;
    if (!sat && addr >= bw * bh)
        return;
    int x = sat ? (addr - v->reg[0x13]) / 4 : addr % bw, y = sat ? 0 : addr / bw;
    uint32_t r0 = dma ? 0 : bw_routine(0), r1 = dma ? 0 : bw_routine(1), r2 = dma ? 0 : bw_routine(2);
    uint32_t pc = dma ? 0xFFFFFFFFu : g_bw_pc;
    if (!sat && g_bw_frame >= g_bw_detail_from && g_bw_frame <= g_bw_detail_to) /* PCE_BW_DETAIL=FROM-TO */
        fprintf(g_batw, "D %ld %d %d %06X %06X %04X\n", g_bw_frame, x, y, (unsigned)(r0 & 0xFFFFFF),
                (unsigned)(r1 & 0xFFFFFF), value);
    BwKey *k = NULL;
    for (int i = 0; i < g_bw_nkeys; i++)
        if (g_bw_keys[i].pc == pc && g_bw_keys[i].r0 == r0 && g_bw_keys[i].r1 == r1 && g_bw_keys[i].r2 == r2 &&
            g_bw_keys[i].sat == sat) {
            k = &g_bw_keys[i];
            break;
        }
    if (!k) {
        if (g_bw_nkeys == BW_KEYS) {
            g_bw_lost++;
            return;
        }
        k = &g_bw_keys[g_bw_nkeys++];
        *k = (BwKey){ r0, r1, r2, pc, (uint8_t)sat, 0, x, y, x, y };
    }
    k->count++;
    if (x < k->x0) k->x0 = x;
    if (x > k->x1) k->x1 = x;
    if (y < k->y0) k->y0 = y;
    if (y > k->y1) k->y1 = y;
}

static uint32_t bw_here(const Pce *pce)
{
    return (uint32_t)pce->cpu.mpr[pce->cpu.pc >> 13] << 16 | pce->cpu.pc;
}

/* One instruction, following the call stack */
static void bw_step(Pce *pce)
{
    uint16_t pc0 = pce->cpu.pc;
    uint8_t op = pce_bus_read(pce, huc6280_phys(&pce->cpu, pc0));
    uint8_t s0 = pce->cpu.s;
    g_bw_pc = bw_here(pce);
    pce_step(pce);
    uint8_t s1 = pce->cpu.s;
    int pushed = (uint8_t)(s0 - s1);
    int call = op == 0x20 || op == 0x44;
    /* the stack rose: pop the calls whose return address was pulled */
    while (g_bw_depth > 0 && g_bw_stack[g_bw_depth - 1].sp < s1)
        g_bw_depth--;
    if (pushed >= 1 && pushed <= 8) {
        int irq = 0;
        if (call && pushed == 2) {
            /* call */
        } else if (call && pushed == 5) {
            irq = 1; /* call and interrupt in the same instruction: only the interrupt is followed */
        } else if (!call && pushed == 3 && op != 0x08 && op != 0x48 && op != 0x5A && op != 0xDA) {
            irq = 1;
        } else if (!call && pushed == 4) {
            irq = 1; /* PHA/PHX/PHY/PHP and interrupt */
        } else
            return;
        if (g_bw_depth == BW_DEPTH) {
            memmove(g_bw_stack, g_bw_stack + 1, sizeof(g_bw_stack[0]) * (BW_DEPTH - 1));
            g_bw_depth--;
        }
        g_bw_stack[g_bw_depth++] = (BwCall){ s1, (uint8_t)irq, bw_here(pce) };
    }
}

static void bw_frame_end(long frame)
{
    int border = 0;
    for (int i = 0; i < g_bw_nkeys; i++) {
        const BwKey *k = &g_bw_keys[i];
        border |= g_bw_routine && !k->sat && k->r0 == g_bw_routine;
        fprintf(g_batw, "%ld %s", frame, k->sat ? "sat" : "bat");
        const uint32_t r[3] = { k->r0, k->r1, k->r2 };
        for (int j = 0; j < 3; j++) {
            if (r[j] == 0)
                fprintf(g_batw, " -");
            else
                fprintf(g_batw, " %s%02X:%04X", (r[j] >> 24) ? "irq@" : "", (unsigned)(r[j] >> 16 & 0xFF),
                        (unsigned)(r[j] & 0xFFFF));
        }
        if (k->pc == 0xFFFFFFFFu)
            fprintf(g_batw, " dma");
        else
            fprintf(g_batw, " %02X:%04X", (unsigned)(k->pc >> 16), (unsigned)(k->pc & 0xFFFF));
        fprintf(g_batw, " n=%d x=%d-%d y=%d-%d\n", k->count, k->x0, k->x1, k->y0, k->y1);
    }
    if (g_bw_lost)
        fprintf(g_batw, "%ld lost=%d\n", frame, g_bw_lost);
    if (border) {
        g_bw_routine_frames++;
        if (g_bw_routine_first < 0)
            g_bw_routine_first = frame;
    }
    g_bw_nkeys = 0;
    g_bw_lost = 0;
    g_bw_frame = frame + 1;
}


/* --log-calls: BIOS entries (addresses in window 7 with bank 0) whose calls are listed */
static uint16_t g_log_calls[16];
static int g_log_call_bank[16]; /* -1 = BIOS (window 7, bank 0); otherwise the bank (BANK:ADDRESS, game routine) */
static int g_log_call_count;

static void log_calls_hook(Pce *pce, int after)
{
    const HuC6280 *c = &pce->cpu;
    if (after)
        return;
    for (int i = 0; i < g_log_call_count; i++) {
        if (g_log_call_bank[i] >= 0) {
            if (c->pc != g_log_calls[i] || c->mpr[c->pc >> 13] != g_log_call_bank[i])
                continue;
        } else if (c->mpr[7] != 0x00 ||
                   (c->pc != g_log_calls[i] && !(g_log_calls[i] == 0xE000 && c->pc >= 0xE000 && c->pc < 0xE100 && i == 0)))
            continue;
        uint8_t zp[8];
        pce_sync_video(pce); /* current line and position */
        for (int k = 0; k < 8; k++)
            zp[k] = pce->ram[0xF8 + k];
        /* who called: the JSRs on top of the stack (return - 2 = the JSR), with the bank that was in its window */
        int ret = (pce->ram[0x100 + ((c->s + 1) & 0xFF)] | pce->ram[0x100 + ((c->s + 2) & 0xFF)] << 8) + 1;
        int ret2 = (pce->ram[0x100 + ((c->s + 3) & 0xFF)] | pce->ram[0x100 + ((c->s + 4) & 0xFF)] << 8) + 1;
        fprintf(stderr, "call %04X frame %llu line %d.%d: A=%02X X=%02X Y=%02X F8-FF=%02X %02X %02X %02X %02X %02X %02X %02X return %02X:%04X %02X:%04X\n",
                c->pc, (unsigned long long)pce->vce.frame_count, pce->vdc.vpos, pce->vdc.hpos, c->a, c->x, c->y,
                zp[0], zp[1], zp[2], zp[3], zp[4], zp[5], zp[6], zp[7], c->mpr[(ret & 0xFFFF) >> 13], ret & 0xFFFF,
                c->mpr[(ret2 & 0xFFFF) >> 13], ret2 & 0xFFFF);
    }
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static int save_ppm(const Pce *pce, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return 1;
    const Vce *v = &pce->vce;
    fprintf(f, "P6\n%d %d\n255\n", v->frame_width, v->frame_height);
    for (int i = 0; i < v->frame_width * v->frame_height; i++) {
        uint8_t rgb[3] = { (uint8_t)(v->frame[i] >> 16), (uint8_t)(v->frame[i] >> 8), (uint8_t)v->frame[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 0;
}

int trace_save_ppm(const Pce *pce, const char *path)
{
    return save_ppm(pce, path);
}

int trace_save_ppm_argb(const uint32_t *px, int w, int h, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return 1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint8_t rgb[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 0;
}

/* Runs <frames> frames and saves the last one as PPM (RGB). */
static int dump_frame(Pce *pce, long frames, const char *path)
{
    if (g_ext && g_ext->input)
        g_ext->input(&g_run, 0);
    FILE *audio = g_audio_path ? fopen(g_audio_path, "wb") : NULL;
    FILE *hashes = g_hashes_path ? fopen(g_hashes_path, "w") : NULL;
    for (long i = 0; i < frames; i++) {
        if (g_batw) {
            pce->frame_ready = 0;
            for (long k = 0; k < 10000000 && !pce->frame_ready; k++)
                bw_step(pce);
            if (!pce->frame_ready) {
                printf("frame %ld did not finish\n", i + 1);
                return 1;
            }
            bw_frame_end(i + 1);
            pce->frame_ready = 0;
            pce_end_frame_audio(pce);
        } else if (g_watch_vce_from >= 0 && i + 1 >= g_watch_vce_from && i + 1 <= g_watch_vce_to) {
            /* --watch-vce: instruction by instruction, the writes to the VCE colors (the color address changed or
             * the color at the address changed), grouped by instruction address; the first time for each, the 64
             * previous instructions */
            static uint32_t seen[256];
            static int seen_n;
            uint32_t who[32];
            int cnt[32], nw = 0, lo = 511, hi = 0;
            uint16_t pcs[64];
            uint8_t banks[64];
            int n = 0;
            pce->frame_ready = 0;
            for (long k = 0; k < 10000000 && !pce->frame_ready; k++) {
                static uint16_t pal[512];
                static int pal_ok;
                if (!pal_ok) {
                    memcpy(pal, pce->vce.color, sizeof(pal));
                    pal_ok = 1;
                }
                uint32_t vw = pce->vce_writes;
                uint8_t req = pce->ram[0x21F];
                pcs[n & 63] = pce->cpu.pc;
                banks[n & 63] = pce->cpu.mpr[pce->cpu.pc >> 13];
                uint32_t id = (uint32_t)banks[n & 63] << 16 | pce->cpu.pc;
                n++;
                pce_step(pce);
                if (pce->ram[0x21F] && pce->ram[0x21F] != req) /* ex_colorcmd request (palettes from RAM) */
                    printf("frame %ld: color request %02X by %02X:%04X (background %04X x%d, sprites %04X x%d)\n", i + 1,
                           pce->ram[0x21F], id >> 16, id & 0xFFFF, pce->ram[0x220] | pce->ram[0x221] << 8,
                           pce->ram[0x222], pce->ram[0x223] | pce->ram[0x224] << 8, pce->ram[0x225]);
                if (pce->vce_writes == vw)
                    continue;
                /* a TIA of 512 colors wraps back to the same address: compare the whole palette */
                int ca = -1, last = -1;
                uint16_t cv = 0;
                for (int q = 0; q < 512; q++)
                    if (pal[q] != pce->vce.color[q]) {
                        if (ca < 0) {
                            ca = q;
                            cv = pal[q];
                        }
                        last = q;
                        pal[q] = pce->vce.color[q];
                    }
                if (ca < 0)
                    continue;
                lo = ca < lo ? ca : lo;
                hi = last > hi ? last : hi;
                int j = 0;
                while (j < nw && who[j] != id)
                    j++;
                if (j == nw && nw < 32) {
                    who[nw] = id;
                    cnt[nw++] = 0;
                }
                if (j < 32)
                    cnt[j]++;
                int s = 0;
                while (s < seen_n && seen[s] != id)
                    s++;
                if (s == seen_n && seen_n < 256) {
                    seen[seen_n++] = id;
                    printf("frame %ld: 1st color write by %02X:%04X (colors %03X-%03X; %03X: %03X -> %03X); before:",
                           i + 1, id >> 16, id & 0xFFFF, ca, last, ca, cv, pce->vce.color[ca]);
                    for (int q = n > 64 ? n - 64 : 0; q < n; q++)
                        printf(" %02X:%04X", banks[q & 63], pcs[q & 63]);
                    printf("\n  MPR");
                    for (int q = 0; q < 8; q++)
                        printf(" %02X", pce->cpu.mpr[q]);
                    printf(" A=%02X X=%02X Y=%02X\n", pce->cpu.a, pce->cpu.x, pce->cpu.y);
                }
            }
            if (!pce->frame_ready) {
                printf("frame %ld did not finish\n", i + 1);
                return 1;
            }
            if (nw) {
                printf("frame %ld: colors %03X-%03X written by", i + 1, lo, hi);
                for (int j = 0; j < nw; j++)
                    printf(" %02X:%04X(%d)", who[j] >> 16, who[j] & 0xFFFF, cnt[j]);
                printf("\n");
            }
            pce->frame_ready = 0;
            pce_end_frame_audio(pce);
        } else if (g_watch_vram >= 0 || g_watch_ram >= 0) {
            /* --watch-vram/--watch-ram: instruction by instruction, reporting when the word/byte changes */
            uint16_t pcs[64];
            uint8_t banks[64];
            int n = 0;
            pce->frame_ready = 0;
            for (long k = 0; k < 10000000 && !pce->frame_ready; k++) {
                uint16_t before = g_watch_vram >= 0 ? pce->vdc.vram[g_watch_vram] : pce->ram[g_watch_ram];
                pcs[n & 63] = pce->cpu.pc;
                banks[n & 63] = pce->cpu.mpr[pce->cpu.pc >> 13];
                n++;
                pce_step(pce);
                uint16_t after = g_watch_vram >= 0 ? pce->vdc.vram[g_watch_vram] : pce->ram[g_watch_ram];
                if (after != before) {
                    if (g_watch_vram >= 0)
                        printf("frame %ld: VRAM %04X %04X -> %04X; last instructions:", i + 1, g_watch_vram, before,
                               after);
                    else
                        printf("frame %ld: RAM %04X %02X -> %02X; last instructions:", i + 1,
                               (unsigned)g_watch_ram + 0x2000, before, after);
                    for (int j = n > 64 ? n - 64 : 0; j < n; j++)
                        printf(" %02X:%04X", banks[j & 63], pcs[j & 63]);
                    printf("\n  MPR");
                    for (int j = 0; j < 8; j++)
                        printf(" %02X", pce->cpu.mpr[j]);
                    printf(" A=%02X X=%02X Y=%02X\n", pce->cpu.a, pce->cpu.x, pce->cpu.y);
                }
            }
            if (!pce->frame_ready) {
                printf("frame %ld did not finish\n", i + 1);
                return 1;
            }
            pce->frame_ready = 0;
            pce_end_frame_audio(pce);
        } else if (!pce_run_frame(pce, 10000000)) {
            printf("frame %ld did not finish\n", i + 1);
            return 1;
        }
        if (g_ext && g_ext->frame)
            g_ext->frame(&g_run, i + 1);
        if (g_state_log) {
            /* --state-log: per frame, RAM (8 KB), sprite table (512 B) and, per image line, BXR and the map
             * line (2 x 16 bits; 0xFFFF = line outside the VDC) */
            fwrite(pce->ram, 1, sizeof(pce->ram), g_state_log);
            fwrite(pce->vdc.sat, 2, VDC_SAT_SIZE, g_state_log);
            for (int r = 0; r < VCE_ACTIVE_LINES; r++) {
                const VdcLineInfo *li = &g_line_log.row[r];
                uint16_t w[2] = { li->valid ? li->bxr : 0xFFFF, li->valid ? li->bgy : 0xFFFF };
                fwrite(w, 2, 2, g_state_log);
            }
        }
        if (hashes) {
            const Vce *v = &pce->vce;
            uint32_t crc_image = 0;
            for (int p = 0; p < v->frame_width * v->frame_height; p++) {
                uint8_t rgb[3] = { (uint8_t)(v->frame[p] >> 16), (uint8_t)(v->frame[p] >> 8), (uint8_t)v->frame[p] };
                crc_image = crc32_update(crc_image, rgb, 3);
            }
            uint32_t crc_audio = crc32_update(0, (const uint8_t *)pce->audio, (size_t)pce->audio_count * sizeof(int16_t));
            fprintf(hashes, "%ld %dx%d %08X %d %08X\n", i + 1, v->frame_width, v->frame_height, crc_image,
                    pce->audio_count, crc_audio);
        }
        if (g_lockstep_on && lockstep_end_frame(&g_lockstep) != 0) {
            printf("%s", g_lockstep.report);
            return 1;
        }
        if (audio)
            fwrite(pce->audio, sizeof(int16_t), (size_t)pce->audio_count, audio);
        if (g_every_dir && (i + 1) % g_every == 0 && i + 1 >= g_every_from) {
            char p[1024];
            snprintf(p, sizeof(p), "%s/q%06ld.ppm", g_every_dir, i + 1);
            save_ppm(pce, p);
        }
        if (pce->mix.on) {
            for (int s = 0; s < pce->audio_count / 2; s++)
                if (pce->psg.sfx_mask[s]) {
                    g_sfx_samples++;
                    if (g_first_sfx_frame < 0)
                        g_first_sfx_frame = i + 1;
                }
            g_chip_samples += pce->audio_count / 2;
        }
        pce->pad_buttons = input_script_update(&g_input, i + 1, pce->pad_buttons);
        if (g_ext && g_ext->input)
            g_ext->input(&g_run, i + 1);
    }
    if (audio)
        fclose(audio);
    if (hashes)
        fclose(hashes);
    if (g_state_log)
        fclose(g_state_log);
    if (g_batw) {
        fclose(g_batw);
        if (g_bw_routine)
            printf("bat-writers: routine %02X:%04X wrote to the map in %ld frames (1st: %ld)\n",
                   (unsigned)(g_bw_routine >> 16), (unsigned)(g_bw_routine & 0xFFFF), g_bw_routine_frames,
                   g_bw_routine_first);
    }
    int failed = g_ext && g_ext->done ? g_ext->done(&g_run, frames, path) : 0;
    int saved = save_ppm(pce, path);
    return failed ? 1 : saved;
}

/* Trace state, used by the instruction observer (works the same with recompiled code). */
static FILE *g_trace_out;
static long g_trace_count, g_trace_written, g_trace_from_frame;
static int g_trace_cycles; /* --cycles: appends the master clock to each line */
static uint64_t g_trace_frames_before;

static void trace_hook(Pce *pce, int after)
{
    const HuC6280 *c = &pce->cpu;
    if (!after) {
        g_trace_frames_before = pce->vce.frame_count;
        if ((long)pce->vce.frame_count >= g_trace_from_frame && g_trace_written < g_trace_count) {
            g_trace_written++;
            fprintf(g_trace_out, "%04X A:%02X X:%02X Y:%02X S:%02X P:%02X V:%d M:%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X",
                    c->pc, c->a, c->x, c->y, c->s, c->p, c->speed,
                    c->mpr[0], c->mpr[1], c->mpr[2], c->mpr[3], c->mpr[4], c->mpr[5], c->mpr[6], c->mpr[7]);
            if (g_trace_cycles)
                fprintf(g_trace_out, " C:%llu", (unsigned long long)pce->master_cycles);
            fputc('\n', g_trace_out);
        }
        return;
    }
    if (pce->vce.frame_count != g_trace_frames_before)
        pce->pad_buttons = input_script_update(&g_input, (long)pce->vce.frame_count, pce->pad_buttons);
    if (g_trace_written >= g_trace_count)
        pce->cpu.yield = 1;
}

/* Writes <count> trace lines, starting after <from_frame> frames. */
static int write_trace(Pce *pce, long count, long from_frame, const char *path)
{
    g_trace_out = fopen(path, "w");
    if (!g_trace_out) {
        printf("could not create %s\n", path);
        return 1;
    }
    g_trace_count = count;
    g_trace_from_frame = from_frame;
    g_trace_written = 0;
    pce->on_instruction = trace_hook;
    while (g_trace_written < count)
        pce_step(pce);
    pce->on_instruction = NULL;
    fclose(g_trace_out);
    return 0;
}

/* --interp-log: the instructions that went through the interpreter the most (bank:offset and how many times),
 * grouped into contiguous ranges, from the heaviest to the lightest */
static void write_interp_log(const Pce *pce, const char *path)
{
    FILE *f = u8_fopen(path, "w");
    if (!f)
        return;
    uint64_t total = 0;
    for (uint32_t i = 0; i < 256u * 0x2000u; i++)
        total += pce->interp_hist[i];
    fprintf(f, "# interpreted instructions: %llu\n# bank:start-end  instructions  %%\n", (unsigned long long)total);
    /* ranges: counted addresses less than 16 bytes apart */
    static uint32_t starts[65536], ends[65536];
    static uint64_t sums[65536];
    int n = 0;
    for (uint32_t i = 0; i < 256u * 0x2000u && n < 65536; i++) {
        if (!pce->interp_hist[i])
            continue;
        if (n && i - ends[n - 1] < 16 && (i >> 13) == (ends[n - 1] >> 13)) {
            ends[n - 1] = i;
            sums[n - 1] += pce->interp_hist[i];
        } else {
            starts[n] = ends[n] = i;
            sums[n++] = pce->interp_hist[i];
        }
    }
    for (int k = 0; k < 60; k++) {
        int best = -1;
        for (int j = 0; j < n; j++)
            if (sums[j] && (best < 0 || sums[j] > sums[best]))
                best = j;
        if (best < 0)
            break;
        fprintf(f, "%02X:%04X-%04X  %llu  %.2f\n", starts[best] >> 13, starts[best] & 0x1FFF, ends[best] & 0x1FFF,
                (unsigned long long)sums[best], total ? 100.0 * (double)sums[best] / (double)total : 0.0);
        sums[best] = 0;
    }
    fclose(f);
}


static void print_help(const char *prog)
{
    printf(TRACE_HELP, prog, prog);
    if (g_ext && g_ext->help)
        printf("\nGame-specific (%s):\n%s", g_ext->name ? g_ext->name : "extension", g_ext->help);
}

int pce_trace_main(int argc, char **argv, const TraceExt *ext)
{
    int frame_mode = 0;
    int interpret_only = 0;
    int gg_fader = 0;
    const char *bram_path = NULL;
    const char *bram_out_path = NULL;
    const char *dump_state_path = NULL;
    const char *volumes = NULL;
    const char *bios_profile_path = NULL;
    static BiosProfile bios_profile;
    static Hle hle;
    int use_hle = 0;
    const char *bios_reads_path = NULL;
    const char *hle_only = NULL;
    const char *input_path = NULL;
    const char *profile_path = NULL;
    const char *interp_log_path = NULL;
    long from_frame = 0;
    const char *pos[3];
    int npos = 0;
    g_ext = ext;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--frame") == 0)
            frame_mode = 1;
        else if (strcmp(argv[i], "--interpret") == 0)
            interpret_only = 1; /* ignores the recompiled code */
        else if (strcmp(argv[i], "--lockstep") == 0)
            g_lockstep_on = 1;
        else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc)
            input_path = argv[++i];
        else if (strcmp(argv[i], "--coverage") == 0 && i + 1 < argc)
            g_coverage_path = argv[++i];
        else if (strcmp(argv[i], "--audio") == 0 && i + 1 < argc)
            g_audio_path = argv[++i];
        else if (strcmp(argv[i], "--from-frame") == 0 && i + 1 < argc)
            from_frame = strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--bios") == 0 && i + 1 < argc)
            g_bios_path = argv[++i];
        else if (strcmp(argv[i], "--gg-fader") == 0)
            gg_fader = 1;
        else if (strcmp(argv[i], "--hashes") == 0 && i + 1 < argc)
            g_hashes_path = argv[++i];
        else if (strcmp(argv[i], "--bram") == 0 && i + 1 < argc)
            bram_path = argv[++i];
        else if (strcmp(argv[i], "--bram-out") == 0 && i + 1 < argc)
            bram_out_path = argv[++i];
        else if (strcmp(argv[i], "--every") == 0 && i + 2 < argc) {
            g_every = strtol(argv[++i], NULL, 10);
            g_every_dir = argv[++i];
        } else if (strcmp(argv[i], "--every-from") == 0 && i + 1 < argc) {
            g_every_from = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--volumes") == 0 && i + 1 < argc)
            volumes = argv[++i];
        else if (strcmp(argv[i], "--watch-vram") == 0 && i + 1 < argc)
            g_watch_vram = strtol(argv[++i], NULL, 16) & 0x7FFF;
        else if (strcmp(argv[i], "--bat-writers") == 0 && i + 1 < argc) {
            g_batw = u8_fopen(argv[++i], "w");
            if (!g_batw) {
                printf("could not create %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--bat-routine") == 0 && i + 1 < argc) {
            unsigned bank, addr;
            if (sscanf(argv[++i], "%x:%x", &bank, &addr) != 2) {
                printf("--bat-routine: BANK:ADDRESS in hex (e.g. 81:607E)\n");
                return 1;
            }
            g_bw_routine = (uint32_t)(bank & 0xFF) << 16 | (addr & 0xFFFF);
        } else if (strcmp(argv[i], "--watch-ram") == 0 && i + 1 < argc)
            g_watch_ram = strtol(argv[++i], NULL, 16) & 0x1FFF;
        else if (strcmp(argv[i], "--watch-vce") == 0 && i + 2 < argc) {
            g_watch_vce_from = atol(argv[++i]);
            g_watch_vce_to = atol(argv[++i]);
        }
        else if (strcmp(argv[i], "--state-log") == 0 && i + 1 < argc) {
            g_state_log = u8_fopen(argv[++i], "wb");
            if (!g_state_log) {
                printf("could not create %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--dump-state") == 0 && i + 1 < argc)
            dump_state_path = argv[++i];
        else if (strcmp(argv[i], "--bios-profile") == 0 && i + 1 < argc)
            bios_profile_path = argv[++i];
        else if (strcmp(argv[i], "--log-calls") == 0 && i + 1 < argc) {
            for (char *p = argv[++i]; *p && g_log_call_count < 16;) {
                long v = strtol(p, &p, 16);
                g_log_call_bank[g_log_call_count] = -1;
                if (*p == ':') { /* BANK:ADDRESS (game routine) */
                    g_log_call_bank[g_log_call_count] = (int)v;
                    v = strtol(p + 1, &p, 16);
                }
                g_log_calls[g_log_call_count++] = (uint16_t)v;
                if (*p)
                    p++;
            }
        } else if (strcmp(argv[i], "--bios-reads") == 0 && i + 1 < argc) {
            bios_reads_path = argv[++i];
        } else if (strcmp(argv[i], "--cycles") == 0)
            g_trace_cycles = 1;
        else if (strcmp(argv[i], "--hle") == 0)
            use_hle = 1;
        else if (strcmp(argv[i], "--hle-only") == 0 && i + 1 < argc)
            hle_only = argv[++i]; /* list of entries in hex (e.g. E009,E012): only these routines */
        else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc)
            profile_path = argv[++i];
        else if (strcmp(argv[i], "--interp-log") == 0 && i + 1 < argc)
            interp_log_path = argv[++i];
        else {
            int r = ext && ext->arg ? ext->arg(argc, argv, &i) : 0;
            if (r < 0)
                return 1;
            if (r == 0 && npos < 3)
                pos[npos++] = argv[i];
        }
    }
    if (npos < 3) {
        print_help(argv[0]);
        return 2;
    }
    long count = strtol(pos[1], NULL, 10);
    if (input_path && input_script_load(&g_input, input_path) != 0) {
        printf("could not read the input script %s\n", input_path);
        return 1;
    }

    if (load_media(&g_pce, pos[0]) != 0) {
        printf("could not load %s\n", pos[0]);
        return 1;
    }
    pce_reset(&g_pce);
    if (g_batw) {
        g_bw_pce = &g_pce;
        g_pce.vdc.write_hook = bw_hook;
        const char *det = getenv("PCE_BW_DETAIL");
        if (det && sscanf(det, "%ld-%ld", &g_bw_detail_from, &g_bw_detail_to) != 2)
            g_bw_detail_from = -1;
    }
    if (g_pce.cd)
        g_pce.cd->fader_geargrafx = gg_fader;
    if (g_state_log)
        g_pce.vdc.line_log = &g_line_log;
    if (volumes) {
        AudioVolumes vol = { 0 };
        if (sscanf(volumes, "%d,%d,%d,%d,%d,%d", &vol.master, &vol.music, &vol.chip_music, &vol.effects, &vol.voices,
                   &vol.chip_filter) < 5) {
            printf("--volumes: master,cd_music,chip_music,effects,voices[,chip_filter] (e.g. 100,0,0,100,0,1)\n");
            return 1;
        }
        audio_mix_mednafen(&g_pce.mix, &vol);
    }
    g_run.pce = &g_pce;
    g_run.ref = g_lockstep_on ? &g_ref : NULL;
    g_run.input = &g_input;
    g_run.line_log = &g_line_log;
    g_run.frame_mode = frame_mode;
    g_run.lockstep = g_lockstep_on;
    g_run.volumes = volumes != NULL;
    if (ext && ext->setup && ext->setup(&g_run) != 0)
        return 1;
    if (bram_path && pce_load_bram(&g_pce, bram_path) != 0) {
        printf("could not load the save %s\n", bram_path);
        return 1;
    }
    if (use_hle && !hle_only)
        hle_init(&hle, 0);
    for (const char *p = hle_only; p && *p;) {
        static Hle one;
        char *end;
        hle_init(&one, (uint16_t)strtol(p, &end, 16));
        for (int i = 0; i < 0x2000; i++)
            if (one.fn[i])
                hle.fn[i] = one.fn[i];
        p = *end ? end + 1 : end;
    }
    if (use_hle || hle_only)
        g_pce.hle = &hle;
    if (g_log_call_count)
        g_pce.on_instruction = log_calls_hook;
    static uint8_t bios_reads[0x40000];
    if (bios_reads_path)
        g_pce.bios_reads = bios_reads;
    if (bios_profile_path) {
        bios_profile_init(&bios_profile, &g_pce);
        g_pce.bios_profile = &bios_profile;
    }
#ifdef PCE_RECOMP
    if (!interpret_only) {
        if (!recomp_rom_crc32 || g_pce.rom_crc32 == recomp_rom_crc32) /* 0 = CD game */
            g_pce.recomp_lookup = recomp_lookup;
        else
            fprintf(stderr, "ROM differs from the one used for recompilation: using only the interpreter\n");
    }
#endif
    (void)interpret_only;
    g_pce.pad_buttons = input_script_update(&g_input, 0, 0);
    if (g_lockstep_on) {
        if (!frame_mode) {
            printf("--lockstep works together with --frame\n");
            return 2;
        }
        if (load_media(&g_ref, pos[0]) != 0)
            return 1;
        pce_reset(&g_ref);
        if (bram_path)
            pce_load_bram(&g_ref, bram_path);
        if (g_ref.cd)
            g_ref.cd->fader_geargrafx = gg_fader;
        lockstep_init(&g_lockstep, &g_pce, &g_ref);
    }
    if (g_coverage_path) {
        if (coverage_init(&g_coverage) != 0)
            return 1;
        g_pce.coverage = &g_coverage;
    }

    if (interp_log_path)
        g_pce.interp_hist = (uint32_t *)calloc(256u * 0x2000u, sizeof(uint32_t));
    if (profile_path)
        profiler_start();
    clock_t t0 = clock();
    int result = frame_mode ? dump_frame(&g_pce, count, pos[2]) : write_trace(&g_pce, count, from_frame, pos[2]);
    if (profile_path) {
        double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
        profiler_stop(profile_path);
        fprintf(stderr, "Time: %.2f s", secs);
        if (frame_mode && secs > 0)
            fprintf(stderr, " (%.0f frames/s)", count / secs);
        fprintf(stderr, "\n");
    }
    fflush(stdout); /* otherwise the line below can land in the middle of a stdout line (and the tests miss it) */
    if (g_pce.recomp_lookup)
        fprintf(stderr, "recompiled code: %llu function entries; interpreter: %llu instructions\n",
                (unsigned long long)g_pce.recomp_calls, (unsigned long long)g_pce.interpreted_steps);
    if (g_pce.interp_hist)
        write_interp_log(&g_pce, interp_log_path);
    if (g_lockstep_on && !g_lockstep.diverged)
        printf("LOCKSTEP OK: %llu instructions and %llu frames identical to the interpreter\n",
               (unsigned long long)g_lockstep.instructions, (unsigned long long)g_lockstep.frames);
    if (g_lockstep_on)
        pce_free(&g_ref);
    if (bios_profile_path && bios_profile_save(&bios_profile, bios_profile_path) != 0) {
        printf("could not write the BIOS profile to %s\n", bios_profile_path);
        result = 1;
    }
    if (bios_reads_path) {
        FILE *f = fopen(bios_reads_path, "w");
        for (int i = 0; f && i < 0x40000; i++) {
            if (!bios_reads[i] || (i && bios_reads[i - 1]))
                continue;
            int j = i;
            while (j + 1 < 0x40000 && bios_reads[j + 1])
                j++;
            fprintf(f, "bank %02X: %04X-%04X\n", i >> 13, 0x0000 | (i & 0x1FFF), 0x0000 | (j & 0x1FFF));
        }
        if (f)
            fclose(f);
    }
    if (dump_state_path) {
        FILE *f = fopen(dump_state_path, "wb");
        if (f) {
            fwrite(g_pce.vdc.vram, sizeof(g_pce.vdc.vram), 1, f);
            fwrite(g_pce.vdc.sat, sizeof(g_pce.vdc.sat), 1, f);
            fwrite(g_pce.vce.color, sizeof(g_pce.vce.color), 1, f);
            fwrite(g_pce.vdc.reg, sizeof(g_pce.vdc.reg), 1, f);
            fwrite(g_pce.ram, sizeof(g_pce.ram), 1, f);
            if (g_pce.cd)
                fwrite(g_pce.cd->ram, sizeof(g_pce.cd->ram), 1, f);
            fclose(f);
        } else {
            printf("could not write %s\n", dump_state_path);
            result = 1;
        }
    }
    if (bram_out_path && pce_save_bram(&g_pce, bram_out_path) != 0) {
        printf("could not write the save to %s\n", bram_out_path);
        result = 1;
    }
    if (volumes)
        fprintf(stderr, "Chip: effects in %.1f%% of the samples (first at frame %ld)\n",
                g_chip_samples ? 100.0 * g_sfx_samples / g_chip_samples : 0.0, g_first_sfx_frame);
    if (g_pce.cd)
        fprintf(stderr, "CD: disc music played %.1f s; ADPCM played %.1f s\n",
                g_pce.cd->audio.played_samples / 44100.0, g_pce.cd->adpcm.played_samples / 44100.0);
    if (g_pce.hle && g_pce.hle->missing) {
        printf("FAILED: a BIOS routine in C is missing (see message above)\n");
        result = 1;
    }
    if (g_pce.cd && g_pce.cd->unimplemented) {
        printf("FAILED: the game used a part of the CD not yet implemented (see message above)\n");
        result = 1;
    }

    if (g_coverage_path) {
        if (coverage_write(&g_coverage, g_coverage_path, g_pce.rom_crc32) != 0) {
            printf("could not write %s\n", g_coverage_path);
            result = 1;
        }
        coverage_free(&g_coverage);
    }
    pce_free(&g_pce);
    return result;
}
