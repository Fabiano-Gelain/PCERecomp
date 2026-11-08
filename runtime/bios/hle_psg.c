/*
 * C BIOS: the sound chip (PSG) driver — psg_bios ($E0D8) and the driver step ($E6E8), called
 * by the timer or VBlank interrupt. See hle.h.
 *
 * The driver plays two independent "programs", music and effects, each with 6 channels; an effect has
 * priority over the music on the same channel. Each channel reads a byte sequence (in game memory,
 * banks $22EB/$22EC in windows 4 and 5): $00-$CF note (high nibble: note 1-12, 0 = rest; low:
 * duration) and $D0-$FF commands (octave, volume, pan, waveform, envelope, repeats, calls...). On each
 * step, the channel updates the volume envelope, pitch effects and slides and writes to the PSG.
 *
 * Music and effects use the same code with variables in different places (Eng): the music's per-channel
 * tables are at $22EE-$2455 and $2456-$248A; the effects', $19D and $18B bytes later. The names in the
 * comments are the music addresses.
 *
 * Large tables (note periods, tempos, envelopes and factory waveforms) are read from
 * memory, at the same BIOS addresses (bank 2 in window 6): the console BIOS's own if present; otherwise
 * those of the synthetic BIOS (bios/synth.c).
 *
 * The step is split into small pieces (private points in $FEB0-$FEFF, a BIOS area that is never
 * executed), so the VDC interrupts can come in the middle, as on the real BIOS.
 */
#include "bios/alu.h"
#include "bios/hle.h"
#include "bios/internal.h"
#include "bios/routines.h"

#define A (pce->cpu.a)
#define X (pce->cpu.x)
#define Y (pce->cpu.y)

static uint8_t m(Pce *pce, uint16_t a) { return hle_rd(pce, a); }
static void mw(Pce *pce, uint16_t a, uint8_t v) { hle_wr(pce, a, v); }
static uint8_t z(Pce *pce, uint8_t n) { return hle_rd(pce, ZP(n)); }
static void zw(Pce *pce, uint8_t n, uint8_t v) { hle_wr(pce, ZP(n), v); }
static void go(Pce *pce, uint16_t pc) { pce->cpu.pc = pc; }

/* Piece timing: each function adds up the cost (CPU cycles) of the real BIOS blocks it imitates, path by
 * path (branch taken = +2), and the piece ends with the total (the step timing decides where the timer
 * interrupt lands in the frame). The comments with addresses are the music blocks ($C2EF-$CD11);
 * the effects have a copy of the code with the same timings ($CD12-$D713), except where noted. */
static void T(Pce *pce, int n) { pce->hle->psg_t += n; }

static void piece_end(Pce *pce, uint16_t next)
{
    hle_cycles(pce, pce->hle->psg_t);
    go(pce, next);
}

static void tam(Pce *pce, int win, uint8_t v)
{
    pce->cpu.mpr[win] = v;
    pce->cpu.mpr_buffer = v;
}

/* ---- Music or effects ---- */

typedef struct Eng {
    uint16_t lo, hi;       /* offset of the tables below / from $2456 on */
    uint16_t stack;        /* stack of repeats and calls: 12 (music) or 9 (effects) bytes per channel */
    uint8_t stack_step;
    int sfx;
    uint16_t pt;           /* first private point of this program */
} Eng;

static const Eng k_music = {0x000, 0x000, 0x240E, 12, 0, 0xFEC0};
static const Eng k_sfx = {0x19D, 0x18B, 0x25AB, 9, 1, 0xFEE0};

/* Private points (added to Eng.pt) */
enum { P_START, P_CH, P_SEQ, P_SLIDES, P_PATTERN, P_ENV, P_PITCH, P_NEXT, P_OUT, P_WAVE, P_COUNT };

static uint16_t ad(const Eng *e, uint16_t a) { return (uint16_t)(a + (a >= 0x2456 ? e->hi : e->lo)); }
static uint8_t rv(Pce *pce, const Eng *e, uint16_t a, int x) { return m(pce, (uint16_t)(ad(e, a) + x)); }
static void wv(Pce *pce, const Eng *e, uint16_t a, int x, uint8_t v) { mw(pce, (uint16_t)(ad(e, a) + x), v); }

/* Current channel ($22E0) */
static int cur(Pce *pce) { return m(pce, 0x22E0); }

/* Zero-page pointers: ($E8) sequence, ($EA) auxiliary */
static uint16_t zp16(Pce *pce, uint8_t lo) { return (uint16_t)(z(pce, lo) | (z(pce, (uint8_t)(lo + 1)) << 8)); }

static void zp16w(Pce *pce, uint8_t lo, uint16_t v)
{
    zw(pce, lo, (uint8_t)v);
    zw(pce, (uint8_t)(lo + 1), (uint8_t)(v >> 8));
}

static uint8_t ind(Pce *pce, uint8_t zp, uint8_t y) { return m(pce, (uint16_t)(zp16(pce, zp) + y)); }
static void ind_w(Pce *pce, uint8_t zp, uint8_t y, uint8_t v) { mw(pce, (uint16_t)(zp16(pce, zp) + y), v); }

/* INC zp / BNE / INC zp+1 */
static void inc16(Pce *pce, uint8_t lo)
{
    uint8_t v = (uint8_t)(z(pce, lo) + 1);
    zw(pce, lo, v);
    if (!v)
        zw(pce, (uint8_t)(lo + 1), (uint8_t)(z(pce, (uint8_t)(lo + 1)) + 1));
    T(pce, v ? 10 : 14);
}

/* LDA ($E8); INC $E8 */
static uint8_t fetch(Pce *pce)
{
    uint8_t v = ind(pce, 0xE8, 0);
    T(pce, 7);
    inc16(pce, 0xE8);
    return v;
}

/* Envelopes: number < $10 = factory (addresses at $D8A1); otherwise from the game's table ($22D5).
 * ($EA) = entry n - $10 of the game's table */
static void game_envelope_entry(Pce *pce, uint8_t n)
{
    alu_setc(pce, 0);
    zw(pce, 0xEA, alu_adc(pce, (uint8_t)((n - 0x10) * 2), m(pce, 0x22D5)));
    zw(pce, 0xEB, alu_adc(pce, 0, m(pce, 0x22D6)));
}

/* ($EA) = envelope n */
static void envelope_pointer(Pce *pce, uint8_t n)
{
    if (n < 0x10) {
        zw(pce, 0xEA, m(pce, (uint16_t)(0xD8A1 + n * 2)));
        zw(pce, 0xEB, m(pce, (uint16_t)(0xD8A2 + n * 2)));
        return;
    }
    game_envelope_entry(pce, n);
    uint8_t lo = ind(pce, 0xEA, 0);
    inc16(pce, 0xEA);
    zw(pce, 0xEB, ind(pce, 0xEA, 0));
    zw(pce, 0xEA, lo);
}

/* C83D: ($EA) = stack of channel x, returns the index (low nibble of $2456) */
static uint8_t stack_top(Pce *pce, const Eng *e, int x)
{
    uint16_t base = (uint16_t)(e->stack + x * e->stack_step);
    zw(pce, 0xEA, (uint8_t)base);
    zw(pce, 0xEB, (uint8_t)(base >> 8));
    return (uint8_t)(rv(pce, e, 0x2456, x) & 0x0F);
}

static void stack_set(Pce *pce, const Eng *e, int x, uint8_t idx)
{
    zw(pce, 0xEA, idx);
    wv(pce, e, 0x2456, x, (uint8_t)((rv(pce, e, 0x2456, x) & 0xF0) | idx));
}

/* ======== Start of a music / effect (C330) ======== */

static void track_start(Pce *pce, const Eng *e)
{
    uint8_t req = m(pce, 0x22E1);
    if (req & 0x80) {
        T(pce, 7 + 7); /* C330, RTS */
        return;
    }
    zw(pce, 0xEA, m(pce, 0x22D0));
    zw(pce, 0xEB, m(pce, 0x22D1));
    uint8_t y = (uint8_t)(req * 2);
    zw(pce, 0xE8, ind(pce, 0xEA, y));
    zw(pce, 0xE9, ind(pce, 0xEA, (uint8_t)(y + 1)));
    uint8_t mask = ind(pce, 0xE8, 0);
    if (e->sfx ? !(mask & 0x80) : (mask & 0x80) != 0) {
        T(pce, 9 + 57 + 7); /* C330, C336, RTS */
        return;
    }
    T(pce, 9 + 55 + 12); /* C330, C336, C34F */
    zw(pce, 0xEA, mask);
    zw(pce, 0xEB, mask);
    y = 0;
    for (int x = 0; x < 6; x++) {
        uint8_t ea = z(pce, 0xEA);
        zw(pce, 0xEA, (uint8_t)(ea >> 1));
        T(pce, 8 + 6 + (x < 5 ? 2 : 0)); /* C355, C3CB */
        if (!(ea & 1)) {
            T(pce, 2);
            continue;
        }
        T(pce, 165 + (x < 4 ? 2 : 5) + 11 + ((z(pce, 0xEB) & 0x40) ? 10 : 2)); /* C359, C3BC, C3BF, C3C5 */
        wv(pce, e, 0x247C, x, 1);
        wv(pce, e, 0x230C, x, 1);
        static const uint16_t clear[] = {0x2482, 0x2318, 0x2312, 0x238A, 0x23AE, 0x2396, 0x23BA, 0x23E4,
                                         0x23F6, 0x2456, 0x2330, 0x23C0, 0x23CC, 0x23D8, 0x2336};
        for (unsigned i = 0; i < sizeof(clear) / sizeof(clear[0]); i++)
            wv(pce, e, clear[i], x, 0);
        wv(pce, e, 0x233C, x, m(pce, 0xD8A1));
        wv(pce, e, 0x2342, x, m(pce, 0xD8A2));
        wv(pce, e, 0x236C, x, 0x84);
        wv(pce, e, 0x22FA, x, 0x04);
        y++;
        uint8_t lo = ind(pce, 0xE8, y);
        wv(pce, e, 0x22EE, x, lo);
        wv(pce, e, 0x2324, x, lo);
        y++;
        uint8_t hi = ind(pce, 0xE8, y);
        wv(pce, e, 0x22F4, x, hi);
        wv(pce, e, 0x232A, x, hi);
        if (x >= 4)
            wv(pce, e, 0x2476, x, 0x80);
        wv(pce, e, 0x2474, x, 0x80);
        if (z(pce, 0xEB) & 0x40)
            wv(pce, e, 0x2474, x, m(pce, 0x22E2));
    }
    T(pce, e->sfx ? 19 + 16 : 43); /* C3D0 (efeitos: CDE5, CDF0) */
    wv(pce, e, 0x2488, 0, 0);
    wv(pce, e, 0x248A, 0, 0);
    mw(pce, 0x22E1, (uint8_t)(m(pce, 0x22E1) | 0x80));
    if (e->sfx)
        mw(pce, 0x22DE, (uint8_t)(m(pce, 0x22DE) & ~0x40));
    else
        mw(pce, 0x22DE, (uint8_t)((m(pce, 0x22DE) & 0x70) | m(pce, 0x22DD)));
}

