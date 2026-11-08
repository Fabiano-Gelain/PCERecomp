/*
 * C BIOS: video, system and controller routines (ex_*). See hle.h.
 *
 * BIOS variables these routines maintain (the game reads them too):
 *   $20F3  VDC enable bits: bit 2 line interrupt, 3 VBlank, 6 sprites, 7 background (applied
 *          by the VBlank interrupt); $20F4 increment/DMA mode (register 5, high byte);
 *   $20F5  bit 1 = don't wait for VBlank; $20F7 selected VDC register
 *   $2241  VBlank counter; $2242 MWR (register 9); $2243 DCR (register 15)
 *   $2244-$2246 screen mode, width and height (ex_scrmod)
 *   $2214/$2215 sprite table address in VRAM; $2216 next sprite; $2217-$221E sprite to write
 *   $2220-$2225 palettes to load (ex_colorcmd); $221F request; $2286-$228D copy mini-routine
 *   $2227 controllers that accept reset; $2228-$222C buttons, $222D-$2231 newly pressed, $2232-$2236 previous
 *   $2249-$224B random number (ex_rnd); $2200-$220B interrupt hooks; $2284 game reset address
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

/* Selects the VDC register and stores the selection in $20F7 (A keeps the number). */
static void vdc_select(Pce *pce, uint8_t reg)
{
    A = alu_nz(pce, reg);
    zw(pce, 0xF7, reg);
    mw(pce, 0x0000, reg);
}

/* Register 5 (control) with the enable bits from $20F3 (low byte) or the mode from $20F4 (high byte). */
static void write_cr_low(Pce *pce)
{
    vdc_select(pce, 0x05);
    A = alu_nz(pce, z(pce, 0xF3));
    mw(pce, 0x0002, A);
}

static void write_cr_high(Pce *pce)
{
    vdc_select(pce, 0x05);
    A = alu_nz(pce, z(pce, 0xF4));
    mw(pce, 0x0003, A);
}

/* ---- Screen mode ---- */

/* In the screen mode routines: with hle_ram_free, the time of each instruction is spent before the next
 * hardware access (hle_idle; STA/ST0-ST2 on the hardware spend their own), and each write to the VDC/VCE lands on
 * the cycle of the real BIOS. This matters: vertical timing registers written on a different line change the line
 * where the switch takes effect and the time of the VBlank. */

/* E290: MWR (register 9) = ($2242 & keep) | Y, up to the RTS */
static void write_mwr(Pce *pce, uint8_t keep)
{
    mw(pce, 0x2242, alu_nz(pce, (uint8_t)(m(pce, 0x2242) & keep)));
    mw(pce, 0x2242, alu_nz(pce, (uint8_t)(Y | m(pce, 0x2242))));
    hle_idle(pce, 5 + 5 + 2 + 5 + 5 + 2 + 4); /* AND, STA, TYA, ORA, STA, LDA #, STA $F7 */
    vdc_select(pce, 0x09);
    hle_idle(pce, 5); /* LDA $2242 */
    A = alu_nz(pce, m(pce, 0x2242));
    mw(pce, 0x0002, A);
    hle_idle(pce, 7);
}

/* E28B, up to the RTS */
static void dotmod(Pce *pce, uint8_t a)
{
    hle_ram_free(pce);
    Y = alu_nz(pce, (uint8_t)(a & 0x8F));
    hle_idle(pce, 2 + 2 + 2); /* AND, TAY, LDA #$70 */
    write_mwr(pce, 0x70);
}

/* ex_scrsiz: background map size (MWR bits 4-6) */
static void ex_scrsiz(Pce *pce)
{
    hle_ram_free(pce);
    uint8_t a = (uint8_t)(A & 0x07);
    for (int i = 0; i < 4; i++)
        a = alu_asl(pce, a);
    Y = alu_nz(pce, a);
    hle_idle(pce, 2 + 4 * 2 + 2 + 2 + 4); /* AND, ASL x4, TAY, LDA #$8F, BRA */
    write_mwr(pce, 0x8F);
    hle_rts(pce);
}

/* ex_dotmod: VRAM access width (MWR bits 0-3 and 7) */
static void ex_dotmod(Pce *pce)
{
    dotmod(pce, A);
    hle_rts(pce);
}

