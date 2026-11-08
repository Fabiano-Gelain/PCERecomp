/*
 * C BIOS: digitized sound (ADPCM, ad_*) and the CD interrupt (IRQ2). See hle.h and, for the
 * hardware, runtime/cd/adpcm.h.
 *
 * Registers: $1808/$1809 ADPCM RAM address (and length), $180A data, $180B CD-to-ADPCM-RAM DMA,
 * $180C status (bit 0 end, bit 2 DMA busy, bit 3 playing, bit 7 read busy),
 * $180D control (bit 3 latches the read address, bits 0-1 the write address, bit 4 the length, bits 5-6
 * play), $180E rate. $1802/$1803: CD interrupts (bit 2 ADPCM half... bit 3 ADPCM end,
 * bit 5 status ready, bit 6 data ready).
 *
 * BIOS variables: $22A5 play again when finished (bit 7); $22A6-$22AA the last ad_play (length,
 * address, rate); $229D-$22A3 background CD read (sectors at a time, how many remain,
 * next sector); $20F2 pending CD interrupts.
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

static void call(Pce *pce, uint16_t target, uint16_t ret) { bios_call(pce, target, ret); }
static void go(Pce *pce, uint16_t pc) { pce->cpu.pc = pc; }

#define AD_READ_BYTE 0xF417 /* waits for the previous read and reads a byte from ADPCM RAM ($180A) */

/* ---- Non-waiting parts ---- */

/* F6F4: status (X = DMA finished/busy, A = playing); returns the cycles spent in the BIOS */
static int ad_status(Pce *pce)
{
    int cycles = 9 + 11 + 7; /* F6F4, F700, RTS */
    A = alu_nz(pce, (uint8_t)(m(pce, 0x180C) & 0x01));
    if (A) {
        cycles += 2;
    } else {
        A = alu_nz(pce, (uint8_t)(m(pce, 0x1803) & 0x04));
        cycles += 7; /* F6FB */
    }
    X = A;
    A = alu_nz(pce, (uint8_t)(m(pce, 0x180D) & 0x20));
    if (A) {
        cycles += 2;
    } else {
        A = alu_nz(pce, (uint8_t)(m(pce, 0x180C) & 0x08));
        cycles += 7; /* F708 */
    }
    return cycles;
}

/* F742: address/length X (low), Y (high) */
static void ad_set_value(Pce *pce)
{
    mw(pce, 0x1808, X);
    mw(pce, 0x1809, Y);
}

/* F718: the value becomes the read address (with a dummy read) */
static void ad_latch_read(Pce *pce)
{
    bios_tsb(pce, 0x180D, 0x08);
    A = m(pce, 0x180A);
    A = alu_nz(pce, 0);
    bios_trb(pce, 0x180D, 0x08);
}


/* F737: the value becomes the length */
static void ad_latch_length(Pce *pce)
{
    bios_tsb(pce, 0x180D, 0x10);
    bios_trb(pce, 0x180D, 0x10);
}

/* F60E: plays again what is in $22A6-$22AA */
static void ad_start(Pce *pce)
{
    X = m(pce, 0x22A8);
    Y = m(pce, 0x22A9);
    ad_set_value(pce);
    ad_latch_read(pce);
    X = m(pce, 0x22A6);
    Y = m(pce, 0x22A7);
    ad_set_value(pce);
    ad_latch_length(pce);
    A = m(pce, 0x22AA);
    mw(pce, 0x180E, A);
}

/* F6E7: end of the background read */
static void ad_stream_clear(Pce *pce)
{
    mw(pce, 0x229D, 0);
    mw(pce, 0x229E, 0);
    mw(pce, 0x229F, 0);
    mw(pce, 0x22A0, 0);
}

/* F6A5: the background read advances $229D sectors */
static void ad_stream_advance(Pce *pce)
{
    alu_setc(pce, 0);
    A = alu_adc(pce, m(pce, 0x22A3), m(pce, 0x229D));
    mw(pce, 0x22A3, A);
    A = alu_adc(pce, m(pce, 0x22A2), 0);
    mw(pce, 0x22A2, A);
    A = alu_adc(pce, m(pce, 0x22A1), 0);
    mw(pce, 0x22A1, A);
    alu_setc(pce, 1);
    A = alu_sbc(pce, m(pce, 0x229E), m(pce, 0x229D));
    mw(pce, 0x229E, A);
    A = alu_sbc(pce, m(pce, 0x229F), 0);
    mw(pce, 0x229F, A);
    A = alu_sbc(pce, m(pce, 0x22A0), 0);
    mw(pce, 0x22A0, A);
}