/* ======== Sequence (C3E7) ======== */

/* Note: duration, sounding time (gate), pitch. Returns with the sequence pointer saved. */
static void note(Pce *pce, const Eng *e, int ch, uint8_t b)
{
    uint8_t mult = rv(pce, e, 0x2336, ch), len;
    if (mult) {
        uint8_t a = (uint8_t)((b & 0x0F) + 1), x = mult;
        T(pce, 7 + 11); /* C46E, C473 */
        if (a < mult) {
            x = a;
            a = mult;
            T(pce, 3); /* SAX */
        } else {
            T(pce, 2);
        }
        T(pce, 10 + (x - 1) * 12 + 2); /* C47C and the loop C482/C47F */
        zw(pce, 0xEA, a);
        len = a;
        alu_setc(pce, 0);
        while (--x)
            len = alu_adc(pce, len, a);
    } else {
        T(pce, 9); /* C46E */
        len = fetch(pce);
    }
    wv(pce, e, 0x230C, ch, len);
    uint8_t gate = len;
    T(pce, 21); /* C48E */
    if (ind(pce, 0xE8, 0) != 0xDA) {
        uint8_t step = (uint8_t)(len >> 3);
        zw(pce, 0xEA, step);
        alu_setc(pce, 1);
        uint8_t a = rv(pce, e, 0x230C, ch);
        uint8_t q0 = rv(pce, e, 0x2318, ch);
        T(pce, 22 + q0 * 12 + 4); /* C499 and the loop C4A5/C4A7 */
        for (uint8_t q = q0; q; q--)
            a = alu_sbc(pce, a, step);
        gate = a;
    } else {
        T(pce, 2);
    }
    wv(pce, e, 0x231E, ch, gate);
    uint8_t state;
    T(pce, 13); /* C4AC */
    if (!(b & 0xF0)) {
        state = 0; /* rest */
        T(pce, 6); /* C4B4 */
    } else {
        uint8_t n2 = (uint8_t)(((b & 0xF0) - 0x10) >> 3);
        uint8_t mode = (uint8_t)(rv(pce, e, 0x2456, ch) & 0xF0);
        int tone = mode == 0 || (mode != 0x10 && ch < 4);
        T(pce, 2 + 24); /* C4B8 */
        if (mode == 0x10) {
            /* drum mode: the note selects a sequence of sounds ($22DB) */
            T(pce, 4 + 34); /* C4C7, C4CB */
            alu_setc(pce, 0);
            zw(pce, 0xEA, alu_adc(pce, n2, m(pce, 0x22DB)));
            zw(pce, 0xEB, alu_adc(pce, 0, m(pce, 0x22DC)));
            wv(pce, e, 0x2402, ch, ind(pce, 0xEA, 0));
            inc16(pce, 0xEA);
            wv(pce, e, 0x2408, ch, ind(pce, 0xEA, 0));
            T(pce, 16); /* C4E2 */
        } else if (!tone) {
            /* noise */
            T(pce, 4 + 2 + 4 + 18); /* C4C7, C4E9, C4ED */
            wv(pce, e, 0x2476, ch, (uint8_t)((uint8_t)(n2 + rv(pce, e, 0x2330, ch)) >> 1));
        } else {
            T(pce, mode == 0 ? 2 : 4 + 2 + 4 + 2); /* up to C4FF */
            T(pce, 43 + (ch < 4 ? 2 : 7)); /* C4FF, C51A */
            /* start of the octave in the period table (BIOS table right before the code, read from memory) */
            uint8_t o = rv(pce, e, 0x22FA, ch);
            alu_setc(pce, 0);
            uint8_t i = alu_adc(pce, n2, m(pce, (uint16_t)((e->sfx ? 0xCF05 : 0xC4F6) + o)));
            i = alu_adc(pce, i, rv(pce, e, 0x2330, ch));
            wv(pce, e, 0x2300, ch, m(pce, (uint16_t)(0xD71A + i)));
            wv(pce, e, 0x2306, ch, m(pce, (uint16_t)(0xD71B + i)));
            if (ch >= 4)
                wv(pce, e, 0x2476, ch, 0x80);
        }
        T(pce, 9); /* C51F */
        if (rv(pce, e, 0x2482, ch) == 0x03) {
            state = 1; /* tied to the previous one */
            T(pce, 6); /* C526 */
        } else {
            T(pce, 2 + 42 + 12); /* C52A, C543 */
            wv(pce, e, 0x2384, ch, 0);
            wv(pce, e, 0x23A8, ch, 0);
            wv(pce, e, 0x23EA, ch, 0);
            wv(pce, e, 0x23F0, ch, 0);
            wv(pce, e, 0x2390, ch, rv(pce, e, 0x238A, ch));
            wv(pce, e, 0x23B4, ch, rv(pce, e, 0x23AE, ch));
            wv(pce, e, 0x23FC, ch, rv(pce, e, 0x23F6, ch));
            state = 2;
        }
    }
    wv(pce, e, 0x2482, ch, state);
    wv(pce, e, 0x22EE, ch, z(pce, 0xE8));
    wv(pce, e, 0x22F4, ch, z(pce, 0xE9));
    T(pce, 30); /* C54B, including the RTS */
}

