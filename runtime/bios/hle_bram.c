/*
 * C BIOS: CD save RAM (bm_*). See hle.h and runtime/cd/cdrom.h (the 2 KB memory sits in
 * bank $F7, unlocked by writing $80 to $1807 and locked again by reading $1803).
 *
 * Format (the same as the consoles, so saves are compatible): $8000 "HUBM", $8004/$8005 end of the
 * memory, $8006/$8007 end of the used area, $8010... the files. Each file: size (2 bytes, including the
 * 16-byte header), checksum (2 bytes: the negative of the sum of the data), name (12 bytes: 2 of
 * "code" + 10), data. A file of size 0 marks the end.
 *
 * While they touch the memory, the routines disable interrupts and slow the CPU down; at the end they restore
 * the memory windows and wait for the next VBlank (reading the VDC status) before returning.
 *
 * Inputs: ($20F8) file name; ($20FA) buffer in RAM; $20FC/$20FD size; $20FE/$20FF position
 * within the file. Variables: $22B3/$22B4 counter; $22B5-$22BC saved windows.
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

static uint16_t ptr(Pce *pce, uint8_t zp) { return (uint16_t)(z(pce, zp) | (z(pce, (uint8_t)(zp + 1)) << 8)); }
static uint8_t pr(Pce *pce, uint8_t zp, uint8_t y) { return m(pce, (uint16_t)(ptr(pce, zp) + y)); }
static void pw(Pce *pce, uint8_t zp, uint8_t y, uint8_t v) { mw(pce, (uint16_t)(ptr(pce, zp) + y), v); }

static void tam(Pce *pce, int win, uint8_t v)
{
    pce->cpu.mpr[win] = v;
    pce->cpu.mpr_buffer = v;
}

static uint8_t tma(Pce *pce, int win)
{
    pce->cpu.mpr_buffer = pce->cpu.mpr[win];
    return pce->cpu.mpr[win];
}

/* Pair of bytes in the zero page ($lo, $lo+1) as a 16-bit number */
static uint16_t zp16(Pce *pce, uint8_t lo) { return (uint16_t)(z(pce, lo) | (z(pce, (uint8_t)(lo + 1)) << 8)); }

static void zp16w(Pce *pce, uint8_t lo, uint16_t v)
{
    zw(pce, lo, (uint8_t)v);
    zw(pce, (uint8_t)(lo + 1), (uint8_t)(v >> 8));
}

static const uint8_t k_magic[4] = {'H', 'U', 'B', 'M'};

#define BM_CLOSE 0xFC98

/* ---- Open / close ---- */

/* The internal points sit at the same addresses as in the BIOS and do the same as it does there (the real
 * BIOS also goes through them when only some of the routines are in C, during verification).
 *
 * FC4B: opens (FC62) and checks the header. The routine calls it on entry and continues in `body` (return
 * from the JSR). FC62: saves the windows (interrupts can still enter here); FC8F: save RAM in window 4,
 * slow CPU, interrupts disabled. */
static void bm_begin(Pce *pce, uint16_t body)
{
    bios_call(pce, 0xFC4B, body);       /* JSR FC4B */
    bios_call(pce, 0xFC62, 0xFC4D);     /* BSR FC62 */
    hle_push(pce, A);
    for (int i = 0; i < 8; i++)
        mw(pce, (uint16_t)(0x22B5 + i), tma(pce, i));
    A = 0xF7; /* LDA #$F7; TAM #$10 (A is left as in the BIOS) */
    tam(pce, 4, A);
    hle_cycles(pce, 4 + 7 + 8 + 3 + 8 * 9 + 2 + 5);
    pce->cpu.pc = 0xFC8F;
}

static void bm_open_rest(Pce *pce)
{
    pce->cpu.speed = 0;
    pce->cpu.p |= HU_FLAG_I;
    A = 0x80;
    hle_idle(pce, 3 + 2 + 2); /* CSL, SEI, LDA # */
    mw(pce, 0x1807, A);
    A = alu_nz(pce, hle_pull(pce));
    hle_cycles(pce, 3 + 2 + 2 + 5 + 4 + 7);
    hle_rts(pce);
}

static void bm_open_check(Pce *pce)
{
    for (X = 3;; X--) {
        uint8_t v = m(pce, (uint16_t)(0x8000 + X));
        alu_cmp(pce, v, k_magic[X]);
        if (v != k_magic[X]) {
            hle_cycles(pce, 2 + (3 - X) * 18 + 5 + 5 + 4 + 8); /* LDX, whole iterations, LDA, CMP, BNE, BSR */
            bios_call(pce, 0xFC98, 0xFC5D); /* not formatted: closes and returns with A = $FF */
            return;
        }
        if (X == 0)
            break;
    }
    X = alu_nz(pce, 0xFF);
    hle_cycles(pce, 2 + 4 * 18 - 2 + 7); /* iteration: LDA, CMP, BNE, DEX, BPL (the last one does not branch) */
    hle_rts(pce);
}