/* Destination ($20FA) + 1 and counter $20F8/$20F9 - 1 (as in the BIOS: DEC $F9 if $F8 was 0, DEC $F8).
 * Returns the extra cycles (FB and F9). */
static int next_byte(Pce *pce, int toggle)
{
    int extra = 0;
    if (toggle) {
        zw(pce, 0xFA, alu_nz(pce, (uint8_t)(z(pce, 0xFA) ^ 0x01)));
    } else {
        uint8_t lo = (uint8_t)(z(pce, 0xFA) + 1);
        zw(pce, 0xFA, lo);
        if (lo == 0) {
            zw(pce, 0xFB, (uint8_t)(z(pce, 0xFB) + 1));
            extra += 4;
        }
    }
    if (z(pce, 0xF8) == 0) {
        zw(pce, 0xF9, (uint8_t)(z(pce, 0xF9) - 1));
        extra += 4;
    }
    zw(pce, 0xF8, (uint8_t)(z(pce, 0xF8) - 1));
    A = alu_nz(pce, (uint8_t)(z(pce, 0xF8) | z(pce, 0xF9)));
    return extra;
}

/* ---- ad_trans ($E033): CD sectors ($20FC-$20FE + base, $20F8 sectors) to ADPCM RAM by DMA.
 * $20FF = 0: starting at address $20FA/$20FB; otherwise continues where it left off. A = 4 if the DMA was
 * busy. ---- */

/* F3AC (`jmp` = cycles of the table JMP; 0 when ad_cplay calls it directly) */
static void ad_trans_body(Pce *pce, int jmp)
{
    if (bios_tst(pce, 0x03, 0x180B)) {
        A = alu_nz(pce, 0x04);
        hle_cycles(pce, jmp + 8 + 4 + 2 + 4 + 7);
        hle_rts(pce);
        return;
    }
    mw(pce, 0x2273, 0);
    bios_save_args(pce);
    hle_cycles(pce, jmp + 165); /* JMP, TST, BEQ, STZ, JSR $F322 (saves $20F8-$20FF) */
    go(pce, 0xF3BC);
}

static void ad_trans(Pce *pce) { ad_trans_body(pce, 4); }
static void ad_trans_direct(Pce *pce) { ad_trans_body(pce, 0); }

static void ad_trans_start(Pce *pce)
{
    /* With hle_ram_free: the ADPCM writes land on the BIOS cycle */
    hle_ram_free(pce);
    A = alu_nz(pce, z(pce, 0xFF));
    int dest = A == 0;
    if (dest) {
        X = z(pce, 0xFA);
        Y = z(pce, 0xFB);
        hle_idle(pce, 4 + 2 + 4 + 4 + 7); /* LDA, BNE, LDX, LDY, JSR $F742 */
        ad_set_value(pce);
        hle_idle(pce, 7 + 7 + 2); /* RTS, JSR $F729, LDA # */
        bios_tsb(pce, 0x180D, 0x03);
        hle_idle(pce, 2);
        bios_trb(pce, 0x180D, 0x01);
        hle_idle(pce, 2 + 4); /* LDA #, BRA */
        bios_trb(pce, 0x180D, 0x02);
    }
    bios_clear_command(pce);
    A = 0x08;
    mw(pce, 0x224C, A);
    bios_add_base(pce);
    bios_put3(pce, 4, 1);
    A = z(pce, 0xF8);
    mw(pce, 0x2250, A);
    hle_cycles(pce, dest ? 342 : 342 - 75); /* without the address: the BNE skips over 83 cycles */
    call(pce, SCSI_SEND, 0xF3E4);
}

static void ad_trans_sent(Pce *pce)
{
    A = 0x02;
    hle_idle(pce, 2); /* LDA #: the transfer starts on the BIOS cycle */
    mw(pce, 0x180B, A);
    hle_cycles(pce, 7);
    go(pce, 0xF3E9);
}