/* A command byte ($D0-$FF). Returns 1 if the channel's sequence stopped on this step. */
static int command(Pce *pce, const Eng *e, int ch, uint8_t c)
{
    uint8_t p;
    /* The table jump was already counted (C405); here, the command. "break" = with parameter: C834 (INC $E8,
     * JMP C3F8); "return 0" = returns directly to C3F8. */
    switch (c) {
    case 0xD0: /* duration multiplier */
        T(pce, 18);
        wv(pce, e, 0x2336, ch, (uint8_t)(ind(pce, 0xE8, 0) & 0x0F));
        break;
    case 0xD1: case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD7: /* octave */
        T(pce, 11);
        wv(pce, e, 0x22FA, ch, (uint8_t)(c & 0x07));
        return 0;
    case 0xD8: /* octave + 1 */
        T(pce, 16);
        wv(pce, e, 0x22FA, cur(pce), (uint8_t)(rv(pce, e, 0x22FA, cur(pce)) + 1));
        return 0;
    case 0xD9: /* octave - 1 */
        T(pce, 16);
        wv(pce, e, 0x22FA, cur(pce), (uint8_t)(rv(pce, e, 0x22FA, cur(pce)) - 1));
        return 0;
    case 0xDA: /* tie the next note */
        T(pce, 11);
        wv(pce, e, 0x2482, ch, 0x03);
        return 0;
    case 0xDB: /* tempo (music only; in effects the table returns directly to CE07) */
        if (e->sfx)
            return 0;
        T(pce, 29); /* C585, C58B, C58D */
        p = ind(pce, 0xE8, 0);
        mw(pce, 0x22D4, m(pce, (uint16_t)(0xD7A1 + (p < 0x23 ? 0x23 : p))));
        break;
    case 0xDC: /* volume */
        T(pce, 29);
        alu_setc(pce, 1);
        wv(pce, e, 0x2312, ch, alu_sbc(pce, (uint8_t)(ind(pce, 0xE8, 0) & 0x1F), 0x1F));
        wv(pce, e, 0x23C0, ch, 0);
        break;
    case 0xDD: /* pan */
        T(pce, 28);
        wv(pce, e, 0x246E, ch, ind(pce, 0xE8, 0));
        wv(pce, e, 0x23CC, ch, 0);
        wv(pce, e, 0x23D8, ch, 0);
        break;
    case 0xDE: /* note sounding time */
        T(pce, 26);
        alu_setc(pce, 1);
        p = alu_sbc(pce, (uint8_t)((ind(pce, 0xE8, 0) - 1) & 0x07), 0x08);
        wv(pce, e, 0x2318, ch, (uint8_t)(p ^ 0xFF));
        break;
    case 0xDF: { /* relative volume */
        alu_setc(pce, 0);
        uint8_t v = alu_adc(pce, rv(pce, e, 0x2312, ch), ind(pce, 0xE8, 0));
        T(pce, ((v & 0x80) ? 24 : 22) + 9); /* C5C7, C5CF or C5D2/C5D6, C5D8 */
        if (!(v & 0x80))
            v = 0;
        else if (v < 0xE1)
            v = 0xE1;
        wv(pce, e, 0x2312, ch, v);
        break;
    }
    case 0xE1: /* return to the marked point */
        T(pce, 22);
        zw(pce, 0xE8, rv(pce, e, 0x2324, ch));
        zw(pce, 0xE9, rv(pce, e, 0x232A, ch));
        return 0;
    case 0xE2: /* mark the point */
        T(pce, 22);
        wv(pce, e, 0x2324, ch, z(pce, 0xE8));
        wv(pce, e, 0x232A, ch, z(pce, 0xE9));
        return 0;
    case 0xE3: { /* repeat start */
        uint8_t n = ind(pce, 0xE8, 0);
        if (!n)
            n = 2;
        T(pce, 14 + 3); /* C5F8, C5FD, PHA */
        inc16(pce, 0xE8);
        T(pce, 7 + 34 + 66); /* JSR C83D, C609 */
        uint8_t y = stack_top(pce, e, ch);
        ind_w(pce, 0xEA, y++, z(pce, 0xE8));
        ind_w(pce, 0xEA, y++, z(pce, 0xE9));
        ind_w(pce, 0xEA, y++, n);
        stack_set(pce, e, ch, y);
        return 0;
    }
    case 0xE4: { /* repeat end */
        uint8_t y = (uint8_t)(stack_top(pce, e, ch) - 1);
        uint8_t n = (uint8_t)(ind(pce, 0xEA, y) - 1);
        T(pce, 10 + 34 + 13 + (n ? 40 : 2 + 31)); /* C627, C83D, C62B, C631 or C641 */
        if (n) {
            ind_w(pce, 0xEA, y, n);
            zw(pce, 0xE9, ind(pce, 0xEA, (uint8_t)(y - 1)));
            zw(pce, 0xE8, ind(pce, 0xEA, (uint8_t)(y - 2)));
        } else {
            stack_set(pce, e, ch, (uint8_t)(y - 2));
        }
        return 0;
    }
    case 0xE5: /* waveform */
        T(pce, 16);
        wv(pce, e, 0x2474, ch, ind(pce, 0xE8, 0));
        break;
    case 0xE6: { /* volume envelope */
        uint8_t n = ind(pce, 0xE8, 0);
        T(pce, 14 + (n < 0x10 ? 26 : 2 + 37)); /* C65B, C662 or C670 */
        envelope_pointer(pce, n);
        if (n >= 0x10)
            T(pce, 15); /* C689 */
        wv(pce, e, 0x2366, ch, 0);
        wv(pce, e, 0x236C, ch, 0);
        if (ind(pce, 0xEA, 0) == 0xFB) {
            T(pce, 21 + 50); /* C68F, C69B */
            wv(pce, e, 0x2366, ch, ind(pce, 0xEA, 1));
            wv(pce, e, 0x236C, ch, ind(pce, 0xEA, 2));
            zp16w(pce, 0xEA, (uint16_t)(zp16(pce, 0xEA) + 3));
        } else {
            T(pce, 23);
        }
        T(pce, 45); /* C6B4 */
        wv(pce, e, 0x233C, ch, z(pce, 0xEA));
        wv(pce, e, 0x2342, ch, z(pce, 0xEB));
        wv(pce, e, 0x2372, ch, 0);
        wv(pce, e, 0x2354, ch, 0);
        wv(pce, e, 0x235A, ch, 0);
        wv(pce, e, 0x2360, ch, 0);
        break;
    }
    case 0xE7: { /* pitch envelope */
        T(pce, 70);
        uint8_t y = (uint8_t)(ind(pce, 0xE8, 0) * 2);
        zw(pce, 0xEA, m(pce, 0x22D7));
        zw(pce, 0xEB, m(pce, 0x22D8));
        wv(pce, e, 0x2378, ch, ind(pce, 0xEA, y));
        wv(pce, e, 0x237E, ch, ind(pce, 0xEA, (uint8_t)(y + 1)));
        wv(pce, e, 0x2384, ch, 0);
        break;
    }
    case 0xE8:
        T(pce, 16);
        wv(pce, e, 0x238A, ch, ind(pce, 0xE8, 0));
        break;
    case 0xE9:
        T(pce, 18);
        wv(pce, e, 0x2396, ch, (uint8_t)(ind(pce, 0xE8, 0) & 0x07));
        break;
    case 0xEA: { /* modulation */
        T(pce, 70);
        uint8_t y = (uint8_t)(ind(pce, 0xE8, 0) * 2);
        zw(pce, 0xEA, m(pce, 0x22D9));
        zw(pce, 0xEB, m(pce, 0x22DA));
        wv(pce, e, 0x239C, ch, ind(pce, 0xEA, y));
        wv(pce, e, 0x23A2, ch, ind(pce, 0xEA, (uint8_t)(y + 1)));
        wv(pce, e, 0x23A8, ch, 0);
        break;
    }
    case 0xEB:
        T(pce, 16);
        wv(pce, e, 0x23AE, ch, ind(pce, 0xE8, 0));
        break;
    case 0xEC:
        T(pce, 16);
        wv(pce, e, 0x23BA, ch, ind(pce, 0xE8, 0));
        break;
    case 0xED:
        T(pce, 16);
        wv(pce, e, 0x23E4, ch, ind(pce, 0xE8, 0));
        break;
    case 0xEE:
        T(pce, 16);
        wv(pce, e, 0x23F6, ch, ind(pce, 0xE8, 0));
        break;
    case 0xEF: { /* jump */
        uint8_t lo = ind(pce, 0xE8, 0);
        T(pce, 9); /* C742 */
        inc16(pce, 0xE8);
        T(pce, 19); /* C74B */
        zw(pce, 0xE9, ind(pce, 0xE8, 0));
        zw(pce, 0xE8, lo);
        return 0;
    }
    case 0xF0: { /* call */
        T(pce, 10 + 34 + 55); /* C754, C83D, C758 */
        uint8_t y = stack_top(pce, e, ch);
        ind_w(pce, 0xEA, y++, z(pce, 0xE8));
        ind_w(pce, 0xEA, y++, z(pce, 0xE9));
        stack_set(pce, e, ch, y);
        uint8_t lo = ind(pce, 0xE8, 0);
        inc16(pce, 0xE8);
        T(pce, 22); /* C777 */
        zw(pce, 0xE9, ind(pce, 0xE8, 0));
        zw(pce, 0xE8, lo);
        return 0;
    }
    case 0xF1: { /* return from call */
        T(pce, 10 + 34 + 46); /* C781, C83D, C785 */
        uint8_t y = stack_top(pce, e, ch);
        y--;
        zw(pce, 0xE9, ind(pce, 0xEA, y));
        y--;
        zw(pce, 0xE8, ind(pce, 0xEA, y));
        stack_set(pce, e, ch, y);
        inc16(pce, 0xE8);
        T(pce, 7); /* C7A1 */
        break;
    }
    case 0xF2: /* transpose */
        T(pce, 18);
        wv(pce, e, 0x2330, ch, (uint8_t)(ind(pce, 0xE8, 0) * 2));
        break;
    case 0xF3: /* relative transpose */
        T(pce, 33);
        zw(pce, 0xEA, (uint8_t)(ind(pce, 0xE8, 0) * 2));
        wv(pce, e, 0x2330, ch, (uint8_t)(rv(pce, e, 0x2330, ch) + z(pce, 0xEA)));
        break;
    case 0xF4: { /* transpose all playing channels */
        uint8_t t = (uint8_t)(ind(pce, 0xE8, 0) * 2);
        T(pce, 16 + 8); /* C7BE, C7D1 */
        for (int x = 5; x >= 0; x--) {
            int on = rv(pce, e, 0x247C, x) != 0;
            T(pce, (on ? 14 : 9) + 4 + (x ? 2 : 0)); /* C7C5, C7CA, C7CE */
            if (on)
                wv(pce, e, 0x2330, x, t);
        }
        break;
    }
    case 0xF5:
        T(pce, 23);
        wv(pce, e, 0x23C0, ch, ind(pce, 0xE8, 0));
        wv(pce, e, 0x23C6, ch, 0);
        break;
    case 0xF6:
        T(pce, 23);
        wv(pce, e, 0x23CC, ch, ind(pce, 0xE8, 0));
        wv(pce, e, 0x23D2, ch, 0);
        break;
    case 0xF7:
        T(pce, 23);
        wv(pce, e, 0x23D8, ch, ind(pce, 0xE8, 0));
        wv(pce, e, 0x23DE, ch, 0);
        break;
    case 0xF8: { /* channel mode (tone, drum, noise) */
        uint8_t v = ind(pce, 0xE8, 0);
        T(pce, v == 0x01 ? 46 + 16 : 48); /* C7F5, C80D */
        zw(pce, 0xEA, (uint8_t)(v << 4));
        wv(pce, e, 0x2456, ch, (uint8_t)((rv(pce, e, 0x2456, ch) & 0x0F) | (uint8_t)(v << 4)));
        if (v == 0x01) {
            wv(pce, e, 0x2366, ch, 0);
            wv(pce, e, 0x236C, ch, 0);
        }
        break;
    }
    case 0xFE: { /* fade */
        uint8_t v = ind(pce, 0xE8, 0);
        T(pce, !(v & 0x7F) ? 13 : (v & 0x80) ? 38 : 36); /* C816, C81C, C820, C823 */
        if (v & 0x7F) {
            if (v & 0x80)
                v = (uint8_t)(-v);
            wv(pce, e, 0x2488, 0, v);
            wv(pce, e, 0x2489, 0, 0);
            wv(pce, e, 0x248A, 0, 0);
        }
        break;
    }
    case 0xFF: /* channel end */
        T(pce, 14); /* C82E, including the RTS */
        wv(pce, e, 0x247C, ch, 0x02);
        return 1;
    default: /* $E0, $F9-$FD: nothing (the table returns directly to C3F8) */
        return 0;
    }
    inc16(pce, 0xE8); /* the parameter */
    T(pce, 4);        /* JMP C3F8 */
    return 0;
}

/* ======== Slides (C85A): volume and pan ======== */

/* Bit 7 of the sum A + accumulator; returns the bit and stores the sum in $EA */
static uint8_t slide_step(Pce *pce, uint8_t rate, uint8_t acc)
{
    uint8_t s = (uint8_t)(rate + acc);
    zw(pce, 0xEA, s);
    return (uint8_t)(s >> 7);
}