/* Vertical registers (12-14) and the VCE pixel clock, common to both ex_scrmod modes. */
/* E35A, up to the RTS */
static void scrmod_vertical(Pce *pce)
{
    hle_idle(pce, 2 + 4); /* LDA #, STA $F7 */
    vdc_select(pce, 0x0C);
    hle_st(pce, 2, 0x02);
    uint8_t a = m(pce, 0x2246);
    a = alu_asl(pce, alu_asl(pce, alu_asl(pce, a)));
    X = (uint8_t)(a - 1);
    a = (uint8_t)(alu_lsr(pce, a) ^ 0xFF);
    alu_setc(pce, 0);
    a = alu_adc(pce, a, 0x88);
    hle_idle(pce, 5 + 3 * 2 + 2 + 2 + 2 + 2 + 2 + 2); /* LDA, ASL x3, TAX, DEX, LSR, EOR, CLC, ADC */
    mw(pce, 0x0003, a);
    hle_idle(pce, 2 + 4);
    vdc_select(pce, 0x0D);
    mw(pce, 0x0002, X);
    hle_st(pce, 3, 0x00);
    hle_idle(pce, 2 + 4);
    vdc_select(pce, 0x0E);
    a = (uint8_t)(X + 2);
    a = alu_lsr(pce, a);
    a = alu_adc(pce, a, 0x7D);
    hle_idle(pce, 2 + 2 + 2 + 2 + 2); /* TXA, INC A x2, LSR, ADC */
    mw(pce, 0x0002, a);
    hle_st(pce, 3, 0x00);
    A = alu_nz(pce, (uint8_t)(m(pce, 0x2244) ^ 0x04));
    hle_idle(pce, 5 + 2); /* LDA, EOR */
    mw(pce, 0x0400, A);
    hle_idle(pce, 7);
}

/* ex_scrmod (E2B6, up to the RTS): screen mode. A = mode (bit 0: 7 MHz; bit 2: ...), X = width and Y = height in
 * 8-pixel characters. Out-of-range values: uses 32x30 and returns A = 1, C = 1. */
static void scrmod(Pce *pce)
{
    mw(pce, 0x2244, A);
    A = alu_nz(pce, (uint8_t)(A & 0x05));
    mw(pce, 0x2244, A);
    mw(pce, 0x2245, X);
    mw(pce, 0x2246, Y);
    uint8_t h = m(pce, 0x2246);
    int wide = m(pce, 0x2244) & 0x01;
    uint8_t w = m(pce, 0x2245);
    uint8_t wmin = wide ? 0x10 : 0x0A, wmax = wide ? 0x2D : 0x23;
    /* STA, AND, STA, STX, STY; the checks (LDA, CMP, Bcc...) up to the one that fails, or up to the BSR $E35A */
    int t = 5 + 2 + 5 + 5 + 5 + 5 + 2;
    if (h < 0x0A)
        t += 4;
    else if (t += 2 + 2, h >= 0x1F)
        t += 4;
    else if (t += 2 + 8 + (wide ? 4 : 2) + 5 + 2, w < wmin)
        t += 4;
    else
        t += 2 + 2 + (w >= wmax ? 4 : 2 + 8);
    hle_idle(pce, t);
    if (h < 0x0A || h >= 0x1F || w < wmin || w >= wmax) {
        A = 0;
        X = 0x20;
        Y = 0x1E;
        hle_idle(pce, 2 + 2 + 2 + 8); /* CLA, LDX, LDY, BSR */
        scrmod(pce);
        A = alu_nz(pce, 0x01);
        alu_setc(pce, 1);
        hle_idle(pce, 2 + 2 + 7); /* LDA #, SEC, RTS */
        return;
    }
    scrmod_vertical(pce);
    hle_idle(pce, 2 + 4);
    vdc_select(pce, 0x0A);
    hle_st(pce, 2, wide ? 0x03 : 0x02);
    uint8_t a = m(pce, 0x2245);
    X = (uint8_t)(a - 1);
    a = (uint8_t)(alu_lsr(pce, a) ^ 0xFF);
    alu_setc(pce, 0);
    a = alu_adc(pce, a, wide ? 0x19 : 0x13);
    hle_idle(pce, 5 + 2 + 2 + 2 + 2 + 2 + 2); /* LDA, TAX, DEX, LSR, EOR, CLC, ADC */
    mw(pce, 0x0003, a);
    alu_setc(pce, 0);
    a = alu_adc(pce, a, m(pce, 0x2245));
    Y = (uint8_t)(a ^ 0xFF);
    hle_idle(pce, 2 + 5 + 2 + 2 + 2 + 4); /* CLC, ADC, EOR, TAY, LDA #, STA $F7 */
    vdc_select(pce, 0x0B);
    mw(pce, 0x0002, X);
    alu_setc(pce, 0);
    a = alu_adc(pce, Y, wide ? 0x35 : 0x27);
    hle_idle(pce, 2 + 2 + 2); /* TYA, CLC, ADC */
    mw(pce, 0x0003, a);
    hle_idle(pce, 2 + 7); /* CLA or LDA #$0A, JSR $E28B */
    dotmod(pce, wide ? 0x0A : 0x00);
    alu_setc(pce, 0);
    hle_idle(pce, 2 + 7); /* CLC, RTS */
}