/* F3E9-F3F9: waits for the end of the transfer, one instruction per pseudo-instruction (see hle_next) */
static void ad_trans_wait(Pce *pce)
{
    HuC6280 *c = &pce->cpu;
    uint16_t pc = c->pc;
    int z = (c->p & HU_FLAG_Z) != 0;
    hle_irq_sample(pce);
    switch (pc) {
    case 0xF3E9: { /* TST #$04,$180C */
        uint8_t v = hle_peek(pce, 0x180C);
        c->p = (uint8_t)((c->p & ~(HU_FLAG_Z | HU_FLAG_V | HU_FLAG_N)) | ((v & 0x04) ? 0 : HU_FLAG_Z) |
                         (v & (HU_FLAG_V | HU_FLAG_N)));
        hle_next(pce, 8, 0xF3ED);
        break;
    }
    case 0xF3ED: hle_next(pce, z ? 2 : 4, z ? 0xF3EF : 0xF3E9); break; /* BNE */
    case 0xF3EF: A = alu_nz(pce, hle_peek(pce, 0x1803)); hle_next(pce, 5, 0xF3F2); break;
    case 0xF3F2: A = alu_nz(pce, (uint8_t)(A & 0x20)); hle_next(pce, 2, 0xF3F4); break;
    case 0xF3F4: hle_next(pce, z ? 4 : 2, z ? 0xF3E9 : 0xF3F6); break; /* BEQ */
    case 0xF3F6: hle_poke(pce, 0x180B, 0); hle_next(pce, 5, 0xF3F9); break;
    default: hle_next(pce, 7, 0); call(pce, SCSI_STATUS, 0xF3FC); break; /* F3F9: JSR $E9DC */
    }
}

static void ad_trans_status(Pce *pce)
{
    alu_cmp(pce, A, 0x00);
    if (A == 0) {
        hle_cycles(pce, 11);
        hle_rts(pce);
        return;
    }
    hle_cycles(pce, 11);
    call(pce, CD_ERROR, 0xF403);
}

static void ad_trans_error(Pce *pce)
{
    if (alu_c(pce)) {
        hle_cycles(pce, 11);
        hle_rts(pce);
        return;
    }
    bios_restore_args(pce);
    A = m(pce, 0x227C);
    alu_cmp(pce, A, 0x10);
    if (A >= 0x10) {
        A = 0x01;
        mw(pce, 0x2273, A);
    }
    hle_cycles(pce, 160);
    go(pce, 0xF3BC);
}

/* ---- ad_read ($E036): ADPCM RAM ($20FC/$20FD) to the CPU, $20F8/$20F9 bytes. $20FF = 0: RAM at
 * ($20FA); $FF: VRAM at address $20FA; 2-6: whole banks in window $20FF starting at bank $20FA. --- */

static void ad_read_mpr_window(Pce *pce);

static void ad_read(Pce *pce)
{
    int c = ad_status(pce);
    if (A) {
        hle_cycles(pce, 7 + c + 4);
        go(pce, 0xE036); /* still playing or transferring: wait */
        return;
    }
    X = z(pce, 0xFC);
    Y = z(pce, 0xFD);
    ad_set_value(pce);
    ad_latch_read(pce);
    hle_cycles(pce, 4 + 7 + c + 2 + 4 + 4 + 24 + 7 + 64 + 8);
    call(pce, AD_READ_BYTE, 0xF431);
}

/* F417 */
static void ad_read_byte(Pce *pce)
{
    A = alu_nz(pce, m(pce, 0x180C));
    if (A & 0x80) {
        hle_cycles(pce, 9);
        go(pce, AD_READ_BYTE);
        return;
    }
    A = alu_nz(pce, m(pce, 0x180A));
    hle_cycles(pce, 19);
    hle_rts(pce);
}