static void slides(Pce *pce, const Eng *e, int x)
{
    uint8_t r = rv(pce, e, 0x23C0, x);
    T(pce, 12); /* C85A */
    if (r) {
        uint8_t v;
        T(pce, 2); /* C862 */
        if (!(r & 0x80)) {
            v = (uint8_t)(slide_step(pce, r, rv(pce, e, 0x23C6, x)) + rv(pce, e, 0x2312, x));
            T(pce, !v ? 28 : (v & 0x80) ? 30 : 39); /* C864, C874, C876 */
            if (v && !(v & 0x80)) {
                v = 0;
                wv(pce, e, 0x23C0, x, 0);
            }
        } else {
            uint8_t b = slide_step(pce, (uint8_t)(-r), rv(pce, e, 0x23C6, x));
            zw(pce, 0xEB, b);
            v = (uint8_t)(rv(pce, e, 0x2312, x) - b);
            T(pce, 2 + (!v ? 40 : v >= 0xE1 ? 44 : 49)); /* C87C, C893, C897 */
            if (v && v < 0xE1) {
                v = 0xE1;
                wv(pce, e, 0x23C0, x, 0);
            }
        }
        T(pce, 16); /* C89C */
        wv(pce, e, 0x2312, x, v);
        wv(pce, e, 0x23C6, x, (uint8_t)(z(pce, 0xEA) & 0x7F));
    } else {
        T(pce, 2);
    }
    r = rv(pce, e, 0x23CC, x);
    T(pce, 7); /* C8A6 */
    if (r) {
        uint8_t v;
        T(pce, 2);
        if (!(r & 0x80)) {
            uint8_t b = slide_step(pce, r, rv(pce, e, 0x23D2, x));
            zw(pce, 0xEB, b);
            v = (uint8_t)((rv(pce, e, 0x246E, x) & 0x0F) + b);
            T(pce, 38 + (v < 0x10 ? 2 : 11)); /* C8AD, C8C5 */
            if (v >= 0x10) {
                v = 0x0F;
                wv(pce, e, 0x23CC, x, 0);
            }
        } else {
            uint8_t b = slide_step(pce, (uint8_t)(-r), rv(pce, e, 0x23D2, x));
            zw(pce, 0xEB, b);
            v = (uint8_t)((rv(pce, e, 0x246E, x) & 0x0F) - b);
            T(pce, 2 + 40 + (v ? 2 : 7)); /* C8CC, C8E5 */
            if (!v)
                wv(pce, e, 0x23CC, x, 0);
        }
        T(pce, 31); /* C8E9 */
        zw(pce, 0xEB, v);
        wv(pce, e, 0x246E, x, (uint8_t)((rv(pce, e, 0x246E, x) & 0xF0) | v));
        wv(pce, e, 0x23D2, x, (uint8_t)(z(pce, 0xEA) & 0x7F));
    } else {
        T(pce, 2);
    }
    r = rv(pce, e, 0x23D8, x);
    T(pce, 7); /* C8FC */
    if (r) {
        uint8_t v;
        T(pce, 2);
        if (!(r & 0x80)) {
            uint8_t s = (uint8_t)(r + rv(pce, e, 0x23DE, x));
            zw(pce, 0xEA, s);
            uint8_t b = (uint8_t)((s & 0x80) >> 3);
            zw(pce, 0xEB, b);
            unsigned t = (unsigned)(rv(pce, e, 0x246E, x) & 0xF0) + b;
            v = (uint8_t)t;
            T(pce, 38 + (t > 0xFF ? 11 : 2)); /* C903, C91A */
            if (t > 0xFF) {
                v = 0xF0;
                wv(pce, e, 0x23D8, x, 0);
            }
        } else {
            uint8_t s = (uint8_t)((uint8_t)(-r) + rv(pce, e, 0x23DE, x));
            zw(pce, 0xEA, s);
            uint8_t b = (uint8_t)((s & 0x80) >> 3);
            zw(pce, 0xEB, b);
            v = (uint8_t)((rv(pce, e, 0x246E, x) & 0xF0) - b);
            T(pce, 2 + 42 + (v ? 2 : 7)); /* C921, C93B */
            if (!v)
                wv(pce, e, 0x23D8, x, 0);
        }
        T(pce, 31); /* C93F */
        zw(pce, 0xEB, v);
        wv(pce, e, 0x246E, x, (uint8_t)((rv(pce, e, 0x246E, x) & 0x0F) | v));
        wv(pce, e, 0x23DE, x, (uint8_t)(z(pce, 0xEA) & 0x7F));
    } else {
        T(pce, 2);
    }
    T(pce, 7); /* RTS */
}

/* ======== Drum mode (C953): the note's sound sequence ======== */

static void pattern(Pce *pce, const Eng *e, int y)
{
    T(pce, 16); /* C953 */
    if ((rv(pce, e, 0x2456, y) & 0xF0) != 0x10) {
        T(pce, 7); /* RTS */
        return;
    }
    T(pce, 2 + 7); /* C960 */
    if (!rv(pce, e, 0x2482, y)) {
        T(pce, 2 + 7);
        return;
    }
    T(pce, 18); /* C965 */
    zw(pce, 0xE8, rv(pce, e, 0x2402, y));
    zw(pce, 0xE9, rv(pce, e, 0x2408, y));
    for (;;) {
        uint8_t b = fetch(pce);
        T(pce, 6); /* C977 */
        if (b < 0xB0) {
            T(pce, 2 + (y >= 4 ? 4 + 11 : 6)); /* C991, C995 */
            if (y >= 4)
                wv(pce, e, 0x2476, y, (uint8_t)(b & 0x1F));
            break;
        }
        uint8_t hi = (uint8_t)(b & 0xF0);
        T(pce, 6); /* C97C */
        if (hi == 0xB0) {
            T(pce, 2 + 11); /* C99C */
            wv(pce, e, 0x2462, y, (uint8_t)(b - 0xB0));
            wv(pce, e, 0x245C, y, fetch(pce));
            T(pce, 9 + (y >= 4 ? 7 : 2)); /* C9AB, C9B2 */
            if (y >= 4)
                wv(pce, e, 0x2476, y, 0x80);
            break;
        }
        T(pce, 4); /* C982 */
        if (hi == 0xC0) {
            uint8_t n = ind(pce, 0xE8, 0);
            T(pce, 2 + 11); /* C9C2 */
            if (n < 0x10) {
                T(pce, 28); /* C9C8 */
                wv(pce, e, 0x233C, y, m(pce, (uint16_t)(0xD8A1 + n * 2)));
                wv(pce, e, 0x2342, y, m(pce, (uint16_t)(0xD8A2 + n * 2)));
            } else {
                T(pce, 2 + 40); /* C9D8 */
                game_envelope_entry(pce, n);
                wv(pce, e, 0x233C, y, ind(pce, 0xEA, 0));
                inc16(pce, 0xEA);
                T(pce, 16); /* C9F3 */
                wv(pce, e, 0x2342, y, ind(pce, 0xEA, 0));
            }
        } else if (hi == 0xD0) {
            T(pce, 4 + 2 + 16); /* C986, C9FA */
            wv(pce, e, 0x246E, y, ind(pce, 0xE8, 0));
        } else if (hi == 0xE0) {
            T(pce, 4 + 4 + 2 + 16); /* C986, C98A, CA01 */
            wv(pce, e, 0x2474, y, ind(pce, 0xE8, 0));
        } else {
            T(pce, 4 + 4 + 4 + 38); /* C986, C98A, C98E, CA08 (including the RTS) */
            wv(pce, e, 0x2482, y, 0);
            uint16_t p = (uint16_t)(zp16(pce, 0xE8) - 1);
            wv(pce, e, 0x2402, y, (uint8_t)p);
            wv(pce, e, 0x2408, y, (uint8_t)(p >> 8));
            return;
        }
        inc16(pce, 0xE8);
        T(pce, 4); /* JMP C96F */
    }
    T(pce, 25); /* C9B7, including the RTS */
    wv(pce, e, 0x2402, y, z(pce, 0xE8));
    wv(pce, e, 0x2408, y, z(pce, 0xE9));
}

/* ======== Volume envelope (CA25) ======== */

/* Level += rate (16 bits); past the limit: 0 (falling) or $7C (rising) */
static void env_add(Pce *pce, const Eng *e, int x, uint16_t rate_lo, uint16_t rate_hi)
{
    unsigned lo = (unsigned)rv(pce, e, 0x234E, x) + rv(pce, e, rate_lo, x);
    wv(pce, e, 0x234E, x, (uint8_t)lo);
    uint8_t hi = (uint8_t)(rv(pce, e, 0x2354, x) + rv(pce, e, rate_hi, x) + (lo > 0xFF));
    if (hi & 0x80) {
        T(pce, 29 + 14 + 2); /* CA47/CA73, CA59/CA85, CA62/CA8E */
        wv(pce, e, 0x234E, x, 0);
        hi = (rv(pce, e, rate_hi, x) & 0x80) ? 0 : 0x7C;
    } else {
        T(pce, 29 + 2);
    }
    wv(pce, e, 0x2354, x, hi);
}

static void env_out(Pce *pce, const Eng *e, int x)
{
    uint8_t v = (uint8_t)((rv(pce, e, 0x2354, x) >> 2) + rv(pce, e, 0x2312, x));
    if (v & 0x80)
        v = 0;
    wv(pce, e, 0x2468, x, v);
    T(pce, 20 + 5 + 7); /* CAA6, CAB1, CAB2, RTS */
}

/* Next envelope segment: $00-$FA steps (+ 16-bit rate), $FB skips, $FC level, $FD+ end */
static void env_next(Pce *pce, const Eng *e, int x)
{
    uint8_t y = rv(pce, e, 0x2348, x);
    T(pce, 23); /* CAB6 */
    zw(pce, 0xE8, rv(pce, e, 0x233C, x));
    zw(pce, 0xE9, rv(pce, e, 0x2342, x));
    for (;;) {
        uint8_t a = ind(pce, 0xE8, y);
        if (a == 0xFB) {
            T(pce, 15 + 10); /* CAC3, CAC9, CAE8 */
            y = (uint8_t)(y + 3);
            continue;
        }
        if (a == 0xFC) {
            T(pce, 19 + 41); /* CAC3, CAC9, CACB, CAED */
            y++;
            wv(pce, e, 0x234E, x, ind(pce, 0xE8, y));
            y++;
            wv(pce, e, 0x2354, x, ind(pce, 0xE8, y));
            y++;
            a = ind(pce, 0xE8, y);
            if (a >= 0xFB) {
                T(pce, 9 + 7); /* CB00, CAA2 */
                wv(pce, e, 0x2372, x, 0);
                wv(pce, e, 0x2348, x, y);
                env_out(pce, e, x);
                return;
            }
            T(pce, 2);
        } else if (a > 0xFC) {
            T(pce, 21 + 21); /* CAC3, CAC9, CACB, CACF, CB05 */
            wv(pce, e, 0x2372, x, 0xFF);
            wv(pce, e, 0x235A, x, 0);
            wv(pce, e, 0x2360, x, 0);
            env_out(pce, e, x);
            return;
        } else {
            T(pce, 13); /* CAC3 */
        }
        T(pce, 6 + 39 + 7); /* CAD1, CAD5, CAD6, CAA2 */
        wv(pce, e, 0x2372, x, a ? a : 0xFF);
        y++;
        wv(pce, e, 0x235A, x, ind(pce, 0xE8, y));
        y++;
        wv(pce, e, 0x2360, x, ind(pce, 0xE8, y));
        y++;
        wv(pce, e, 0x2348, x, y);
        env_out(pce, e, x);
        return;
    }
}

static void envelope(Pce *pce, const Eng *e, int x)
{
    uint8_t s = rv(pce, e, 0x2482, x);
    T(pce, 12); /* CA25 */
    if (!s) {
        T(pce, 4 + 9 + 7); /* CA2D, CA9D, RTS */
        wv(pce, e, 0x2468, x, 0);
        return;
    }
    T(pce, 2 + 2); /* CA30 */
    if (s & 0x80) { /* note released */
        T(pce, 2);
        env_add(pce, e, x, 0x2366, 0x236C);
        T(pce, 9); /* CA90 */
        env_out(pce, e, x);
        return;
    }
    T(pce, 4); /* CA32 */
    if (s == 0x02) { /* new note */
        T(pce, 2 + 16); /* CA95 */
        wv(pce, e, 0x2482, x, 0x01);
        wv(pce, e, 0x2348, x, 0);
        env_next(pce, e, x);
        return;
    }
    uint8_t g = (uint8_t)(rv(pce, e, 0x231E, x) - 1);
    wv(pce, e, 0x231E, x, g);
    T(pce, 9 + (g ? 2 : 7) + 7); /* CA36, CA3B, CA40 */
    if (!g)
        wv(pce, e, 0x2482, x, (uint8_t)(s | 0x80));
    uint8_t n = rv(pce, e, 0x2372, x);
    if (!n) {
        T(pce, 4); /* CA45 */
        env_next(pce, e, x);
        return;
    }
    T(pce, 2);
    env_add(pce, e, x, 0x235A, 0x2360);
    T(pce, 14); /* CA64 */
    if (rv(pce, e, 0x2372, x) != 0xFF) {
        T(pce, 11); /* CA6E */
        wv(pce, e, 0x2372, x, (uint8_t)(rv(pce, e, 0x2372, x) - 1));
    } else {
        T(pce, 2);
    }
    env_out(pce, e, x);
}