static void ex_scrmod(Pce *pce)
{
    hle_ram_free(pce);
    scrmod(pce);
    hle_rts(pce);
}

/* ex_imode/ex_vmode/ex_hmode: VRAM address increment mode (register 5, bits 11-12) */
static void ex_imode(Pce *pce)
{
    zw(pce, 0xF4, (uint8_t)(z(pce, 0xF4) & ~0x08)); /* RMB3 and RMB4, separate as in the BIOS */
    zw(pce, 0xF4, (uint8_t)(z(pce, 0xF4) & ~0x10));
    uint8_t a = alu_asl(pce, alu_asl(pce, alu_asl(pce, A)));
    a = alu_nz(pce, (uint8_t)(a & 0x18));
    zw(pce, 0xF4, alu_nz(pce, (uint8_t)(a | z(pce, 0xF4))));
    write_cr_high(pce);
    hle_cycles(pce, 67);
    hle_rts(pce);
}

static void ex_vmode(Pce *pce)
{
    zw(pce, 0xF4, (uint8_t)(z(pce, 0xF4) | 0x08));
    uint8_t a = alu_nz(pce, (uint8_t)(m(pce, 0x2242) & 0x30));
    if (a == 0) {
        zw(pce, 0xF4, (uint8_t)(z(pce, 0xF4) & ~0x10));
    } else {
        zw(pce, 0xF4, (uint8_t)(z(pce, 0xF4) | 0x10));
        if (alu_nz(pce, (uint8_t)(a & 0x01)) == 0)
            zw(pce, 0xF4, (uint8_t)(z(pce, 0xF4) & ~0x08));
    }
    write_cr_high(pce);
    hle_cycles(pce, 71);
    hle_rts(pce);
}

static void ex_hmode(Pce *pce)
{
    zw(pce, 0xF4, (uint8_t)(z(pce, 0xF4) & ~0x08)); /* RMB3 and RMB4, separate as in the BIOS */
    zw(pce, 0xF4, (uint8_t)(z(pce, 0xF4) & ~0x10));
    write_cr_high(pce);
    hle_cycles(pce, 47);
    hle_rts(pce);
}

/* ---- VBlank and VDC interrupts ---- */

/* ex_vsync: waits for the next VBlank (the $2241 counter to change), with interrupts enabled. One instruction
 * per pseudo-instruction, like the real BIOS: the VBlank interrupt lands in the middle of the loop, and the point
 * where it lands (after the CMP or after the BEQ) decides when the routine returns to the game. */
static void ex_vsync(Pce *pce)
{
    hle_instr(pce, 4); /* JMP $E3CE */
    pce->cpu.pc = 0xE3CE;
}

static void ex_vsync_bbs1(Pce *pce)
{
    hle_ram_free(pce);
    int set = (z(pce, 0xF5) & 0x02) != 0; /* the game handles VBlank itself: return directly */
    hle_instr(pce, set ? 8 : 6);
    pce->cpu.pc = set ? 0xE3DF : 0xE3D1;
}

static void ex_vsync_cli(Pce *pce)
{
    pce->cpu.p &= (uint8_t)~HU_FLAG_I;
    hle_instr(pce, 2);
    pce->cpu.pc = 0xE3D2;
}

static void ex_vsync_bbs3(Pce *pce)
{
    hle_ram_free(pce);
    int set = (z(pce, 0xF3) & 0x08) != 0;
    hle_instr(pce, set ? 8 : 6);
    pce->cpu.pc = set ? 0xE3D7 : 0xE3D5;
}

static void ex_vsync_bsr(Pce *pce)
{
    hle_instr(pce, 8);
    bios_call(pce, 0xE3E8, 0xE3D7);
}