/* FC98: restores the windows, locks the memory, fast CPU; then waits for the VBlank (points FCD1/FCD8) */
static void bm_close(Pce *pce)
{
    hle_ram_free(pce);
    hle_push(pce, A);
    for (int i = 0; i < 8; i++)
        tam(pce, i, m(pce, (uint16_t)(0x22B5 + i)));
    hle_idle(pce, 3 + 8 * (5 + 5)); /* PHA, LDA/TAM (slow CPU) */
    m(pce, 0x1803);
    pce->cpu.speed = 1; /* CSH: it and the rest already on the fast CPU */
    hle_idle(pce, 3 + 2);
    mw(pce, 0x0000, 0x05);
    hle_idle(pce, 4 + 2);
    mw(pce, 0x0002, (uint8_t)(z(pce, 0xF3) | 0x08));
    pce->cpu.pc = 0xFCD1;
}

static void bm_close_wait_clear(Pce *pce)
{
    A = alu_nz(pce, (uint8_t)(m(pce, 0x0000) & 0x20));
    hle_idle(pce, A ? 2 + 4 : 2 + 2); /* (with hle_idle the VDC's extra cycle stays, as on the CPU) */
    pce->cpu.pc = A ? 0xFCD1 : 0xFCD8;
}

static void bm_close_wait_vblank(Pce *pce)
{
    A = alu_nz(pce, (uint8_t)(m(pce, 0x0000) & 0x20));
    if (!A) {
        hle_idle(pce, 2 + 4);
        pce->cpu.pc = 0xFCD8;
        return;
    }
    hle_ram_free(pce);
    hle_idle(pce, 2 + 2 + 4); /* AND, BEQ, LDA $F3 */
    mw(pce, 0x0002, z(pce, 0xF3));
    hle_idle(pce, 4);
    mw(pce, 0x0000, z(pce, 0xF7));
    A = alu_nz(pce, hle_pull(pce));
    pce->cpu.p &= (uint8_t)~HU_FLAG_I; /* CLI; the RTS is another instruction (the pending interrupt enters on it) */
    hle_idle(pce, 4 + 2); /* PLA, CLI */
    pce->cpu.pc = 0xFCEB;
}

static void bm_close_return(Pce *pce)
{
    hle_cycles(pce, 7);
    hle_rts(pce);
}

/* FC5D: end with unformatted memory: discards the return to the routine body and returns with A = $FF */
static void bm_fail_done(Pce *pce)
{
    hle_pull(pce);
    hle_pull(pce);
    A = alu_nz(pce, 0xFF);
    hle_cycles(pce, 4 + 4 + 2 + 7);
    hle_rts(pce);
}

/* End of the body: body time (pce->hle->bram_t + the rest of the routine) and JMP FC98 */
static void bm_finish(Pce *pce, int cycles)
{
    hle_cycles(pce, pce->hle->bram_t + cycles + 4);
    pce->cpu.pc = BM_CLOSE;
}

/* ---- Internal parts ---- */

/* FDB0: counter $22B3/$22B4 - 1; A = low | high */
static uint8_t count_down(Pce *pce)
{
    uint8_t lo = m(pce, 0x22B3);
    pce->hle->bram_t += 7 + 5 + 4 + 7 + 5 + 5 + 7;
    if (lo == 0) {
        mw(pce, 0x22B4, (uint8_t)(m(pce, 0x22B4) - 1));
        pce->hle->bram_t += 5;
    }
    mw(pce, 0x22B3, (uint8_t)(lo - 1));
    A = alu_nz(pce, (uint8_t)(m(pce, 0x22B3) | m(pce, 0x22B4)));
    return A;
}

/* FD94: maps the game's buffer ($20EE) into windows 5 and 6 (using the windows the game had) */
static void map_user(Pce *pce)
{
    pce->hle->bram_t += 7 + 4 + 8 + 2 + 2 + 5 + 5 + 5 + 5 + 4 + 2 + 2 + 4 + 7;
    A = z(pce, 0xEF);
    for (int i = 0; i < 4; i++)
        A = alu_rol(pce, A);
    X = alu_nz(pce, (uint8_t)(A & 0x07));
    tam(pce, 5, m(pce, (uint16_t)(0x22B5 + X)));
    tam(pce, 6, m(pce, (uint16_t)(0x22B6 + X)));
    A = alu_nz(pce, (uint8_t)((z(pce, 0xEF) & 0x1F) | 0xA0));
    zw(pce, 0xEF, A);
}