/* ======== Pitch (CB13): sweep, detune, modulation, pitch envelope ======== */

/* CBE5: $EB:$EA = $EA signed, shifted by the difference between $2396 and the octave */
static void shift_amount(Pce *pce, const Eng *e, int x)
{
    T(pce, (z(pce, 0xEA) & 0x80) ? 10 + 6 : 12); /* CBE5, CBEB */
    zw(pce, 0xEB, (z(pce, 0xEA) & 0x80) ? 0xFF : 0x00);
    uint8_t s = rv(pce, e, 0x2396, x);
    T(pce, 7); /* CBED */
    if (!s) {
        T(pce, 2 + 7);
        return;
    }
    int8_t d = (int8_t)(uint8_t)(s - rv(pce, e, 0x22FA, x));
    uint16_t v = zp16(pce, 0xEA);
    T(pce, 11); /* CBF2 */
    if (d >= 0) {
        T(pce, d * 20 + 4 + 7); /* loop CBF9/CBFB, RTS */
        v = d >= 16 ? 0 : (uint16_t)(v << d);
        zp16w(pce, 0xEA, v);
        return;
    }
    T(pce, 2 + -d * 20 + 4); /* loop CC02/CC04 */
    v = -d >= 16 ? 0 : (uint16_t)(v >> -d);
    zp16w(pce, 0xEA, v);
    T(pce, (z(pce, 0xEB) ? 6 + 6 : 8) + 7); /* CC0B, CC0F, RTS */
    if (z(pce, 0xEB))
        zw(pce, 0xEB, 0xFF);
}

/* CC14: output frequency += $EB:$EA */
static void add_freq(Pce *pce, const Eng *e, int x)
{
    unsigned lo = (unsigned)rv(pce, e, 0x245C, x) + z(pce, 0xEA);
    wv(pce, e, 0x245C, x, (uint8_t)lo);
    wv(pce, e, 0x2462, x, (uint8_t)(rv(pce, e, 0x2462, x) + z(pce, 0xEB) + (lo > 0xFF)));
    T(pce, 37);
}

static void pitch(Pce *pce, const Eng *e, int x)
{
    T(pce, 14); /* CB13 */
    if (rv(pce, e, 0x2456, x) & 0xF0) {
        T(pce, 7); /* RTS */
        return;
    }
    T(pce, 2 + 29); /* CB1E */
    wv(pce, e, 0x245C, x, rv(pce, e, 0x2300, x));
    wv(pce, e, 0x2462, x, rv(pce, e, 0x2306, x));
    uint8_t sweep = rv(pce, e, 0x23E4, x);
    if (sweep) {
        int apply_only = 0;
        T(pce, 7); /* CB30 */
        if (rv(pce, e, 0x23F6, x)) {
            uint8_t c = (uint8_t)(rv(pce, e, 0x23FC, x) - 1);
            wv(pce, e, 0x23FC, x, c);
            T(pce, 9); /* CB35 */
            if (!c) {
                wv(pce, e, 0x23FC, x, 1);
                apply_only = 1;
            } else {
                T(pce, 2);
            }
        } else {
            T(pce, 2);
        }
        if (apply_only) {
            T(pce, 32); /* CB3A */
            zw(pce, 0xEA, rv(pce, e, 0x23EA, x));
            zw(pce, 0xEB, rv(pce, e, 0x23F0, x));
        } else {
            T(pce, 11); /* CB4C */
            zw(pce, 0xEA, sweep);
            shift_amount(pce, e, x);
            T(pce, 45); /* CB51 */
            unsigned lo = (unsigned)rv(pce, e, 0x23EA, x) + z(pce, 0xEA);
            wv(pce, e, 0x23EA, x, (uint8_t)lo);
            zw(pce, 0xEA, (uint8_t)lo);
            uint8_t hi = (uint8_t)(rv(pce, e, 0x23F0, x) + z(pce, 0xEB) + (lo > 0xFF));
            wv(pce, e, 0x23F0, x, hi);
            zw(pce, 0xEB, hi);
        }
        add_freq(pce, e, x);
        if (apply_only)
            T(pce, 4); /* CB4A */
    } else {
        T(pce, 2);
    }
    uint8_t det = rv(pce, e, 0x23BA, x);
    T(pce, 7); /* CB69 */
    if (det) {
        T(pce, 11); /* CB6E */
        zw(pce, 0xEA, det);
        shift_amount(pce, e, x);
        T(pce, 7); /* CB73 */
        add_freq(pce, e, x);
    } else {
        T(pce, 2);
    }
    T(pce, 7); /* CB76 */
    if (rv(pce, e, 0x23AE, x)) {
        uint8_t c = (uint8_t)(rv(pce, e, 0x23B4, x) - 1);
        wv(pce, e, 0x23B4, x, c);
        T(pce, 9); /* CB7B */
        if (!c) {
            wv(pce, e, 0x23B4, x, 1);
            zw(pce, 0xE8, rv(pce, e, 0x239C, x));
            zw(pce, 0xE9, rv(pce, e, 0x23A2, x));
            uint8_t y = rv(pce, e, 0x23A8, x);
            uint8_t a = ind(pce, 0xE8, y);
            int skip = 0;
            T(pce, 41); /* CB80 */
            if (a == 0x80) {
                T(pce, 4); /* CB96 */
                if (y == 0) {
                    skip = 1;
                    T(pce, 2);
                } else {
                    T(pce, 9); /* CB9A */
                    a = ind(pce, 0xE8, --y);
                }
            } else {
                T(pce, 2);
            }
            if (!skip) {
                T(pce, 14); /* CB9D */
                zw(pce, 0xEA, a);
                shift_amount(pce, e, x);
                T(pce, 7); /* CBA3 */
                add_freq(pce, e, x);
                T(pce, 10 + 7); /* CBA6, CBAA, CBAB */
                y++;
                if (!y)
                    y--;
                wv(pce, e, 0x23A8, x, y);
            }
        } else {
            T(pce, 2);
        }
    } else {
        T(pce, 2);
    }
    T(pce, 7); /* CBAF */
    if (rv(pce, e, 0x238A, x)) {
        uint8_t c = (uint8_t)(rv(pce, e, 0x2390, x) - 1);
        wv(pce, e, 0x2390, x, c);
        T(pce, 9); /* CBB4 */
        if (!c) {
            T(pce, 30); /* CBB9 */
            wv(pce, e, 0x2390, x, 1);
            zw(pce, 0xE8, rv(pce, e, 0x2378, x));
            zw(pce, 0xE9, rv(pce, e, 0x237E, x));
            uint8_t y = rv(pce, e, 0x2384, x);
            for (;;) {
                uint8_t a = ind(pce, 0xE8, y);
                if (a != 0x80) {
                    T(pce, 11 + 2 + 11); /* CBC9, CBD9 */
                    zw(pce, 0xEA, a);
                    shift_amount(pce, e, x);
                    T(pce, 7); /* CBDE */
                    add_freq(pce, e, x);
                    T(pce, 7); /* CBE1 */
                    wv(pce, e, 0x2384, x, (uint8_t)(rv(pce, e, 0x2384, x) + 1));
                    break;
                }
                if (y == 0) {
                    T(pce, 11 + 4 + 2); /* CBC9, CBCF */
                    break;
                }
                T(pce, 11 + 4 + 11); /* CBC9, CBCF, CBD3 */
                wv(pce, e, 0x2384, x, 0);
                y = 0;
            }
        } else {
            T(pce, 2);
        }
    } else {
        T(pce, 2);
    }
    T(pce, 7); /* RTS */
}

/* ======== Fade (CC26) ======== */

static void fade(Pce *pce, const Eng *e)
{
    uint8_t rate = rv(pce, e, 0x2488, 0);
    T(pce, 7); /* CC26 */
    if (!rate) {
        T(pce, 2 + 7);
        return;
    }
    uint8_t a = (uint8_t)(rate + rv(pce, e, 0x2489, 0));
    T(pce, 9 + ((a & 0x80) ? 9 : 2) + 14); /* CC2B, CC31, CC36 */
    if (a & 0x80) {
        wv(pce, e, 0x248A, 0, (uint8_t)(rv(pce, e, 0x248A, 0) + 1));
        a &= 0x7F;
    }
    wv(pce, e, 0x2489, 0, a);
    if (rv(pce, e, 0x248A, 0) >= 0x1E) {
        T(pce, 2 + 7); /* CC40, RTS */
        for (int x = 5; x >= 0; x--) {
            int on = rv(pce, e, 0x247C, x) != 0;
            T(pce, (on ? 14 : 9) + 4 + (x ? 2 : 0)); /* CC42, CC47, CC4C */
            if (on)
                wv(pce, e, 0x247C, x, 0x02);
        }
    } else {
        T(pce, 2 + 7);
    }
}

/* ======== Status (C151 / C16C): playing channels, or $80 if the program is off ======== */

static uint8_t status(Pce *pce, const Eng *e)
{
    int head = e->sfx ? 9 : 11; /* C16C / C151 */
    if (m(pce, 0x22DF) == (e->sfx ? 0x00 : 0x01)) {
        T(pce, head + 2 + 7);
        return 0x80;
    }
    T(pce, head + 4 + 7); /* C15A, RTS */
    uint8_t a = 0;
    for (int x = 5; x >= 0; x--) {
        int on = rv(pce, e, 0x247C, x) != 0;
        T(pce, 7 + (on ? 6 : 8) + 4 + (x ? 2 : 0)); /* C15D, C166 or C162, C168 */
        a = (uint8_t)((a << 1) | on);
    }
    return a;
}

/* ======== The step, in pieces ======== */