static void ad_read_dest(Pce *pce)
{
    A = alu_nz(pce, z(pce, 0xFF));
    if (!A) {
        hle_cycles(pce, 8);
        go(pce, 0xF43C);
        return;
    }
    alu_cmp(pce, A, 0xFF);
    if (A == 0xFF) {
        bios_cd_vdc_select(pce, 0x00);
        bios_vram_write_address(pce);
        zw(pce, 0xFA, 0x02);
        zw(pce, 0xFB, 0x00);
        bios_cd_vdc_select(pce, 0x02);
        hle_cycles(pce, 80);
        go(pce, 0xF467);
        return;
    }
    alu_cmp(pce, A, 0x07);
    if (!(pce->cpu.p & HU_FLAG_N)) {
        A = alu_nz(pce, 0x22);
        hle_cycles(pce, 30);
        hle_rts(pce);
        return;
    }
    uint8_t bit = 1;
    for (X = A; X; X--)
        bit = alu_asl(pce, bit);
    A = bit;
    mw(pce, 0x228F, A);
    mw(pce, 0x228E, 0x43); /* TMA: the BIOS builds "TMA #bit; RTS" at $228E and calls it */
    mw(pce, 0x2290, 0x60);
    A = pce->cpu.mpr[bios_window_of_bit(bit)];
    pce->cpu.mpr_buffer = A;
    mw(pce, 0x2268, A);
    A = z(pce, 0xFA);
    zw(pce, 0xFE, A);
    ad_read_mpr_window(pce);
}

static void ad_read_ram(Pce *pce)
{
    hle_cycles(pce, 8);
    call(pce, AD_READ_BYTE, 0xF43E);
}

static void ad_read_ram_next(Pce *pce)
{
    hle_ram_free(pce); /* the timing is the total below */
    mw(pce, (uint16_t)(z(pce, 0xFA) | (z(pce, 0xFB) << 8)), A);
    int extra = next_byte(pce, 0);
    if (A) {
        hle_cycles(pce, 43 + extra);
        go(pce, 0xF43C);
        return;
    }
    A = 0;
    hle_cycles(pce, 50 + extra);
    hle_rts(pce);
}

static void ad_read_vram(Pce *pce)
{
    hle_cycles(pce, 8);
    call(pce, AD_READ_BYTE, 0xF469);
}

static void ad_read_vram_next(Pce *pce)
{
    hle_ram_free(pce); /* the timing is the total below */
    mw(pce, (uint16_t)(z(pce, 0xFA) | (z(pce, 0xFB) << 8)), A);
    int extra = next_byte(pce, 1);
    if (A) {
        hle_cycles(pce, 45 + extra);
        go(pce, 0xF467);
        return;
    }
    mw(pce, 0x2272, 0);
    A = 0;
    hle_cycles(pce, 57 + extra);
    hle_rts(pce);
}

static void ad_read_mpr_window(Pce *pce)
{
    A = z(pce, 0xFF);
    for (int i = 0; i < 5; i++)
        A = alu_asl(pce, A);
    zw(pce, 0xFB, A);
    zw(pce, 0xFA, 0);
    mw(pce, 0x228E, 0x53); /* TAM */
    A = z(pce, 0xFE);
    int win = bios_window_of_bit(m(pce, 0x228F));
    pce->cpu.mpr[win] = A;
    pce->cpu.mpr_buffer = A;
    X = 0x20;
    Y = 0;
    hle_cycles(pce, 70);
    go(pce, 0xF4C1);
}

static void ad_read_mpr(Pce *pce)
{
    hle_cycles(pce, 7);
    call(pce, AD_READ_BYTE, 0xF4C4);
}

static void ad_read_mpr_next(Pce *pce)
{
    hle_ram_free(pce); /* the timing is the total below */
    mw(pce, (uint16_t)(z(pce, 0xFA) | (z(pce, 0xFB) << 8)), A);
    int extra = next_byte(pce, 0);
    if (!A) {
        mw(pce, 0x228E, 0x53);
        A = m(pce, 0x2268);
        int win = bios_window_of_bit(m(pce, 0x228F));
        pce->cpu.mpr[win] = A;
        pce->cpu.mpr_buffer = A;
        A = 0;
        hle_cycles(pce, 80 + extra);
        hle_rts(pce);
        return;
    }
    Y = alu_nz(pce, (uint8_t)(Y - 1));
    if (!Y)
        X = alu_nz(pce, (uint8_t)(X - 1));
    if (Y || X) {
        hle_cycles(pce, 43 + extra);
        go(pce, 0xF4C1);
        return;
    }
    zw(pce, 0xFE, (uint8_t)(z(pce, 0xFE) + 1));
    ad_read_mpr_window(pce);
}