/* E3E8: enables the VBlank interrupt in the VDC control register (SMB3 $F3, BRA, and writes register 5) */
static void ex_vsync_irq_on(Pce *pce)
{
    hle_ram_free(pce);
    zw(pce, 0xF3, (uint8_t)(z(pce, 0xF3) | 0x08));
    hle_idle(pce, 7 + 4 + 2 + 4); /* SMB3, BRA, LDA #, STA $F7 */
    A = 0x05;
    zw(pce, 0xF7, A);
    mw(pce, 0x0000, A);
    hle_idle(pce, 4); /* LDA $F3 */
    A = alu_nz(pce, z(pce, 0xF3));
    mw(pce, 0x0002, A);
    hle_idle(pce, 7); /* RTS (with hle_idle the VDC wait cycles are kept, as on the CPU) */
    hle_rts(pce);
}

static void ex_vsync_read(Pce *pce)
{
    hle_ram_free(pce);
    A = alu_nz(pce, m(pce, 0x2241));
    hle_instr(pce, 5);
    pce->cpu.pc = 0xE3DA;
}

static void ex_vsync_cmp(Pce *pce)
{
    hle_ram_free(pce);
    alu_cmp(pce, A, m(pce, 0x2241));
    hle_instr(pce, 5);
    pce->cpu.pc = 0xE3DD;
}

static void ex_vsync_beq(Pce *pce)
{
    int eq = (pce->cpu.p & HU_FLAG_Z) != 0;
    hle_instr(pce, eq ? 4 : 2);
    pce->cpu.pc = eq ? 0xE3DA : 0xE3DF;
}

static void ex_vsync_rts(Pce *pce)
{
    hle_instr(pce, 7);
    hle_rts(pce);
}

static void set_cr_bit(Pce *pce, uint8_t bit, int on)
{
    uint8_t v = z(pce, 0xF3);
    zw(pce, 0xF3, (uint8_t)(on ? (v | bit) : (v & ~bit)));
}

/* E3E0-E3FA: SMBn/RMBn $F3 (BRA, except at $E3EC) and register 5 (low byte), with each VDC write on the
 * BIOS cycle (see write_mwr) */
static void cr_low(Pce *pce, uint8_t bit, int on)
{
    hle_ram_free(pce);
    set_cr_bit(pce, bit, on);
    hle_idle(pce, 7 + (bit == 0x08 && !on ? 0 : 4) + 2 + 4); /* SMB/RMB, BRA, LDA #, STA $F7 */
    vdc_select(pce, 0x05);
    hle_idle(pce, 4); /* LDA $F3 */
    A = alu_nz(pce, z(pce, 0xF3));
    mw(pce, 0x0002, A);
    hle_idle(pce, 7);
    hle_rts(pce);
}

static void ex_rcron(Pce *pce) { cr_low(pce, 0x04, 1); }
static void ex_rcroff(Pce *pce) { cr_low(pce, 0x04, 0); }
static void ex_irqon(Pce *pce) { cr_low(pce, 0x08, 1); }
static void ex_irqoff(Pce *pce) { cr_low(pce, 0x08, 0); }
/* Background and sprites: just recorded; the VBlank interrupt applies them. */
static void ex_bgon(Pce *pce) { set_cr_bit(pce, 0x80, 1); hle_cycles(pce, 18); hle_rts(pce); }
static void ex_bgoff(Pce *pce) { set_cr_bit(pce, 0x80, 0); hle_cycles(pce, 18); hle_rts(pce); }
static void ex_spron(Pce *pce) { set_cr_bit(pce, 0x40, 1); hle_cycles(pce, 18); hle_rts(pce); }
static void ex_sproff(Pce *pce) { set_cr_bit(pce, 0x40, 0); hle_cycles(pce, 18); hle_rts(pce); }
static void ex_dspon(Pce *pce) { set_cr_bit(pce, 0xC0, 1); hle_cycles(pce, 25); hle_rts(pce); }
static void ex_dspoff(Pce *pce) { set_cr_bit(pce, 0xC0, 0); hle_cycles(pce, 25); hle_rts(pce); }

/* ---- DMA, sprites, VRAM addresses ---- */

static void ex_dmamod(Pce *pce)
{
    mw(pce, 0x2243, alu_nz(pce, (uint8_t)(A & 0x1F)));
    vdc_select(pce, 0x0F);
    A = alu_nz(pce, m(pce, 0x2243));
    mw(pce, 0x0002, A);
    hle_cycles(pce, 45);
    hle_rts(pce);
}