/* FC15: sum of the file data at ($20F0) -> $20EE/$20EF */
static void checksum(Pce *pce)
{
    zw(pce, 0xEC, z(pce, 0xF0));
    zw(pce, 0xED, z(pce, 0xF1));
    Y = 1;
    alu_setc(pce, 1);
    mw(pce, 0x22B3, alu_sbc(pce, pr(pce, 0xEC, 0), 0x04));
    mw(pce, 0x22B4, alu_sbc(pce, pr(pce, 0xEC, 1), 0x00));
    zw(pce, 0xEE, 0);
    zw(pce, 0xEF, 0);
    Y = 4;
    pce->hle->bram_t += 7 + 4 + 4 + 4 + 4 + 2 + 7 + 2 + 2 + 5 + 7 + 2 + 5 + 4 + 4 + 2 + 7;
    do {
        pce->hle->bram_t += 7 + 2 + 4 + 4 + 2 + 4 + 4 + 2 + 4 + 4;
        alu_setc(pce, 0);
        zw(pce, 0xEE, alu_adc(pce, pr(pce, 0xEC, Y), z(pce, 0xEE)));
        zw(pce, 0xEF, alu_adc(pce, 0, z(pce, 0xEF)));
        Y++;
        if (Y == 0) {
            zw(pce, 0xED, (uint8_t)(z(pce, 0xED) + 1));
            pce->hle->bram_t += 6 - 2; /* BNE not taken, INC */
        }
    } while (count_down(pce));
    pce->hle->bram_t += -2; /* the last BNE is not taken (the RTS is already counted at the start) */
}

/* FBAF: looks for the file named at ($20F8); found: C = 0, ($20F0) = the file */
static int find_file(Pce *pce)
{
    zw(pce, 0xEE, z(pce, 0xF8));
    zw(pce, 0xEF, z(pce, 0xF9));
    map_user(pce);
    zp16w(pce, 0xF0, 0x8010);
    pce->hle->bram_t += 7 + 4 + 4 + 4 + 4 + 2 + 4 + 2 + 4;
    for (;;) {
        pce->hle->bram_t += 2 + 7 + 7 + 2;
        Y = 1;
        A = alu_nz(pce, (uint8_t)(pr(pce, 0xF0, 0) | pr(pce, 0xF0, 1)));
        if (!A) {
            alu_setc(pce, 1);
            pce->hle->bram_t += 2 + 2 + 7; /* BEQ taken; SEC, RTS */
            return 0;
        }
        alu_setc(pce, 0);
        zp16w(pce, 0xF0, (uint16_t)(zp16(pce, 0xF0) + 4));
        pce->hle->bram_t += 4 + 2 + 2 + 4 + 4 + 2 + 4 + 2;
        int same = 1;
        for (Y = 0; Y < 0x0C; Y++) {
            uint8_t a = pr(pce, 0xEE, Y), b = pr(pce, 0xF0, Y);
            alu_cmp(pce, a, b);
            A = a;
            if (a != b) {
                pce->hle->bram_t += 7 + 7 + 4; /* BNE taken */
                same = 0;
                break;
            }
            pce->hle->bram_t += 7 + 7 + 2 + 2 + 2 + (Y < 0x0B ? 4 : 2);
        }
        zp16w(pce, 0xF0, (uint16_t)(zp16(pce, 0xF0) - 4));
        pce->hle->bram_t += 4 + 2 + 2 + 4 + 4 + 2 + 4; /* FBE3 / FBF2 */
        if (same) {
            alu_setc(pce, 0);
            pce->hle->bram_t += 2 + 7; /* CLC, RTS */
            return 1;
        }
        Y = 1;
        pce->hle->bram_t += 2 + 7 + 3 + 7 + 2 + 4 + 4 + 4 + 4 + 4 + 4;
        uint16_t size = (uint16_t)(pr(pce, 0xF0, 0) | (pr(pce, 0xF0, 1) << 8));
        zp16w(pce, 0xF0, (uint16_t)(zp16(pce, 0xF0) + size));
    }
}

/* FB5F: $EE = position + size + 16 - file size (how far it goes past the end, if >= 0);
 * $EC = file data + position; $22B3 = requested size. Returns A = $EF (N = fits). */