/* ---- ad_play ($E03C): plays $20F8/$20F9 bytes of ADPCM RAM starting at $20FA/$20FB, rate
 * $20FF (0-15). $20FE bit 7: repeat when finished; bits 0-6 != 0: repeat the last one. A != 0 if busy. --- */

static void ad_play(Pce *pce)
{
    int c = ad_status(pce);
    if (A) {
        hle_cycles(pce, 4 + 7 + c + 4 + 7);
        hle_rts(pce);
        return;
    }
    A = (uint8_t)(z(pce, 0xFE) & 0x80);
    mw(pce, 0x22A5, A);
    A = alu_nz(pce, (uint8_t)(z(pce, 0xFE) & 0x7F));
    if (!A) {
        mw(pce, 0x22A8, z(pce, 0xFA));
        mw(pce, 0x22A9, z(pce, 0xFB));
        mw(pce, 0x22A6, z(pce, 0xF8));
        mw(pce, 0x22A7, z(pce, 0xF9));
        A = z(pce, 0xFF);
        alu_cmp(pce, A, 0x10);
        if (A >= 0x10) {
            hle_cycles(pce, 120);
            hle_rts(pce);
            return;
        }
        mw(pce, 0x22AA, A);
        ad_start(pce);
    }
    bios_tsb(pce, 0x1802, 0x08);
    A = 0x60;
    mw(pce, 0x180D, A);
    A = 0;
    hle_cycles(pce, 330);
    hle_rts(pce);
}

/* ---- ad_cplay ($E03F): plays directly from the CD, streaming. Sector $20FC-$20FE (+ base), $20F8-$20FA sectors,
 * rate $20FF. Resets the ADPCM, brings in the first 32 sectors (64 KB, all of the RAM) with ad_trans and
 * leaves the rest to the IRQ2 background read (bit 2: half played). A != 0 if busy,
 * invalid rate or CD error. ---- */

/* The next hardware access lands on cycle `t` of the pseudo-instruction, as on the real BIOS (with hle_ram_free,
 * memory costs no time and the total time comes in the final hle_cycles) */
static void at(Pce *pce, int t)
{
    int rest = t - pce->cpu.instr_cycles;
    if (rest > 0)
        hle_idle(pce, rest);
}

static void ad_cplay(Pce *pce)
{
    hle_idle(pce, 4 + 7); /* JMP, JSR $F6F4 */
    int c = ad_status(pce);
    if (A) {
        hle_cycles(pce, 4 + 7 + c + 4 + 7);
        hle_rts(pce);
        return;
    }
    hle_ram_free(pce);
    mw(pce, 0x229E, z(pce, 0xF8));
    mw(pce, 0x229F, z(pce, 0xF9));
    mw(pce, 0x22A0, z(pce, 0xFA));
    alu_setc(pce, 0);
    A = alu_adc(pce, z(pce, 0xFE), m(pce, 0x2276));
    mw(pce, 0x22A3, A);
    A = alu_adc(pce, z(pce, 0xFD), m(pce, 0x2275));
    mw(pce, 0x22A2, A);
    A = alu_adc(pce, z(pce, 0xFC), m(pce, 0x2274));
    mw(pce, 0x22A1, A);
    /* F398 (ad_reset): JSR after BNE, copies and additions */
    at(pce, 11 + c + 2 + 27 + 44 + 7 + 2);
    mw(pce, 0x180D, 0x80);
    mw(pce, 0x180D, 0);
    mw(pce, 0x180B, 0);
    at(pce, 108 + c + 2);
    bios_trb(pce, 0x1802, 0x6F);
    mw(pce, 0x180E, 0);
    A = z(pce, 0xFF);
    alu_cmp(pce, A, 0x10);
    if (A >= 0x10) {
        hle_cycles(pce, 146 + c); /* RTS, LDA, CMP, BCS, RTS */
        hle_rts(pce);
        return;
    }
    at(pce, 137 + c);
    mw(pce, 0x180E, A);
    zw(pce, 0xF8, 0x20);
    A = 0;
    zw(pce, 0xFF, A);
    zw(pce, 0xFA, 0);
    zw(pce, 0xFB, 0);
    hle_cycles(pce, 169 + c); /* LDA #, STA, LDA #, STA, STZ, STZ, JSR $F3AC */
    call(pce, 0xF3AC, 0xF680);
}