static void ex_sprdma(Pce *pce)
{
    hle_ram_free(pce); /* each VDC write on the BIOS cycle (see write_mwr) */
    hle_idle(pce, 2 + 4); /* LDA #, STA $F7 */
    vdc_select(pce, 0x13);
    hle_idle(pce, 5);
    mw(pce, 0x0002, alu_nz(pce, m(pce, 0x2214)));
    hle_idle(pce, 5);
    A = alu_nz(pce, m(pce, 0x2215));
    mw(pce, 0x0003, A);
    hle_idle(pce, 7);
    hle_rts(pce);
}

/* ex_satclr: clears the sprite table in VRAM (256 words) */
static void ex_satclr(Pce *pce)
{
    /* At the BIOS pace (LDA #; ...; CLX; ST1; ST2; DEX; BNE): the VRAM waits land as on the console */
    hle_idle(pce, 2);
    vdc_select(pce, 0x00);
    mw(pce, 0x0002, alu_nz(pce, m(pce, 0x2214)));
    mw(pce, 0x0003, alu_nz(pce, m(pce, 0x2215)));
    hle_idle(pce, 2);
    vdc_select(pce, 0x02);
    hle_idle(pce, 2);
    for (int i = 0; i < 256; i++) {
        hle_st(pce, 2, 0x00);
        hle_st(pce, 3, 0x00);
        hle_idle(pce, i < 255 ? 6 : 4);
    }
    X = alu_nz(pce, 0);
    mw(pce, 0x2216, 0x00);
    hle_cycles(pce, 4158);
    hle_rts(pce);
}

/* ex_sprput: writes the sprite $2217-$221E at table position $2216 and advances (up to 64 sprites) */
static void ex_sprput(Pce *pce)
{
    /* hle_idle: at the BIOS pace, so the VRAM writes land on the same cycle */
    uint8_t n = m(pce, 0x2216);
    alu_cmp(pce, n, 0x40);
    if (n >= 0x40) {
        A = n;
        hle_cycles(pce, 22);
        hle_rts(pce);
        return;
    }
    hle_idle(pce, 4 + 3 + 2); /* CMP, BCS, PHY, LDA # */
    hle_push(pce, Y);
    vdc_select(pce, 0x00);
    uint8_t a = m(pce, 0x2216);
    hle_idle(pce, 6); /* ASL, ASL, CLC */
    a = alu_asl(pce, alu_asl(pce, a));
    alu_setc(pce, 0);
    mw(pce, 0x0002, alu_adc(pce, a, m(pce, 0x2214)));
    a = m(pce, 0x2215);
    hle_idle(pce, 2); /* ADC # */
    mw(pce, 0x0003, alu_adc(pce, a, 0x00));
    hle_idle(pce, 2); /* LDA # */
    vdc_select(pce, 0x02);
    hle_idle(pce, 2); /* CLY */
    for (uint8_t y = 0; y < 8; y += 2) {
        mw(pce, 0x0002, m(pce, (uint16_t)(0x2217 + y)));
        hle_idle(pce, 2); /* INY */
        A = m(pce, (uint16_t)(0x2218 + y));
        mw(pce, 0x0003, A);
        hle_idle(pce, y < 6 ? 8 : 6); /* INY, CPY #, BNE */
    }
    alu_cmp(pce, 8, 8);
    mw(pce, 0x2216, (uint8_t)(m(pce, 0x2216) + 1));
    Y = alu_nz(pce, hle_pull(pce));
    hle_cycles(pce, 221);
    hle_rts(pce);
}

/* ex_setrcr / ex_setred / ex_setwrt: register 6 (interrupt line), 1 (VRAM read) or 0 (VRAM write)
 * = X:A; read and write leave register 2 (data) selected. */
/* Each VDC write on the BIOS cycle (see write_mwr) */
static void set_vdc_word(Pce *pce, uint8_t reg, int select_data)
{
    hle_ram_free(pce);
    uint8_t a = A;
    hle_push(pce, a);
    hle_idle(pce, 3 + 2 + 4); /* PHA, LDA #, STA $F7 */
    vdc_select(pce, reg);
    hle_idle(pce, 4); /* PLA */
    A = alu_nz(pce, hle_pull(pce));
    mw(pce, 0x0002, A);
    mw(pce, 0x0003, X);
    if (select_data) {
        hle_idle(pce, 2 + 4);
        vdc_select(pce, 0x02);
    }
    hle_idle(pce, 7); /* RTS */
    hle_rts(pce);
}