static uint8_t file_span(Pce *pce)
{
    pce->hle->bram_t += 7 + 27 * 4 + 10 * 2 + 2 * 7 + 2 * 5 + 7;
    alu_setc(pce, 0);
    zw(pce, 0xEE, alu_adc(pce, z(pce, 0xFE), z(pce, 0xFC)));
    zw(pce, 0xEF, alu_adc(pce, z(pce, 0xFF), z(pce, 0xFD)));
    alu_setc(pce, 0);
    zw(pce, 0xEE, alu_adc(pce, z(pce, 0xEE), 0x10));
    zw(pce, 0xEF, alu_adc(pce, z(pce, 0xEF), 0x00));
    Y = 1;
    alu_setc(pce, 1);
    zw(pce, 0xEE, alu_sbc(pce, z(pce, 0xEE), pr(pce, 0xF0, 0)));
    zw(pce, 0xEF, alu_sbc(pce, z(pce, 0xEF), pr(pce, 0xF0, 1)));
    alu_setc(pce, 0);
    zw(pce, 0xEC, alu_adc(pce, z(pce, 0xF0), 0x10));
    zw(pce, 0xED, alu_adc(pce, z(pce, 0xF1), 0x00));
    alu_setc(pce, 0);
    zw(pce, 0xEC, alu_adc(pce, z(pce, 0xFE), z(pce, 0xEC)));
    zw(pce, 0xED, alu_adc(pce, z(pce, 0xFF), z(pce, 0xED)));
    mw(pce, 0x22B3, z(pce, 0xFC));
    mw(pce, 0x22B4, z(pce, 0xFD));
    A = alu_nz(pce, z(pce, 0xEF));
    return A;
}

/* FAFB: creates an empty file (header only, name from ($20F8)) at the end; C = 1 if it does not fit */
static int new_file(Pce *pce)
{
    pce->hle->bram_t += 7 + 5 + 4 + 5 + 4 + 5 + 2 + 4 + 4 + 5 + 4 + 4 + 4 + 2 + 4 + 2; /* JSR, FAFB-FB1A */
    zw(pce, 0xF0, m(pce, 0x8006));
    zw(pce, 0xF1, m(pce, 0x8007));
    alu_setc(pce, 1);
    zw(pce, 0xEC, alu_sbc(pce, m(pce, 0x8004), z(pce, 0xF0)));
    zw(pce, 0xED, alu_sbc(pce, m(pce, 0x8005), z(pce, 0xF1)));
    alu_cmp(pce, z(pce, 0xEE), 0x10);
    A = alu_sbc(pce, z(pce, 0xEF), 0x00);
    if (!alu_c(pce)) {
        alu_setc(pce, 1);
        pce->hle->bram_t += 4 + 2 + 7; /* BCC taken; SEC, RTS */
        return 0;
    }
    alu_setc(pce, 0);
    zw(pce, 0xEC, alu_adc(pce, z(pce, 0xF0), 0x04));
    zw(pce, 0xED, alu_adc(pce, z(pce, 0xF1), 0x00));
    zw(pce, 0xEE, z(pce, 0xF8));
    zw(pce, 0xEF, z(pce, 0xF9));
    pce->hle->bram_t += 2 + 4 + 2 + 2 + 4 + 4 + 2 + 4 + 4 + 4 + 4 + 4; /* BCC, FB1E-FB31 */
    map_user(pce);
    /* CLY; 12 iterations (the last BNE is not taken); FB40-FB5C with the RTS */
    pce->hle->bram_t += 2 + 12 * (7 + 7 + 2 + 2 + 4) - 2 + 5 + 2 + 2 + 5 + 5 + 2 + 5 + 2 + 2 + 7 + 2 + 7 + 2 + 7;
    for (Y = 0; Y != 0x0C; Y++)
        pw(pce, 0xEC, Y, pr(pce, 0xEE, Y));
    alu_cmp(pce, Y, 0x0C);
    alu_setc(pce, 0);
    mw(pce, 0x8006, alu_adc(pce, m(pce, 0x8006), 0x10));
    mw(pce, 0x8007, alu_adc(pce, m(pce, 0x8007), 0x00));
    Y = 1;
    A = 0x10;
    pw(pce, 0xF0, 0, A);
    A = 0x00;
    pw(pce, 0xF0, Y, A);
    alu_setc(pce, 0);
    return 1;
}

/* FD5A: sets up the overlapping copy: X = 0 copies backward (destination after), X = 2
 * forward */