/* F680: return from ad_trans */
static void ad_cplay_sent(Pce *pce)
{
    alu_cmp(pce, A, 0x00);
    if (A) {
        hle_cycles(pce, 2 + 4 + 7);
        hle_rts(pce);
        return;
    }
    hle_ram_free(pce);
    mw(pce, 0x229D, 0x20);
    ad_stream_advance(pce);
    at(pce, 110); /* CMP, BNE, LDA #, STA, BSR $F6A5 (89), LDA # */
    mw(pce, 0x1808, 0xFF);
    mw(pce, 0x1809, 0xFF);
    at(pce, 129); /* JSR $F737, LDA # */
    bios_tsb(pce, 0x180D, 0x10);
    at(pce, 138);
    bios_trb(pce, 0x180D, 0x10);
    mw(pce, 0x22A5, 0);
    at(pce, 159); /* RTS, STZ, LDA # */
    bios_tsb(pce, 0x1802, 0x0C);
    at(pce, 168);
    mw(pce, 0x180D, 0x60);
    A = 0;
    hle_cycles(pce, 182); /* CLA, RTS */
    hle_rts(pce);
}

/* ---- ad_stop ($E042): stops playing and cancels the background read ---- */

static void ad_stop(Pce *pce)
{
    hle_ram_free(pce);
    at(pce, 4 + 2);
    bios_trb(pce, 0x1802, 0x0C);
    at(pce, 15);
    bios_trb(pce, 0x180D, 0x60);
    mw(pce, 0x22A5, 0);
    ad_stream_clear(pce);
    hle_cycles(pce, 54);
    hle_rts(pce);
}

/* ---- ad_stat ($E045): A = playing, X = DMA (Z = idle) ---- */

static void ad_stat(Pce *pce)
{
    int c = ad_status(pce);
    hle_cycles(pce, 4 + c);
    hle_rts(pce);
}

/* ======== IRQ2: CD/ADPCM interrupt ======== */

static void irq_cd_status(Pce *pce);

static void irq_cd(Pce *pce)
{
    if (z(pce, 0xF5) & 0x01) { /* the game handles it itself */
        hle_cycles(pce, 15);
        go(pce, hle_rd16(pce, 0x2200));
        return;
    }
    hle_push(pce, A);
    hle_push(pce, X);
    hle_push(pce, Y);
    hle_cycles(pce, 6 + 9 + 8);
    call(pce, 0xE75B, 0xE757);
}

static void irq_cd_return(Pce *pce)
{
    Y = hle_pull(pce);
    X = hle_pull(pce);
    A = hle_pull(pce);
    hle_rti(pce);
    hle_cycles(pce, 4 * 3 + 7);
}

/* Turns off interrupt `bit` in the CD and in $20F2 and enables interrupts (CLI) */
static void irq_ack(Pce *pce, uint8_t bit)
{
    bios_trb(pce, 0x1802, bit);
    bios_trb(pce, ZP(0xF2), bit);
    pce->cpu.p &= (uint8_t)~HU_FLAG_I;
}

/* E75B: what is requesting: bit 2 = background read (next sectors for the ADPCM) */
static void irq_cd_body(Pce *pce)
{
    A = (uint8_t)(m(pce, 0x1802) & m(pce, 0x1803));
    A = alu_nz(pce, (uint8_t)(A | z(pce, 0xF2)));
    zw(pce, 0xF2, A);
    if (!(A & 0x04)) {
        hle_cycles(pce, 30);
        go(pce, 0xE7CC);
        return;
    }
    irq_ack(pce, 0x04);
    hle_cycles(pce, 50);
    go(pce, 0xE772);
}