/* Each piece: RAM accesses with no separate time (the timing is the BIOS's, summed block by block with T) */
static const Eng *eng_of(Pce *pce)
{
    hle_ram_free(pce);
    pce->hle->psg_t = 0;
    return pce->cpu.pc >= k_sfx.pt ? &k_sfx : &k_music;
}

/* C32D/CD42: JMP to the status routine (C151/C16C), which returns to the step with the RTS */
static void eng_return(Pce *pce, const Eng *e)
{
    T(pce, 4);
    A = alu_nz(pce, status(pce, e));
    hle_cycles(pce, pce->hle->psg_t);
    hle_rts(pce);
}

static void p_start(Pce *pce)
{
    const Eng *e = eng_of(pce);
    T(pce, 7); /* JSR C330 */
    track_start(pce, e);
    uint8_t de = m(pce, 0x22DE);
    if (e->sfx) {
        if (de & 0x40) {
            T(pce, 10 + 2); /* CD15 */
            eng_return(pce, e);
            return;
        }
        T(pce, 10 + 7); /* CD15, CD1B */
    } else {
        if (de & 0x80) {
            T(pce, 7 + 2); /* C2F2 */
            eng_return(pce, e);
            return;
        }
        mw(pce, 0x22DE, (uint8_t)(de + 1));
        if ((de & 0x0F) < m(pce, 0x22DD)) { /* tempo divider */
            T(pce, 7 + 16 + 2); /* C2F2, C2F7 */
            eng_return(pce, e);
            return;
        }
        mw(pce, 0x22DE, (uint8_t)(m(pce, 0x22DE) & ~0x0F));
        T(pce, 7 + 16 + 16); /* C2F2, C2F7, C301 */
    }
    mw(pce, 0x22E0, 0x05);
    piece_end(pce, (uint16_t)(e->pt + P_CH));
}

static void p_ch(Pce *pce)
{
    const Eng *e = eng_of(pce);
    int x = cur(pce);
    T(pce, 12); /* C30B */
    if (!rv(pce, e, 0x247C, x)) {
        T(pce, 2);
        piece_end(pce, (uint16_t)(e->pt + P_NEXT));
        return;
    }
    T(pce, 7 + 9); /* JSR C3E7, C3E7 */
    uint8_t d = (uint8_t)(rv(pce, e, 0x230C, x) - 1);
    wv(pce, e, 0x230C, x, d);
    if (d) {
        T(pce, 7 + 7); /* RTS, JSR C85A */
        piece_end(pce, (uint16_t)(e->pt + P_SLIDES));
        return;
    }
    T(pce, 2 + 21); /* C3ED */
    zw(pce, 0xE8, rv(pce, e, 0x22EE, x));
    zw(pce, 0xE9, rv(pce, e, 0x22F4, x));
    piece_end(pce, (uint16_t)(e->pt + P_SEQ));
}

/* One sequence byte per piece (C3F8) */
static void p_seq(Pce *pce)
{
    const Eng *e = eng_of(pce);
    int ch = cur(pce);
    uint8_t b = fetch(pce);
    T(pce, 7); /* C400 */
    if (b < 0xD0) {
        T(pce, 2);
        note(pce, e, ch, b);
        T(pce, 7); /* JSR C85A */
        piece_end(pce, (uint16_t)(e->pt + P_SLIDES));
        return;
    }
    T(pce, 19); /* C405: jump through the table */
    int stop = command(pce, e, ch, b);
    if (stop)
        T(pce, 7); /* JSR C85A */
    piece_end(pce, (uint16_t)(e->pt + (stop ? P_SLIDES : P_SEQ)));
}

static void p_slides(Pce *pce)
{
    const Eng *e = eng_of(pce);
    slides(pce, e, cur(pce));
    T(pce, 7); /* JSR C953 */
    piece_end(pce, (uint16_t)(e->pt + P_PATTERN));
}

static void p_pattern(Pce *pce)
{
    const Eng *e = eng_of(pce);
    pattern(pce, e, cur(pce));
    T(pce, 7); /* JSR CA25 */
    piece_end(pce, (uint16_t)(e->pt + P_ENV));
}

static void p_env(Pce *pce)
{
    const Eng *e = eng_of(pce);
    envelope(pce, e, cur(pce));
    T(pce, 7); /* JSR CB13 */
    piece_end(pce, (uint16_t)(e->pt + P_PITCH));
}

static void p_pitch(Pce *pce)
{
    const Eng *e = eng_of(pce);
    pitch(pce, e, cur(pce));
    piece_end(pce, (uint16_t)(e->pt + P_NEXT));
}

static void p_next(Pce *pce)
{
    const Eng *e = eng_of(pce);
    uint8_t c = (uint8_t)(m(pce, 0x22E0) - 1);
    mw(pce, 0x22E0, c);
    T(pce, 9); /* C322 */
    if (!(c & 0x80)) {
        T(pce, 2);
        piece_end(pce, (uint16_t)(e->pt + P_CH));
        return;
    }
    T(pce, 7); /* JSR CC26 */
    fade(pce, e);
    T(pce, 7 + 2); /* JSR CC50, LDX #5 */
    X = 5;
    piece_end(pce, (uint16_t)(e->pt + P_OUT));
}

/* Output of a channel (X) to the PSG (CC52). The waveform, if it changed, goes in pieces (P_WAVE). */
static void out_rest(Pce *pce, const Eng *e);

/* CCFD: next channel, or return (RTS and the status) */
static void out_next(Pce *pce, const Eng *e)
{
    X--;
    if (X & 0x80) {
        T(pce, 6 + 7); /* CCFD, RTS */
        eng_return(pce, e);
        return;
    }
    T(pce, 4 + 4); /* CCFD, CD00 */
    piece_end(pce, (uint16_t)(e->pt + P_OUT));
}

static void p_out(Pce *pce)
{
    const Eng *e = eng_of(pce);
    int x = X;
    uint8_t s = rv(pce, e, 0x247C, x);
    T(pce, 9); /* CC52 */
    if ((int8_t)(uint8_t)(s - 1) < 0) { /* stopped ($00) or paused ($FF...) */
        T(pce, 4); /* CC59 */
        out_next(pce, e);
        return;
    }
    T(pce, 2 + 7); /* CC5C */
    mw(pce, 0x0800, (uint8_t)x);
    if (s != 0x01) { /* finished: turn the channel off */
        mw(pce, 0x0804, 0);
        wv(pce, e, 0x247C, x, 0);
        if (!e->sfx) {
            T(pce, 14); /* CC61 */
        } else if (m(pce, 0x22DF) == 0x01) {
            T(pce, 19 + 2 + 4); /* D65E, D678 */
        } else if (!m(pce, (uint16_t)(0x247C + x))) {
            T(pce, 19 + 7 + 2 + 4); /* D65E, D66B, D678 */
        } else {
            T(pce, 19 + 7 + 12 + 4); /* D65E, D66B, D670, D678 */
            mw(pce, (uint16_t)(0x2474 + x), (uint8_t)(m(pce, (uint16_t)(0x2474 + x)) & 0x7F));
        }
        out_next(pce, e);
        return;
    }
    T(pce, 2);
    if (!e->sfx) {
        T(pce, 7); /* CC6A */
        if (m(pce, 0x22DF)) {
            T(pce, 9); /* CC6F */
            if (m(pce, (uint16_t)(0x2607 + x)) == 0x01) { /* the effect owns the channel */
                T(pce, 4); /* CC76 */
                out_next(pce, e);
                return;
            }
        }
        T(pce, 2);
    }
    uint8_t w = rv(pce, e, 0x2474, x);
    T(pce, 7); /* CC79 */
    if (!(w & 0x80)) {
        wv(pce, e, 0x2474, x, (uint8_t)(w | 0x80));
        w &= 0x7F;
        uint16_t base;
        if (w < 0x2D) { /* factory waveform */
            base = 0xD9A6;
            T(pce, 13 + 7 + 39 + 26); /* CC7E, CC89, CD04, CC8C */
        } else {
            w = (uint8_t)(w - 0x2D);
            base = (uint16_t)(m(pce, 0x22D2) | (m(pce, 0x22D3) << 8));
            T(pce, 15 + 11 + 39 + 28); /* CC7E, CC9B, CD04, CCA1 */
        }
        zp16w(pce, 0xE8, (uint16_t)(base + w * 32));
        mw(pce, 0x0804, 0x40);
        mw(pce, 0x0804, 0x00);
        Y = 0;
        T(pce, 14); /* CCB0 */
        piece_end(pce, (uint16_t)(e->pt + P_WAVE));
        return;
    }
    T(pce, 2);
    out_rest(pce, e);
}

/* CCB9: 8 bytes of the waveform per piece */
static void p_wave(Pce *pce)
{
    const Eng *e = eng_of(pce);
    for (int i = 0; i < 8; i++, Y++)
        mw(pce, 0x0806, ind(pce, 0xE8, Y));
    if (Y < 0x20) {
        T(pce, 8 * 20);
        piece_end(pce, (uint16_t)(e->pt + P_WAVE));
        return;
    }
    T(pce, 7 * 20 + 18);
    out_rest(pce, e);
}

/* CCC3 */
static void out_rest(Pce *pce, const Eng *e)
{
    int x = X;
    uint8_t noise = rv(pce, e, 0x2476, x);
    T(pce, 4); /* CCC3 */
    if (x >= 4 && !(noise & 0x80)) {
        T(pce, 7 + 11); /* CCC7, CCCC */
        mw(pce, 0x0807, (uint8_t)(noise | 0x80));
    } else {
        T(pce, (x >= 4 ? 7 + 2 : 2) + 25); /* CCC7, CCD3 */
        mw(pce, 0x0807, 0);
        mw(pce, 0x0802, rv(pce, e, 0x245C, x));
        mw(pce, 0x0803, rv(pce, e, 0x2462, x));
    }
    mw(pce, 0x0805, rv(pce, e, 0x246E, x));
    uint8_t v = (uint8_t)(rv(pce, e, 0x2468, x) - rv(pce, e, 0x248A, 0));
    T(pce, 35); /* CCE2, CCF1, CCF3 or CCF8: the same timing on all three paths */
    if (v == 0 || (v & 0x80))
        mw(pce, 0x0804, 0);
    else
        mw(pce, 0x0804, (uint8_t)(v | 0x80));
    out_next(pce, e);
}

/* ======== E6E8: the driver step ======== */

#define TICK 0xE6E8