static void move_setup(Pce *pce)
{
    pce->hle->bram_t += 7 + 4 + 2 + 4 + 4 + 4; /* JSR, FD5A-FD61 */
    alu_setc(pce, 1);
    alu_sbc(pce, z(pce, 0xEC), z(pce, 0xEE));
    alu_sbc(pce, z(pce, 0xED), z(pce, 0xEF));
    if (alu_c(pce)) {
        X = alu_nz(pce, 0x02);
        pce->hle->bram_t += 4 + 2 + 7; /* BCS taken; LDX #, RTS */
        return;
    }
    pce->hle->bram_t += 2 + 2 * (4 + 2 + 5 + 4 + 4 + 5 + 4) + 7 + 2 + 7; /* BCS; FD68-FD86; LDX #, RTS */
    count_down(pce);
    alu_setc(pce, 0);
    zw(pce, 0xEC, alu_adc(pce, z(pce, 0xEC), m(pce, 0x22B3)));
    zw(pce, 0xED, alu_adc(pce, z(pce, 0xED), m(pce, 0x22B4)));
    alu_setc(pce, 0);
    zw(pce, 0xEE, alu_adc(pce, z(pce, 0xEE), m(pce, 0x22B3)));
    zw(pce, 0xEF, alu_adc(pce, z(pce, 0xEF), m(pce, 0x22B4)));
    uint8_t lo = (uint8_t)(m(pce, 0x22B3) + 1);
    mw(pce, 0x22B3, alu_nz(pce, lo));
    if (lo == 0)
        mw(pce, 0x22B4, alu_nz(pce, (uint8_t)(m(pce, 0x22B4) + 1)));
    pce->hle->bram_t += lo == 0 ? 2 + 7 : 4; /* BNE */
    X = alu_nz(pce, 0x00);
}

/* FCEC: moves $22B3 bytes from ($EC) to ($EE) and marks the end of the used area */
static void move_tail(Pce *pce)
{
    /* JSR, LDA, ORA; FD1E-FD2F with the RTS */
    pce->hle->bram_t += 7 + 5 + 5 + 5 + 4 + 5 + 4 + 2 + 2 + 7 + 7 + 7;
    A = alu_nz(pce, (uint8_t)(m(pce, 0x22B3) | m(pce, 0x22B4)));
    if (A) {
        pce->hle->bram_t += 2; /* BEQ */
        move_setup(pce);
        Y = 0;
        A = alu_nz(pce, X);
        pce->hle->bram_t += 2 + 2 + (A ? 4 : 2); /* CLY, TXA, BNE */
        if (!A) {
            do {
                pce->hle->bram_t += 7 + 7 + 2 + 4 + 2 + 4;
                pw(pce, 0xEE, Y, pr(pce, 0xEC, Y));
                A = alu_nz(pce, Y);
                if (!A) {
                    zw(pce, 0xED, (uint8_t)(z(pce, 0xED) - 1));
                    zw(pce, 0xEF, (uint8_t)(z(pce, 0xEF) - 1));
                    pce->hle->bram_t += 2 + 6 + 6 - 4;
                }
                Y--;
            } while (count_down(pce));
            pce->hle->bram_t += -2 + 4; /* the last BNE is not taken; BEQ */
        } else {
            do {
                pce->hle->bram_t += 7 + 7 + 2 + 4 + 4;
                pw(pce, 0xEE, Y, pr(pce, 0xEC, Y));
                Y++;
                if (!Y) {
                    zw(pce, 0xED, (uint8_t)(z(pce, 0xED) + 1));
                    zw(pce, 0xEF, (uint8_t)(z(pce, 0xEF) + 1));
                    pce->hle->bram_t += 2 + 6 + 6 - 4;
                }
            } while (count_down(pce));
            pce->hle->bram_t += -2;
        }
    } else {
        pce->hle->bram_t += 4; /* BEQ taken */
    }
    zw(pce, 0xEC, m(pce, 0x8006));
    zw(pce, 0xED, m(pce, 0x8007));
    Y = 1;
    A = 0;
    pw(pce, 0xEC, 0, A);
    pw(pce, 0xEC, Y, A);
}

/* FD30 / FD45: copies $22B3 bytes ($EC) -> buffer ($EE) / buffer -> ($EC) */
static void copy_bytes(Pce *pce, int to_user)
{
    map_user(pce);
    Y = 0;
    pce->hle->bram_t += 7 + 2 + 7;
    do {
        pce->hle->bram_t += 7 + 7 + 2 + 4 + 4;
        if (to_user)
            pw(pce, 0xEE, Y, pr(pce, 0xEC, Y));
        else
            pw(pce, 0xEC, Y, pr(pce, 0xEE, Y));
        Y++;
        if (!Y) {
            zw(pce, 0xED, (uint8_t)(z(pce, 0xED) + 1));
            zw(pce, 0xEF, (uint8_t)(z(pce, 0xEF) + 1));
            pce->hle->bram_t += 2 + 6 + 6 - 4;
        }
    } while (count_down(pce));
    pce->hle->bram_t += -2; /* the last BNE is not taken */
}


/* ---- bm_free ($E04B): $20FC/$20FD = free bytes (excluding the header of a new file) ---- */

static void bm_free(Pce *pce)
{
    bm_begin(pce, 0xF8D4);
}