static void ex_setrcr(Pce *pce) { set_vdc_word(pce, 0x06, 0); }
static void ex_setred(Pce *pce) { set_vdc_word(pce, 0x01, 1); }
static void ex_setwrt(Pce *pce) { set_vdc_word(pce, 0x00, 1); }

/* ex_setdma: VRAM-to-VRAM DMA: source $20FA, destination $20FE, length $20FC (registers 16-18) */
static void ex_setdma(Pce *pce)
{
    hle_push(pce, z(pce, 0xF7));
    vdc_select(pce, 0x10);
    mw(pce, 0x0002, z(pce, 0xFA));
    mw(pce, 0x0003, z(pce, 0xFB));
    vdc_select(pce, 0x11);
    mw(pce, 0x0002, z(pce, 0xFE));
    mw(pce, 0x0003, z(pce, 0xFF));
    vdc_select(pce, 0x12);
    mw(pce, 0x0002, z(pce, 0xFC));
    mw(pce, 0x0003, z(pce, 0xFD));
    A = alu_nz(pce, hle_pull(pce));
    zw(pce, 0xF7, A);
    mw(pce, 0x0000, A);
    hle_cycles(pce, 130);
    hle_rts(pce);
}

/* ---- Numbers ---- */

/* ex_binbcd: A (0-99) to BCD; A >= 100: C = 1 and A is unchanged. $20EC keeps the units. */
static void ex_binbcd(Pce *pce)
{
    alu_cmp(pce, A, 100);
    if (A >= 100) {
        hle_cycles(pce, 17);
        hle_rts(pce);
        return;
    }
    hle_push(pce, X);
    uint8_t tens = (uint8_t)(A / 10), units = (uint8_t)(A % 10);
    zw(pce, 0xEC, units);
    A = alu_nz(pce, (uint8_t)((tens << 4) + units));
    X = alu_nz(pce, hle_pull(pce));
    alu_setc(pce, 0);
    hle_cycles(pce, 55 + 8 * tens);
    hle_rts(pce);
}

/* ex_bcdbin: A from BCD to binary; invalid digit: C = 1 and A is unchanged. $20EC = tens * 10. */
static void ex_bcdbin(Pce *pce)
{
    uint8_t in = A;
    hle_push(pce, X);
    int ok = 0;
    uint8_t hi = (uint8_t)(in & 0xF0);
    if (hi < 0xA0) {
        zw(pce, 0xEC, (uint8_t)((hi >> 1) + (hi >> 3)));
        if ((in & 0x0F) < 0x0A) {
            in = (uint8_t)((in & 0x0F) + z(pce, 0xEC));
            ok = 1;
        }
    }
    alu_setc(pce, !ok);
    A = in;
    X = alu_nz(pce, hle_pull(pce));
    hle_cycles(pce, 60);
    hle_rts(pce);
}

/* ex_rnd: pseudorandom number (shift register at $2249-$224A, seed $224B) */
static void ex_rnd(Pce *pce)
{
    uint8_t a = m(pce, 0x224A);
    a = alu_rol(pce, alu_rol(pce, alu_rol(pce, a)));
    a = alu_nz(pce, (uint8_t)(a ^ m(pce, 0x2249)));
    alu_ror(pce, a);
    uint8_t saved = pce->cpu.p;
    hle_push(pce, (uint8_t)(saved | HU_FLAG_B));
    mw(pce, 0x224A, (uint8_t)(m(pce, 0x224A) ^ m(pce, 0x2249)));
    mw(pce, 0x2249, (uint8_t)(m(pce, 0x224B) ^ m(pce, 0x2249)));
    pce->cpu.p = (uint8_t)(hle_pull(pce) & ~HU_FLAG_B);
    mw(pce, 0x2249, alu_rol(pce, m(pce, 0x2249)));
    mw(pce, 0x224A, alu_rol(pce, m(pce, 0x224A)));
    A = alu_nz(pce, (uint8_t)(m(pce, 0x224A) ^ m(pce, 0x2249)));
    hle_cycles(pce, 90);
    hle_rts(pce);
}

/* ---- System ---- */