static void psg_tick(Pce *pce)
{
    hle_ram_free(pce);
    hle_push(pce, pce->cpu.mpr[4]);
    hle_push(pce, pce->cpu.mpr[5]);
    hle_push(pce, pce->cpu.mpr[6]);
    tam(pce, 4, m(pce, 0x22EB));
    tam(pce, 5, m(pce, 0x22EC));
    tam(pce, 6, 0x02);
    if (m(pce, 0x22DF)) {
        hle_cycles(pce, 62);
        bios_call(pce, (uint16_t)(k_sfx.pt + P_START), 0xE707);
        return;
    }
    hle_cycles(pce, 57);
    go(pce, 0xE713);
}

static void psg_tick_sfx_done(Pce *pce)
{
    hle_ram_free(pce);
    if (A == 0)
        mw(pce, 0x22DE, (uint8_t)(m(pce, 0x22DE) | 0x40));
    hle_cycles(pce, A == 0 ? 16 : 6);
    go(pce, 0xE713);
}

static void psg_tick_music(Pce *pce)
{
    hle_ram_free(pce);
    A = m(pce, 0x22DF);
    alu_cmp(pce, A, 0x01);
    if (A == 0x01) {
        hle_cycles(pce, 11);
        go(pce, 0xE729);
        return;
    }
    hle_cycles(pce, 16);
    bios_call(pce, (uint16_t)(k_music.pt + P_START), 0xE71D);
}

static void psg_tick_music_done(Pce *pce)
{
    hle_ram_free(pce);
    if (A == 0)
        mw(pce, 0x22DE, (uint8_t)(m(pce, 0x22DE) | 0x80));
    hle_cycles(pce, A == 0 ? 16 : 6);
    go(pce, 0xE729);
}

static void psg_tick_end(Pce *pce)
{
    hle_ram_free(pce);
    int cycles = 8 + 2 + 44;
    if (!(z(pce, 0xE7) & 0x01)) {
        uint8_t t = m(pce, 0x22D4);
        cycles += 7;
        if (!(t & 0x80)) { /* new tempo: reload the timer */
            mw(pce, 0x0C00, t);
            mw(pce, 0x22D4, (uint8_t)(t | 0x80));
            cycles += 10;
        }
    }
    tam(pce, 6, hle_pull(pce));
    tam(pce, 5, hle_pull(pce));
    tam(pce, 4, hle_pull(pce));
    A = alu_nz(pce, m(pce, 0x22ED));
    mw(pce, 0x0800, A);
    hle_cycles(pce, cycles);
    hle_rts(pce);
}

/* ======== psg_bios ($E0D8): driver functions, number in $20FF (A = $20F8 on entry) ======== */

/* The psg_bios functions below count the time `t` (CPU cycles since the start of the function, including the RTS)
 * and wait until the BIOS cycle before each hardware write (the access lands at the start of the instruction) */
static void at(Pce *pce, int t)
{
    if (t > pce->cpu.instr_cycles)
        hle_idle(pce, t - pce->cpu.instr_cycles);
}

/* C221: stops all channels of the program */
static int all_stop(Pce *pce, int t)
{
    zw(pce, 0xE7, (uint8_t)(z(pce, 0xE7) | 0x80));
    mw(pce, 0x22DE, 0xC0);
    uint8_t mode = m(pce, 0x22DF);
    t += 4 + 2 + 4 + 2 + 5 + 5 + 2;
    for (int x = 5; x >= 0; x--) {
        if (mode != 0x01)
            mw(pce, (uint16_t)(0x247C + x), 0);
        if (mode >= 0x01)
            mw(pce, (uint16_t)(0x2607 + x), 0);
        t += mode == 0x01 ? 2 + 4 + 5 : mode == 0 ? 2 + 2 + 5 + 4 : 2 + 2 + 5 + 2 + 5; /* C231-C23A */
        mw(pce, 0x22ED, (uint8_t)x);
        t += 5;
        at(pce, t);
        mw(pce, 0x0800, (uint8_t)x);
        mw(pce, 0x0804, 0);
        t += 5 + 5 + 2 + (x ? 4 : 2);
    }
    return t + 7;
}

/* C019 */
static int psg_off(Pce *pce, int t)
{
    t += 4;
    if (!(z(pce, 0xE7) & 0x80))
        t = all_stop(pce, t + 2 + 7);
    else
        t += 4;
    A = (uint8_t)(z(pce, 0xE7) & 0x01);
    t += 4 + 2;
    if (!A) {
        at(pce, t + 2);
        mw(pce, 0x0C01, 0);
        t += 2 + 5;
    } else {
        t += 4;
    }
    return t + 7;
}

/* C000 */
static int psg_on(Pce *pce)
{
    int t = 4;
    if (!(z(pce, 0xE7) & 0x80))
        t = psg_off(pce, t + 2 + 7);
    else
        t += 4;
    A = (uint8_t)((z(pce, 0xF8) & 0x01) | 0x80);
    zw(pce, 0xE7, A);
    A &= 0x01;
    t += 4 + 2 + 2 + 4 + 2;
    if (!A) {
        A = 0x01;
        at(pce, t + 2 + 2);
        mw(pce, 0x0C01, A); /* plays from the timer: enable the timer */
        t += 2 + 2 + 5;
    } else {
        t += 4;
    }
    return t + 7;
}

/* C128: tempo (index >= $23 in the $D7A1 table) */
static void set_tempo(Pce *pce, uint8_t t)
{
    if (t < 0x23)
        t = 0x23;
    X = t;
    A = m(pce, (uint16_t)(0xD7A1 + t));
    mw(pce, 0x22D4, A);
}

/* psg_init in pieces (private points $FEB0-$FEB5): each BIOS block copy is a single instruction
 * (no interrupt comes in the middle), but between them one can. Y = mode, X = channel. */
#define P_INIT 0xFEB0

static void psg_bios_end(Pce *pce, uint8_t a, int carry, int overflow, int cycles);

static void init_channel(Pce *pce)
{
    hle_ram_free(pce);
    int x = X;
    mw(pce, 0x22ED, (uint8_t)x);
    mw(pce, 0x0800, (uint8_t)x);
    mw(pce, 0x0804, 0x40);
    mw(pce, 0x0804, 0x00);
    for (int i = 0; i < 32; i++) /* default waveform (factory waveform 0), TIN */
        mw(pce, 0x0806, m(pce, (uint16_t)(0xD9A6 + i)));
    X = alu_nz(pce, (uint8_t)(X - 1));
    hle_cycles(pce, 5 + 5 + 5 + 5 + 17 + 6 * 32 + 2 + 4);
    go(pce, (X & 0x80) ? P_INIT + 1 : P_INIT);
}

static void init_clear_globals(Pce *pce)
{
    hle_ram_free(pce);
    mw(pce, 0x0801, 0xFF);
    mw(pce, 0x0808, 0x01);
    mw(pce, 0x0809, 0x00);
    for (int i = 0; i < 0x1E; i++)
        mw(pce, (uint16_t)(0x22D0 + i), 0);
    hle_cycles(pce, 7 + 7 + 5 + 17 + 6 * 0x1E);
    go(pce, P_INIT + 2);
}

static void init_routine(Pce *pce)
{
    hle_ram_free(pce);
    /* $22E3: mini-routine "TIN $0000,$0806,$0020; RTS" (waveform copy) */
    static const uint8_t tin[8] = {0xD3, 0x00, 0x00, 0x06, 0x08, 0x20, 0x00, 0x60};
    for (int i = 0; i < 8; i++)
        mw(pce, (uint16_t)(0x22E3 + i), tin[i]);
    mw(pce, 0x22DF, Y);
    hle_cycles(pce, 17 + 6 * 8 + 5 + 2 + 2);
    go(pce, Y != 0x01 ? P_INIT + 3 : P_INIT + 4);
}

static void init_clear_music(Pce *pce)
{
    hle_ram_free(pce);
    for (int i = 0; i < 0x19D; i++)
        mw(pce, (uint16_t)(0x22EE + i), 0);
    hle_cycles(pce, 17 + 6 * 0x19D + 2);
    go(pce, Y >= 0x01 ? P_INIT + 4 : P_INIT + 5);
}

static void init_clear_sfx(Pce *pce)
{
    hle_ram_free(pce);
    for (int i = 0; i < 0x18B; i++)
        mw(pce, (uint16_t)(0x248B + i), 0);
    hle_cycles(pce, 17 + 6 * 0x18B);
    go(pce, P_INIT + 5);
}

static void init_finish(Pce *pce)
{
    hle_ram_free(pce);
    uint8_t mode = Y;
    zw(pce, 0xE8, 0);
    zw(pce, 0xE9, 0);
    zw(pce, 0xEA, 0);
    zw(pce, 0xEB, 0);
    zw(pce, 0xE6, 0);
    zw(pce, 0xE7, (uint8_t)(z(pce, 0xE7) | 0x80));
    mw(pce, 0x22E1, 0x80);
    mw(pce, 0x22DE, 0xC0);
    static const uint8_t divider[6] = {0, 0, 0, 1, 3, 4};
    static const uint8_t tempo[6] = {0x26, 0x26, 0x26, 0x4D, 0x9B, 0xC3};
    mw(pce, 0x22DD, divider[mode]);
    set_tempo(pce, tempo[mode]);
    psg_bios_end(pce, 0x02, 1, -1, 90);
}

static void psg_mstop(Pce *pce)
{
    if (m(pce, 0x22DF) == 0x01)
        return;
    uint8_t y = A;
    if (y & 0x80)
        mw(pce, 0x22DE, (uint8_t)(m(pce, 0x22DE) | 0x80));
    for (int x = 0; x < 6; x++) {
        if (m(pce, (uint16_t)(0x247C + x))) {
            int off = 1;
            if (!(y & 0x80)) {
                off = y & 0x01;
                if (off)
                    mw(pce, (uint16_t)(0x247C + x), 0);
            }
            if (off && !(m(pce, 0x22DF) && m(pce, (uint16_t)(0x2607 + x)) == 0x01)) {
                mw(pce, 0x22ED, (uint8_t)x);
                mw(pce, 0x0800, (uint8_t)x);
                mw(pce, 0x0804, 0);
            }
        }
        uint8_t a = (uint8_t)(y >> 1);
        if (a & 0x40)
            a |= 0x80;
        y = a;
    }
    A = y;
    X = 6;
    Y = y;
}