static void bm_free_body(Pce *pce)
{
    hle_ram_free(pce); /* memory only: the time is the BIOS's (bram_t + the rest), not that of the accesses */
    pce->hle->bram_t = 0;
    alu_setc(pce, 1);
    zw(pce, 0xFC, alu_sbc(pce, m(pce, 0x8004), m(pce, 0x8006)));
    zw(pce, 0xFD, alu_sbc(pce, m(pce, 0x8005), m(pce, 0x8007)));
    alu_setc(pce, 1);
    zw(pce, 0xFC, alu_sbc(pce, z(pce, 0xFC), 0x12));
    A = alu_sbc(pce, z(pce, 0xFD), 0x00);
    zw(pce, 0xFD, A);
    int neg = (A & 0x80) != 0;
    if (neg) {
        zw(pce, 0xFC, 0);
        zw(pce, 0xFD, 0);
    }
    A = 0;
    /* F8D4-F8F8: the arithmetic, BPL (or STZ STZ), CLA */
    bm_finish(pce, 2 * (5 + 5 + 4) + 2 + 4 + 2 + 2 + 4 + 4 + 2 + 4 + (neg ? 2 + 4 + 4 : 4) + 2);
}

/* ---- bm_read ($E04E): reads $20FC/$20FD bytes of the file ($20F8), starting at position $20FE/$20FF, into
 * ($20FA). A = 0 ok, 1 = does not exist, 2 = bad checksum. Returns in $20FC/$20FD how many it read. ---- */

static void bm_read(Pce *pce)
{
    bm_begin(pce, 0xF8FF);
}

static void bm_read_body(Pce *pce)
{
    hle_ram_free(pce); /* memory only: the time is the BIOS's (bram_t + the rest), not that of the accesses */
    pce->hle->bram_t = 0;
    if (!find_file(pce)) {
        A = alu_nz(pce, 0x01);
        bm_finish(pce, 4 + 2); /* BCS, LDA # */
        return;
    }
    Y = 1;
    A = z(pce, 0xFF);
    uint8_t hi = pr(pce, 0xF0, Y);
    alu_cmp(pce, A, hi);
    int t = 2 + 2 + 4 + 7; /* BCS, LDY #, LDA, CMP (),Y */
    if (A == hi) {
        A = z(pce, 0xFE);
        alu_cmp(pce, A, pr(pce, 0xF0, 0));
        t += 2 + 4 + 7;
    } else {
        t += 4;
    }
    if (alu_c(pce)) {
        zw(pce, 0xFD, 0);
        zw(pce, 0xFC, 0);
        A = 0;
        bm_finish(pce, t + 4 + 4 + 4 + 2); /* BCS, STZ, STZ, CLA */
        return;
    }
    t += 2 + 2 + 7 + 2 + 2 + 4 + 4 + 7 + 4 + 4 + 3; /* BCS; F915-F925 */
    checksum(pce);
    Y = 2;
    A = pr(pce, 0xF0, Y);
    Y++;
    alu_setc(pce, 0);
    A = alu_adc(pce, A, z(pce, 0xEE));
    zw(pce, 0xEE, A);
    A = alu_adc(pce, pr(pce, 0xF0, Y), z(pce, 0xEF));
    A = alu_nz(pce, (uint8_t)(A | z(pce, 0xEE)));
    uint8_t sum = A;
    if (!(file_span(pce) & 0x80)) {
        alu_setc(pce, 1);
        mw(pce, 0x22B3, alu_sbc(pce, m(pce, 0x22B3), z(pce, 0xEE)));
        mw(pce, 0x22B4, alu_sbc(pce, m(pce, 0x22B4), z(pce, 0xEF)));
        t += 2 + 5 + 2 + 4 + 5 + 5 + 4 + 5; /* BMI; F92B-F939 */
    } else {
        t += 4;
    }
    zw(pce, 0xFC, m(pce, 0x22B3));
    A = m(pce, 0x22B4);
    zw(pce, 0xFD, A);
    A = alu_nz(pce, (uint8_t)(A | m(pce, 0x22B3)));
    t += 5 + 4 + 5 + 4 + 5;
    if (A) {
        zw(pce, 0xEE, z(pce, 0xFA));
        zw(pce, 0xEF, z(pce, 0xFB));
        copy_bytes(pce, 1);
        t += 2 + 4 + 4 + 4 + 4;
    } else {
        t += 4;
    }
    A = alu_nz(pce, sum);
    t += 4; /* PLA */
    if (A) {
        A = alu_nz(pce, 0x02);
        t += 4 + 2;
    } else {
        t += 2;
    }
    bm_finish(pce, t);
}