/* ex_getver: BIOS version in X (major) and Y (minor): 2.01 */
static void ex_getver(Pce *pce)
{
    X = alu_nz(pce, 0x02);
    Y = alu_nz(pce, 0x01);
    hle_cycles(pce, 21);
    hle_rts(pce);
}

/* ex_setvec: hook A (0-5, at $2200 + 2A) or game reset address (A = 6, at $2284) = X:Y */
static void ex_setvec(Pce *pce)
{
    uint8_t p = pce->cpu.p;
    hle_push(pce, (uint8_t)(p | HU_FLAG_B));
    pce->cpu.p |= HU_FLAG_I;
    if (A == 6) {
        mw(pce, 0x2284, X);
        mw(pce, 0x2285, Y);
    } else {
        uint8_t a = A, x = X, y = Y;
        hle_push(pce, a);
        hle_push(pce, y);
        hle_push(pce, x);
        uint8_t i = (uint8_t)(a << 1);
        mw(pce, (uint16_t)(0x2200 + i), hle_pull(pce));
        mw(pce, (uint16_t)(0x2200 + i + 1), hle_pull(pce));
        hle_pull(pce);
        X = (uint8_t)(i + 1);
    }
    pce->cpu.p = (uint8_t)(hle_pull(pce) & ~HU_FLAG_B);
    hle_cycles(pce, A == 6 ? 38 : 63);
    hle_rts(pce);
}

/* ex_joysns: reads the 5 controllers (multitap): $2228+n buttons (directions in the high nibble), $2232+n the
 * previous ones, $222D+n the newly pressed ones. SELECT pressed while RUN is held on a controller enabled
 * in $2227 = game reset: $20E7 = $80 and jumps to ($2284). */
int bios_joysns(Pce *pce)
{
    static const uint8_t masks[5] = {0x01, 0x02, 0x04, 0x08, 0x10};
    mw(pce, 0x1000, 0x01);
    mw(pce, 0x1000, 0x03);
    for (uint8_t y = 0; y < 5; y++) {
        mw(pce, 0x1000, 0x01);
        hle_push(pce, 0x01); /* multitap delay (PHA/PLA) */
        hle_pull(pce);
        mw(pce, (uint16_t)(0x2232 + y), m(pce, (uint16_t)(0x2228 + y)));
        uint8_t hi = (uint8_t)(m(pce, 0x1000) << 4);
        mw(pce, (uint16_t)(0x2228 + y), hi);
        mw(pce, 0x1000, 0x00);
        hle_push(pce, hi);
        hle_pull(pce);
        uint8_t now = (uint8_t)~((m(pce, 0x1000) & 0x0F) | m(pce, (uint16_t)(0x2228 + y)));
        mw(pce, (uint16_t)(0x2228 + y), now);
        mw(pce, (uint16_t)(0x222D + y), (uint8_t)((now ^ m(pce, (uint16_t)(0x2232 + y))) & m(pce, (uint16_t)(0x2228 + y))));
        Y = y;
    }
    uint8_t a = 0;
    for (uint8_t y = 0; y < 5; y++) {
        a = alu_nz(pce, (uint8_t)(m(pce, 0x2227) & masks[y]));
        if (!a)
            continue;
        a = m(pce, (uint16_t)(0x222D + y));
        alu_cmp(pce, a, 0x04);
        if (a != 0x04)
            continue;
        a = m(pce, (uint16_t)(0x2228 + y));
        alu_cmp(pce, a, 0x0C);
        if (a != 0x0C)
            continue;
        A = alu_nz(pce, 0x80);
        zw(pce, 0xE7, 0x80);
        Y = y;
        pce->cpu.pc = hle_rd16(pce, 0x2284); /* game reset (the return address stays on the stack, as in the BIOS) */
        return 1;
    }
    A = a;
    Y = 5;
    alu_cmp(pce, 5, 5);
    return 0;
}

static void ex_joysns(Pce *pce)
{
    int reset = bios_joysns(pce);
    hle_cycles(pce, 700);
    if (!reset)
        hle_rts(pce);
}

/* ex_colorcmd: loads (A bit 0 = 0) or reads (bit 0 = 1) palettes requested in $221F: background from
 * ($2220) with $2222 blocks of 16 colors, sprites from ($2223) with $2225 blocks. The BIOS builds at
 * $2286 a block-copy mini-routine (TIA to the VCE or TAI from the VCE) and calls it; the RAM ends up the same. */