static void irq_cd_stream(Pce *pce)
{
    A = alu_nz(pce, (uint8_t)(m(pce, 0x229E) | m(pce, 0x229F) | m(pce, 0x22A0)));
    if (!A) {
        hle_cycles(pce, 25);
        go(pce, 0xE7CC);
        return;
    }
    if (m(pce, 0x1800) & 0x80) {
        mw(pce, 0x22A4, 0);
        ad_stream_clear(pce);
        hle_cycles(pce, 70);
        go(pce, 0xE7CC);
        return;
    }
    bios_clear_command(pce);
    A = 0x08;
    mw(pce, 0x224C, A);
    mw(pce, 0x224D, m(pce, 0x22A1));
    mw(pce, 0x224E, m(pce, 0x22A2));
    mw(pce, 0x224F, m(pce, 0x22A3));
    X = 0x10;
    A = alu_nz(pce, (uint8_t)(m(pce, 0x229F) | m(pce, 0x22A0)));
    if (!A) {
        A = m(pce, 0x229E);
        alu_cmp(pce, A, 0x10);
        if (A < 0x10)
            X = A;
    }
    A = X;
    mw(pce, 0x229D, A);
    mw(pce, 0x2250, A);
    A = 0x01;
    mw(pce, 0x22A4, A);
    hle_cycles(pce, 200);
    call(pce, SCSI_SEND_NOW, 0xE7C7);
}

static void irq_cd_stream_sent(Pce *pce)
{
    bios_tsb(pce, 0x1802, 0x60);
    hle_cycles(pce, 9);
    go(pce, 0xE7CC);
}

/* E7CC: bit 6 = data ready (DMA to the ADPCM) */
static void irq_cd_data(Pce *pce)
{
    if (z(pce, 0xF2) & 0x40) {
        irq_ack(pce, 0x40);
        A = 0x02;
        mw(pce, 0x180B, A);
        hle_cycles(pce, 40);
    } else {
        hle_cycles(pce, 8);
    }
    go(pce, 0xE7DE);
}

/* E7DE: bit 5 = background command status ready */
static void irq_cd_status(Pce *pce)
{
    if (!(z(pce, 0xF2) & 0x20)) {
        hle_cycles(pce, 8);
        go(pce, 0xE846);
        return;
    }
    irq_ack(pce, 0x20);
    hle_cycles(pce, 30);
    go(pce, 0xE7EB);
}

static void irq_cd_status_read(Pce *pce)
{
    A = alu_nz(pce, m(pce, 0x22A4));
    if (A)
        mw(pce, 0x180B, 0);
    A = alu_nz(pce, m(pce, 0x1801));
    mw(pce, 0x227B, A);
    if (A) {
        mw(pce, 0x22A4, 0);
        ad_stream_clear(pce);
    }
    bios_tsb(pce, 0x1802, 0x80);
    hle_cycles(pce, 50);
    go(pce, 0xE806);
}

static void irq_cd_status_ack(Pce *pce)
{
    if (bios_tst(pce, 0x40, 0x1800)) {
        hle_cycles(pce, 12);
        go(pce, 0xE806);
        return;
    }
    bios_trb(pce, 0x1802, 0x80);
    hle_cycles(pce, 21);
    go(pce, 0xE811);
}

static void irq_cd_message(Pce *pce)
{
    A = alu_nz(pce, (uint8_t)(m(pce, 0x1800) & 0xF8));
    alu_cmp(pce, A, 0xF8);
    if (A != 0xF8) {
        hle_cycles(pce, 13);
        go(pce, 0xE811);
        return;
    }
    A = m(pce, 0x1801);
    bios_tsb(pce, 0x1802, 0x80);
    hle_cycles(pce, 34);
    go(pce, 0xE822);
}

static void irq_cd_message_ack(Pce *pce)
{
    if (bios_tst(pce, 0x40, 0x1800)) {
        hle_cycles(pce, 12);
        go(pce, 0xE822);
        return;
    }
    bios_trb(pce, 0x1802, 0x80);
    hle_cycles(pce, 21);
    go(pce, 0xE82D);
}

static void irq_cd_free(Pce *pce)
{
    if (bios_tst(pce, 0x80, 0x1800)) {
        hle_cycles(pce, 12);
        go(pce, 0xE82D);
        return;
    }
    A = alu_nz(pce, m(pce, 0x22A4));
    if (A) {
        mw(pce, 0x22A4, 0);
        ad_stream_advance(pce);
        mw(pce, 0x229D, 0);
        bios_tsb(pce, 0x1802, 0x04);
        hle_cycles(pce, 140);
    } else {
        hle_cycles(pce, 20);
    }
    go(pce, 0xE846);
}