static void psg_sstop(Pce *pce)
{
    if (!m(pce, 0x22DF))
        return;
    uint8_t y = A;
    if (y & 0x80)
        mw(pce, 0x22DE, (uint8_t)(m(pce, 0x22DE) | 0x40));
    for (int x = 0; x < 6; x++) {
        if (m(pce, (uint16_t)(0x2607 + x))) {
            int off = 1;
            if (y & 0x80) {
                mw(pce, (uint16_t)(0x2607 + x), 0xFF);
            } else {
                off = y & 0x01;
                if (off)
                    mw(pce, (uint16_t)(0x2607 + x), 0);
            }
            if (off) {
                int music = m(pce, 0x22DF) != 0x01 && m(pce, (uint16_t)(0x247C + x));
                if (music)
                    mw(pce, (uint16_t)(0x2474 + x), (uint8_t)(m(pce, (uint16_t)(0x2474 + x)) & 0x7F));
                if (!music || (m(pce, 0x22DE) & 0x80)) {
                    mw(pce, 0x22ED, (uint8_t)x);
                    mw(pce, 0x0800, (uint8_t)x);
                    mw(pce, 0x0804, 0);
                }
            }
        }
        uint8_t a = (uint8_t)(y >> 1);
        if (a & 0x40)
            a |= 0x80;
        y = a;
    }
    A = y;
    X = 6;
    Y = y;
}

static void psg_mvoff(Pce *pce)
{
    if (m(pce, 0x22DF) == 0x01)
        return;
    uint8_t a = A;
    for (int x = 0; x < 6; x++) {
        int bit = a & 0x01;
        a >>= 1;
        if (!bit || !m(pce, (uint16_t)(0x247C + x)))
            continue;
        mw(pce, (uint16_t)(0x247C + x), 0xFF);
        if (m(pce, 0x22DF) && m(pce, (uint16_t)(0x2607 + x)) == 0x01)
            continue;
        mw(pce, 0x22ED, (uint8_t)x);
        mw(pce, 0x0800, (uint8_t)x);
        mw(pce, 0x0804, 0);
    }
    A = a;
}

static void psg_cont(Pce *pce)
{
    uint8_t y = A;
    if (m(pce, 0x22DF) != 0x01 && y != 0x01) {
        for (int x = 5; x >= 0; x--)
            if (m(pce, (uint16_t)(0x247C + x)))
                mw(pce, (uint16_t)(0x247C + x), 0x01);
        mw(pce, 0x22DE, (uint8_t)(m(pce, 0x22DE) & 0x7F));
    }
    if (m(pce, 0x22DF) && y) {
        for (int x = 5; x >= 0; x--) {
            if (m(pce, (uint16_t)(0x2607 + x))) {
                mw(pce, (uint16_t)(0x2607 + x), 0x01);
                mw(pce, (uint16_t)(0x25FF + x), (uint8_t)(m(pce, (uint16_t)(0x25FF + x)) & 0x7F));
            }
        }
        mw(pce, 0x22DE, (uint8_t)(m(pce, 0x22DE) & 0xBF));
    }
    A = (uint8_t)(z(pce, 0xE7) & 0x7F);
    zw(pce, 0xE7, A);
    Y = y;
}

/* C2CD (timing including the RTS) */
static int psg_fdout(Pce *pce)
{
    if (m(pce, 0x22DF) == 0x01)
        return 5 + 2 + 4 + 7;
    if (!(A & 0x7F))
        return 5 + 2 + 2 + 2 + 4 + 7;
    mw(pce, 0x2489, 0);
    mw(pce, 0x248A, 0);
    int t = 5 + 2 + 2 + 2 + 2 + 5 + 5 + 2;
    if (A & 0x80) {
        A = (uint8_t)(-A);
        t += 2 + 2 + 2;
    } else {
        t += 4;
    }
    mw(pce, 0x2488, A);
    return t + 5 + 7;
}

/* psg_bios in three pieces, like the BIOS instructions: entry (pushes, bank 2 in window 6), the
 * function ($FEB8) and the return ($FEB9). The timer interrupt can come in between them. */
#define P_DISPATCH 0xFEB8
#define P_RETURN 0xFEB9

static void psg_bios(Pce *pce)
{
    hle_ram_free(pce);
    hle_push(pce, X);
    hle_push(pce, Y);
    hle_push(pce, pce->cpu.mpr[6]);
    tam(pce, 6, 0x02);
    hle_cycles(pce, 4 + 3 + 3 + 4 + 3 + 2 + 5 + 4 + 2 + 2 + 2 + 2 + 4 + 7 + 7);
    go(pce, P_DISPATCH);
}

static void psg_dispatch(Pce *pce)
{
    hle_ram_free(pce);
    uint8_t fn = z(pce, 0xFF);
    int cycles = 10;
    /* On return, the carry (and sometimes V) is that of each function's last comparison, as in the BIOS */
    int carry = fn >= 0x15, overflow = -1;
    uint8_t mode = m(pce, 0x22DF);
    A = fn;
    if (fn < 0x15) {
        A = z(pce, 0xF8);
        int running = !(z(pce, 0xE7) & 0x80); /* psg_on/psg_off stop everything first */
        switch (fn) {
        /* (cycles = function timing including the RTS - 7: the end adds the 7) */
        case 0x00: cycles = psg_on(pce) - 7; carry = running && mode >= 1; break;
        case 0x01: cycles = psg_off(pce, 0) - 7; carry = running && mode >= 1; break;
        case 0x02: /* continues in the psg_init pieces */
            Y = A >= 6 ? 0x02 : A;
            X = 5;
            hle_cycles(pce, cycles + 10);
            go(pce, P_INIT);
            return;
        case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: {
            /* addresses of the game's tables: banks, songs, waveforms, envelopes, pitch, modulation, drums */
            static const uint16_t where[7] = {0x22EB, 0x22D0, 0x22D2, 0x22D5, 0x22D7, 0x22D9, 0x22DB};
            mw(pce, where[fn - 3], z(pce, 0xF8));
            A = z(pce, 0xF9);
            mw(pce, (uint16_t)(where[fn - 3] + 1), A);
            cycles += 25 - 17; /* C0DB-C11D, including the RTS (the 17 is this function's: 10 + 7) */
            break;
        }
        case 0x0A:
            carry = A >= 0x23;
            set_tempo(pce, A);
            A = 0x02; /* the BIOS returns with the bank it pushed */
            cycles += 22 + 2 + 28 - 17; /* C128, C135, C137 */
            break;
        case 0x0B: /* play: $20F8 = number, $20F9 = initial waveform (with bit 6 in the header) */
            mw(pce, 0x22E1, A);
            mw(pce, 0x22E2, z(pce, 0xF9));
            A = (uint8_t)(z(pce, 0xE7) & 0x7F);
            zw(pce, 0xE7, A);
            cycles += 31 - 17; /* C142 */
            break;
        case 0x0C: A = status(pce, &k_music); carry = mode == 0x01; cycles += 110; break;
        case 0x0D: A = status(pce, &k_sfx); cycles += 110; break;
        case 0x0E:
            psg_mstop(pce);
            carry = 1;
            if (mode != 0x01)
                overflow = 1; /* BIT #$40 in the loop */
            cycles += 250;
            break;
        case 0x0F:
            psg_sstop(pce);
            if (mode) {
                carry = 1;
                overflow = 1;
            }
            cycles += 300;
            break;
        case 0x10: cycles = all_stop(pce, 0) - 7; A = 0xC0; carry = mode >= 1; break;
        case 0x11: psg_mvoff(pce); carry = 1; cycles += 200; break;
        case 0x12: carry = mode == 0x01 || A >= 0x01; psg_cont(pce); cycles += 200; break;
        case 0x13:
            if (mode == 0x01) {
                carry = 1;
            } else {
                overflow = 1; /* BIT #$7F */
                carry = (A & 0x7F) ? 1 : mode >= 2;
            }
            cycles = psg_fdout(pce) - 7;
            break;
        case 0x14: A = (uint8_t)(A & 0x07); mw(pce, 0x22DD, A); cycles += 14; break;
        }
    }
    psg_bios_end(pce, A, carry, overflow, cycles);
}

/* End of the function: A, carry and V of the result (the return doesn't touch them) */
static void psg_bios_end(Pce *pce, uint8_t r, int carry, int overflow, int cycles)
{
    A = r;
    alu_setc(pce, carry);
    if (overflow >= 0)
        pce->cpu.p = (uint8_t)(overflow ? (pce->cpu.p | HU_FLAG_V) : (pce->cpu.p & ~HU_FLAG_V));
    hle_cycles(pce, cycles + 7);
    go(pce, P_RETURN);
}

/* Return: restores window 6, Y and X (the N and Z flags are those of X) */
static void psg_bios_return(Pce *pce)
{
    hle_ram_free(pce);
    tam(pce, 6, hle_pull(pce));
    Y = hle_pull(pce);
    X = alu_nz(pce, hle_pull(pce));
    hle_cycles(pce, 2 + 4 + 5 + 2 + 4 + 4 + 7);
    hle_rts(pce);
}

const HleRoutine hle_psg_routines[] = {
    {0xE0D8, "psg_bios", psg_bios,
     {{P_INIT, init_channel}, {P_INIT + 1, init_clear_globals}, {P_INIT + 2, init_routine},
      {P_INIT + 3, init_clear_music}, {P_INIT + 4, init_clear_sfx}, {P_INIT + 5, init_finish},
      {P_DISPATCH, psg_dispatch}, {P_RETURN, psg_bios_return}}},
    {TICK, "psg_tick", psg_tick,
     {{0xE707, psg_tick_sfx_done}, {0xE713, psg_tick_music}, {0xE71D, psg_tick_music_done},
      {0xE729, psg_tick_end}}, 1},
    {0xFEC0, "psg_music", p_start,
     {{0xFEC0 + P_CH, p_ch}, {0xFEC0 + P_SEQ, p_seq}, {0xFEC0 + P_SLIDES, p_slides},
      {0xFEC0 + P_PATTERN, p_pattern}, {0xFEC0 + P_ENV, p_env}, {0xFEC0 + P_PITCH, p_pitch},
      {0xFEC0 + P_NEXT, p_next}, {0xFEC0 + P_OUT, p_out}}, 1},
    {0xFEC0 + P_WAVE, "psg_music_wave", p_wave, {{0}}, 1},
    {0xFEE0, "psg_sfx", p_start,
     {{0xFEE0 + P_CH, p_ch}, {0xFEE0 + P_SEQ, p_seq}, {0xFEE0 + P_SLIDES, p_slides},
      {0xFEE0 + P_PATTERN, p_pattern}, {0xFEE0 + P_ENV, p_env}, {0xFEE0 + P_PITCH, p_pitch},
      {0xFEE0 + P_NEXT, p_next}, {0xFEE0 + P_OUT, p_out}}, 1},
    {0xFEE0 + P_WAVE, "psg_sfx_wave", p_wave, {{0}}, 1},
    {0, NULL, NULL, {{0}}, 0},
};