/* ---- bm_write ($E051): writes $20FC/$20FD bytes from ($20FA) into the file ($20F8) starting at position
 * $20FE/$20FF (creates the file or grows it, if needed). A = 0 ok, 1 = does not fit. ---- */

static void bm_write_data(Pce *pce)
{
    zw(pce, 0xEE, z(pce, 0xFA));
    zw(pce, 0xEF, z(pce, 0xFB));
    copy_bytes(pce, 0);
    checksum(pce);
    Y = 2;
    alu_setc(pce, 1);
    A = alu_sbc(pce, 0, z(pce, 0xEE));
    pw(pce, 0xF0, Y, A);
    Y++;
    A = alu_sbc(pce, 0, z(pce, 0xEF));
    pw(pce, 0xF0, Y, A);
    A = 0;
}

static void bm_write(Pce *pce)
{
    bm_begin(pce, 0xF971);
}

static void bm_write_body(Pce *pce)
{
    hle_ram_free(pce); /* memory only: the time is the BIOS's (bram_t + the rest), not that of the accesses */
    pce->hle->bram_t = 0;
    int t;
    if (find_file(pce)) {
        t = 4; /* BCC taken */
    } else if (new_file(pce)) {
        t = 2 + 4;
    } else {
        A = alu_nz(pce, 0x01);
        bm_finish(pce, 2 + 2 + 4 + 2); /* BCC, BCC, JMP FA2E, LDA # */
        return;
    }
    if (!(file_span(pce) & 0x80)) {
        t += 4 + 4 + 5 + 4 + 5 + 5 + 2 + 5 + 4 + 5 + 5 + 4 + 4 + 2 + 2 + 4 + 4 + 2 + 4 + 5 + 4 + 5 + 4; /* BPL, F986-F9B6 */
        /* goes past the end of the file: grows it (moves what comes after) */
        mw(pce, 0x22B3, z(pce, 0xEE));
        mw(pce, 0x22B4, z(pce, 0xEF));
        alu_setc(pce, 0);
        zw(pce, 0xEE, alu_adc(pce, m(pce, 0x8006), m(pce, 0x22B3)));
        zw(pce, 0xEF, alu_adc(pce, m(pce, 0x8007), m(pce, 0x22B4)));
        alu_setc(pce, 0);
        zw(pce, 0xEC, alu_adc(pce, z(pce, 0xEE), 0x02));
        zw(pce, 0xED, alu_adc(pce, z(pce, 0xEF), 0x00));
        alu_cmp(pce, m(pce, 0x8004), z(pce, 0xEC));
        alu_sbc(pce, m(pce, 0x8005), z(pce, 0xED));
        if (!alu_c(pce)) {
            A = alu_nz(pce, 0x01);
            bm_finish(pce, t + 4 + 2); /* BCC taken, LDA # */
            return;
        }
        t += 2 + 4 + 5 + 4 + 5; /* BCC, F9BA-F9C1 */
        t += 2 + 7 + 2 + 4 + 4 + 4 + 7 + 4 + 4 + 4 + 2 + 5 + 2 + 7 + 7 + 5 + 7 + 7; /* F9C4-F9E6 */
        t += 5 + 2 + 4 + 4 + 5 + 4 + 4 + 5 + 2 + 4 + 5 + 5 + 4 + 5;                 /* F9E8-FA05 */
        mw(pce, 0x8006, z(pce, 0xEE));
        mw(pce, 0x8007, z(pce, 0xEF));
        Y = 1;
        alu_setc(pce, 0);
        A = alu_adc(pce, pr(pce, 0xF0, 0), z(pce, 0xF0));
        zw(pce, 0xEE, A);
        zw(pce, 0xEC, A);
        A = alu_adc(pce, pr(pce, 0xF0, Y), z(pce, 0xF1));
        zw(pce, 0xEF, A);
        zw(pce, 0xED, A);
        Y = 1;
        alu_setc(pce, 0);
        pw(pce, 0xF0, 0, alu_adc(pce, m(pce, 0x22B3), pr(pce, 0xF0, 0)));
        pw(pce, 0xF0, Y, alu_adc(pce, m(pce, 0x22B4), pr(pce, 0xF0, Y)));
        alu_setc(pce, 0);
        zw(pce, 0xEE, alu_adc(pce, m(pce, 0x22B3), z(pce, 0xEE)));
        zw(pce, 0xEF, alu_adc(pce, m(pce, 0x22B4), z(pce, 0xEF)));
        alu_setc(pce, 1);
        mw(pce, 0x22B3, alu_sbc(pce, m(pce, 0x8006), z(pce, 0xEC)));
        mw(pce, 0x22B4, alu_sbc(pce, m(pce, 0x8007), z(pce, 0xED)));
        move_tail(pce);
        file_span(pce);
    } else {
        t += 2 + 4; /* BPL, JMP FA0E */
    }
    bm_write_data(pce);
    /* FA0E-FA14; FA1C-FA2A */
    bm_finish(pce, t + 4 + 4 + 4 + 4 + 2 + 2 + 2 + 4 + 7 + 2 + 2 + 4 + 7 + 2);
}