static void color_block(Pce *pce, uint16_t ptr_addr, uint8_t count, uint8_t vce_hi, int read)
{
    mw(pce, 0x228C, 0x00);
    uint16_t len = (uint16_t)(count << 5);
    uint8_t a = count;
    for (int i = 0; i < 4; i++)
        a = alu_asl(pce, a);
    a = alu_asl(pce, a);
    mw(pce, 0x228C, (uint8_t)(len >> 8));
    alu_setc(pce, 0); /* bit 7 of $228C before the last ROL (blocks < 128) */
    alu_nz(pce, (uint8_t)(len >> 8));
    mw(pce, 0x228B, (uint8_t)len);
    uint16_t dst_field = read ? 0x2289 : 0x2287;
    mw(pce, dst_field, m(pce, ptr_addr));
    mw(pce, (uint16_t)(dst_field + 1), m(pce, (uint16_t)(ptr_addr + 1)));
    hle_idle(pce, 34 + 17 + 2 * 6); /* STZ $228C ... STA $228B; TII de 2 bytes */
    mw(pce, 0x0402, 0x00);
    if (vce_hi) {
        A = alu_nz(pce, vce_hi);
        hle_idle(pce, 2); /* LDA #1 */
        mw(pce, 0x0403, vce_hi);
    } else {
        mw(pce, 0x0403, 0x00);
    }
    /* The mini-routine: TIA ram->$0404/$0405 (load) or TAI $0404/$0405->ram (read). */
    uint16_t ram = hle_rd16(pce, dst_field);
    /* JSR $2286 + the block copy as on the CPU (huc6280_exec.h, block_transfer): 14 setup cycles; per
     * byte, the access (with the VCE wait) and 6 cycles; 3 at the end; RTS */
    HuC6280 *c = &pce->cpu;
    hle_idle(pce, 7 + 14);
    c->in_block_transfer = 1;
    for (uint16_t i = 0; i < len; i++) {
        uint16_t port = (uint16_t)(0x0404 + (i & 1));
        uint16_t src = read ? port : (uint16_t)(ram + i), dst = read ? (uint16_t)(ram + i) : port;
        c->write(c->ctx, huc6280_phys(c, dst), c->read(c->ctx, huc6280_phys(c, src)));
        hle_idle(pce, 6);
    }
    c->in_block_transfer = 0;
    hle_idle(pce, 3 + 7);
}

/* E522. Memory costs no separate time: the timing is that of the BIOS instructions, in order; only
 * VCE accesses cost their own (plus the wait) at the moment they happen. */
void bios_colorcmd(Pce *pce)
{
    int ram_free = pce->hle->ram_free;
    pce->hle->ram_free = 1;
    uint8_t req = m(pce, 0x221F);
    A = alu_nz(pce, req);
    if (!req) {
        hle_idle(pce, 5 + 2 + 7); /* LDA, BNE, RTS */
        pce->hle->ram_free = ram_free;
        return;
    }
    int read = req & 0x01;
    if (read) {
        hle_idle(pce, 9 + 4 + 4 + 17 + 3 * 6 + 14); /* E522, E528, E52C, E592 (TII de 3 bytes) */
        mw(pce, 0x2286, 0xF3);
        mw(pce, 0x2287, 0x04);
        mw(pce, 0x2288, 0x04);
    } else {
        hle_idle(pce, 9 + 6 + 33); /* E522, E528, E52E */
        mw(pce, 0x2286, 0xE3);
        mw(pce, 0x2289, 0x04);
        mw(pce, 0x228A, 0x04);
    }
    mw(pce, 0x228D, 0x60);
    uint8_t n = m(pce, 0x2222);
    A = alu_nz(pce, n);
    if (n)
        color_block(pce, 0x2220, n, 0x00, read);
    else
        hle_idle(pce, 2);
    n = m(pce, 0x2225);
    A = alu_nz(pce, n);
    hle_idle(pce, 7); /* E566 */
    if (n)
        color_block(pce, 0x2223, n, 0x01, read);
    else
        hle_idle(pce, 2);
    mw(pce, 0x221F, 0x00);
    hle_idle(pce, 12); /* E58E, including the RTS */
    pce->hle->ram_free = ram_free;
}

static void ex_colorcmd(Pce *pce)
{
    bios_colorcmd(pce);
    hle_rts(pce);
}

/* ---- Table ---- */

#include "bios/routines_ex.inc"