/* E846: bit 4 = subchannel */
static void irq_cd_subcode(Pce *pce)
{
    if (z(pce, 0xF2) & 0x10) {
        irq_ack(pce, 0x10);
        A = m(pce, 0x1807);
        mw(pce, 0x227E, A);
        alu_setc(pce, 0);
        bios_tsb(pce, 0x1802, 0x10);
        hle_cycles(pce, 60);
    } else {
        hle_cycles(pce, 8);
    }
    go(pce, 0xE85B);
}

/* E85B: bit 3 = the ADPCM finished playing (stops; plays again if requested) */
static void irq_cd_adpcm_end(Pce *pce)
{
    if (!(z(pce, 0xF2) & 0x08)) {
        hle_cycles(pce, 8 + 7);
        hle_rts(pce);
        return;
    }
    bios_trb(pce, 0x1802, 0x0C);
    bios_trb(pce, ZP(0xF2), 0x08);
    bios_trb(pce, 0x180D, 0x60);
    ad_stream_clear(pce);
    pce->cpu.p &= (uint8_t)~HU_FLAG_I;
    hle_cycles(pce, 8 + 2 + 7 + 2 + 7 + 2 + 7 + 7 + 20 + 7 + 2);
    go(pce, 0xE870);
}

static void irq_cd_adpcm_repeat(Pce *pce)
{
    A = alu_nz(pce, m(pce, 0x22A5));
    if (!A) {
        hle_cycles(pce, 5 + 4 + 7);
        hle_rts(pce);
        return;
    }
    hle_cycles(pce, 7);
    go(pce, 0xE875);
}

static void irq_cd_adpcm_wait(Pce *pce)
{
    A = alu_nz(pce, (uint8_t)(m(pce, 0x180C) & 0x08));
    if (A) {
        hle_cycles(pce, 11);
        go(pce, 0xE875);
        return;
    }
    ad_start(pce);
    hle_cycles(pce, 11 + 7 + 150 + 7);
    hle_rts(pce);
}

static const uint16_t ad_trans_wait_ops[] = {0xF3ED, 0xF3EF, 0xF3F2, 0xF3F4, 0xF3F6, 0xF3F9, 0};

const HleRoutine hle_adpcm_routines[] = {
    {0xE033, "ad_trans", ad_trans,
     {{0xF3BC, ad_trans_start}, {0xF3E4, ad_trans_sent}, {0xF3E9, ad_trans_wait}, {0xF3FC, ad_trans_status},
      {0xF403, ad_trans_error}}},
    {0xE036, "ad_read", ad_read,
     {{0xF431, ad_read_dest}, {0xF43C, ad_read_ram}, {0xF43E, ad_read_ram_next}, {0xF467, ad_read_vram},
      {0xF469, ad_read_vram_next}, {0xF4C1, ad_read_mpr}, {0xF4C4, ad_read_mpr_next}}},
    {0xE03C, "ad_play", ad_play, {{0}}},
    {0xE03F, "ad_cplay", ad_cplay,
     {{0xF3AC, ad_trans_direct}, {0xF3BC, ad_trans_start}, {0xF3E4, ad_trans_sent}, {0xF3FC, ad_trans_status},
      {0xF403, ad_trans_error}, {0xF680, ad_cplay_sent}}},
    {0xE042, "ad_stop", ad_stop, {{0}}},
    {0xE045, "ad_stat", ad_stat, {{0}}},
    {0xE74F, "irq_cd", irq_cd,
     {{0xE757, irq_cd_return}, {0xE75B, irq_cd_body}, {0xE772, irq_cd_stream}, {0xE7C7, irq_cd_stream_sent},
      {0xE7CC, irq_cd_data}, {0xE7DE, irq_cd_status}, {0xE7EB, irq_cd_status_read}, {0xE806, irq_cd_status_ack}}},
    {0xE811, "irq_cd_2", irq_cd_message,
     {{0xE822, irq_cd_message_ack}, {0xE82D, irq_cd_free}, {0xE846, irq_cd_subcode}, {0xE85B, irq_cd_adpcm_end},
      {0xE870, irq_cd_adpcm_repeat}, {0xE875, irq_cd_adpcm_wait}}, 1},
    {AD_READ_BYTE, "ad_read_byte", ad_read_byte, {{0}}, 1},
    {0xF3E9, "ad_trans_wait", ad_trans_wait, {{0}}, 1, ad_trans_wait_ops},
    {0, NULL, NULL, {{0}}, 0},
};