/* ---- bm_files ($E057): name of file number $20F8 (1 = the first) into ($20FA), plus 2 bytes with the
 * data size. A = 0 ok, 1 = does not exist. ---- */

static void bm_files(Pce *pce)
{
    bm_begin(pce, 0xFA8E);
}

static void bm_files_body(Pce *pce)
{
    hle_ram_free(pce); /* memory only: the time is the BIOS's (bram_t + the rest), not that of the accesses */
    pce->hle->bram_t = 0;
    zp16w(pce, 0xF0, 0x8010);
    mw(pce, 0x22B3, z(pce, 0xF8));
    zw(pce, 0xF8, 0);
    Y = 1;
    int t = 2 + 4 + 2 + 4 + 4 + 5 + 4 + 2; /* FA8E-FA9D */
    for (;;) {
        A = alu_nz(pce, (uint8_t)(pr(pce, 0xF0, 0) | pr(pce, 0xF0, Y)));
        if (!A) {
            A = alu_nz(pce, 0x01);
            bm_finish(pce, t + 7 + 7 + 4 + 2); /* LDA, ORA, BEQ taken, LDA # */
            return;
        }
        zw(pce, 0xF8, (uint8_t)(z(pce, 0xF8) + 1));
        uint8_t left = alu_nz(pce, (uint8_t)(m(pce, 0x22B3) - 1));
        mw(pce, 0x22B3, left);
        t += 7 + 7 + 2 + 6 + 7; /* LDA, ORA, BEQ, INC, DEC */
        if (!left) {
            t += 4;
            break;
        }
        uint8_t hi = pr(pce, 0xF0, Y);
        alu_setc(pce, 0);
        zw(pce, 0xF0, alu_adc(pce, pr(pce, 0xF0, 0), z(pce, 0xF0)));
        zw(pce, 0xF1, alu_adc(pce, hi, z(pce, 0xF1)));
        t += 2 + 7 + 3 + 7 + 2 + 4 + 4 + 4 + 4 + 4 + 4; /* BEQ, FAAC-FABB */
    }
    alu_setc(pce, 1);
    uint8_t lo = alu_sbc(pce, pr(pce, 0xF0, 0), 0x10);
    X = lo;
    uint8_t hi = alu_sbc(pce, pr(pce, 0xF0, Y), 0x00);
    alu_setc(pce, 0);
    zw(pce, 0xF0, alu_adc(pce, z(pce, 0xF0), 0x04));
    zw(pce, 0xF1, alu_adc(pce, z(pce, 0xF1), 0x00));
    zw(pce, 0xEE, z(pce, 0xFA));
    zw(pce, 0xEF, z(pce, 0xFB));
    t += 7 + 2 + 2 + 2 + 7 + 2 + 3 + 3 + 4 + 2 + 2 + 4 + 4 + 2 + 4 + 4 + 4 + 4 + 4; /* FABD-FADC */
    map_user(pce);
    for (Y = 0; Y < 0x0C; Y++)
        pw(pce, 0xEE, Y, pr(pce, 0xF0, Y));
    alu_cmp(pce, Y, 0x0C);
    pw(pce, 0xEE, Y, lo);
    Y++;
    pw(pce, 0xEE, Y, hi);
    A = alu_nz(pce, hi);
    A = 0;
    /* CLY; 12 iterations (the last BCC is not taken); PLA STA INY PLA STA CLA */
    bm_finish(pce, t + 2 + 12 * (7 + 7 + 2 + 2 + 4) - 2 + 4 + 7 + 2 + 4 + 7 + 2);
}

const HleRoutine hle_bram_routines[] = {
    {0xE04B, "bm_free", bm_free, {{0xF8D4, bm_free_body}}},
    {0xE04E, "bm_read", bm_read, {{0xF8FF, bm_read_body}}},
    {0xE051, "bm_write", bm_write, {{0xF971, bm_write_body}}},
    {0xE057, "bm_files", bm_files, {{0xFA8E, bm_files_body}}},
    {BM_CLOSE, "bm_close", bm_close,
     {{0xFCD1, bm_close_wait_clear}, {0xFCD8, bm_close_wait_vblank}, {0xFC5D, bm_fail_done},
      {0xFC8F, bm_open_rest}, {0xFC4D, bm_open_check}, {0xFCEB, bm_close_return}}, 1},
    {0, NULL, NULL, {{0}}, 0},
};
