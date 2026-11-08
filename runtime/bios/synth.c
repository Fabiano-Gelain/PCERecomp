/*
 * Synthetic BIOS: the 256 KB image that stands in for the System Card when the player doesn't have the
 * BIOS file. It contains no code: the routines are the C ones (hle.h), called by address. Only the bytes
 * that something reads as data live here, all with our own values:
 *   - interrupt vectors ($FFF6-$FFFF) and the CD RAM bank ($FFF5 = $80), which the game reads;
 *   - $E8F7 = RTI (default interrupt hook);
 *   - the entry table $E000-$E0FF filled with "JMP to itself";
 *   - the sound driver tables (bank 2): note periods, tempos, envelopes and waveforms.
 *     Periods and tempos are computed (equal temperament, timer); envelopes and waves are our own designs,
 *     similar to the factory ones but not identical, so the chip's sound may differ slightly from the console;
 *   - bank $1F, $DFB6: the license text that the disc's boot program checks, copied from the disc itself
 *     at startup (bios_synth_copyright).
 */
#define _USE_MATH_DEFINES /* M_PI on MSVC */
#include "bios/synth.h"

#include <math.h>
#include <string.h>

#define BANK(b, addr) ((size_t)(b) * 0x2000 + ((addr) & 0x1FFF))

static void put16(uint8_t *rom, size_t at, uint16_t v)
{
    rom[at] = (uint8_t)v;
    rom[at + 1] = (uint8_t)(v >> 8);
}

/* Waveforms (32 samples from 0 to 31): 8 families, with shape variations within each one */
static uint8_t wave_sample(int w, int i)
{
    double t = i / 32.0;
    double v;
    switch (w % 8) {
    case 0: v = i < 16 ? 1.0 : 0.0; break;                                  /* square */
    case 1: v = 0.5 + 0.5 * sin(2 * M_PI * t); break;                      /* sine */
    case 2: v = t; break;                                                   /* sawtooth */
    case 3: v = t < 0.5 ? 2 * t : 2 - 2 * t; break;                         /* triangle */
    case 4: v = i < 8 ? 1.0 : 0.0; break;                                   /* 25% pulse */
    case 5: v = 0.5 + 0.35 * sin(2 * M_PI * t) + 0.15 * sin(6 * M_PI * t); break; /* "organ" */
    case 6: v = 0.5 + 0.5 * sin(2 * M_PI * t) * (t < 0.5 ? 1 : 0.4); break; /* skewed sine */
    default: v = i < 4 ? 1.0 : 0.0; break;                                  /* narrow pulse */
    }
    if ((w / 8) & 1) /* variations: an extra half turn */
        v = 0.5 + (v - 0.5) * (0.7 + 0.3 * cos(4 * M_PI * t));
    int s = (int)lround(1 + v * 30);
    return (uint8_t)(s < 0 ? 0 : s > 31 ? 31 : s);
}

void bios_synth_build(uint8_t *rom)
{
    memset(rom, 0xFF, BIOS_SYNTH_SIZE);

    /* Bank 0: entries, default hook, vectors */
    for (int a = 0xE000; a < 0xE100; a += 3) {
        rom[BANK(0, a)] = 0x4C;
        put16(rom, BANK(0, a + 1), (uint16_t)a);
    }
    rom[BANK(0, 0xE8F7)] = 0x40; /* RTI */
    rom[BANK(0, 0xFFF5)] = 0x80; /* first CD RAM bank */
    put16(rom, BANK(0, 0xFFF6), 0xE74F); /* IRQ2 (CD) */
    put16(rom, BANK(0, 0xFFF8), 0xE889); /* IRQ1 (VDC) */
    put16(rom, BANK(0, 0xFFFA), 0xE6CC); /* timer */
    put16(rom, BANK(0, 0xFFFC), 0xE8F7); /* NMI */
    put16(rom, BANK(0, 0xFFFE), 0xE0F3); /* reset */

    /* Bank 2: sound driver tables (bios/hle_psg.c) */
    static const uint8_t octave[9] = {0x28, 0x00, 0x18, 0x30, 0x48, 0x60, 0x78, 0x90, 0xA8};
    memcpy(&rom[BANK(2, 0xC4F6)], octave, sizeof(octave));
    memcpy(&rom[BANK(2, 0xCF05)], octave, sizeof(octave));
    /* Periods: from C in octave 1 (32.70 Hz) upward, PSG clock 3.58 MHz / 32 */
    for (int i = 0; i < 85; i++) {
        double f = 32.70319566 * pow(2.0, i / 12.0);
        put16(rom, BANK(2, 0xD71A + 2 * i), (uint16_t)lround(3580000.0 / 32.0 / f));
    }
    /* Tempos: timer reload (6991 Hz) for 96 steps per beat at t beats per minute */
    for (int t = 0x23; t < 0x100; t++)
        rom[BANK(2, 0xD7A1 + t)] = (uint8_t)lround(6991.0 * 60.0 / 96.0 / t);
    /* Envelopes: FB = release (16-bit rate), FC = initial level, then segments (steps, 16-bit
     * rate), FF = end. Maximum level $7C. */
    static const uint8_t envelopes[16][16] = {
        {0xFB, 0x00, 0xA0, 0xFC, 0x00, 0x7C, 0x00, 0x00, 0x00, 0xFF},             /* sustain */
        {0xFB, 0x00, 0xF8, 0xFC, 0x00, 0x7C, 0x10, 0x00, 0xFE, 0xFF},             /* slow decay */
        {0xFB, 0x00, 0xFC, 0xFC, 0x00, 0x7C, 0x10, 0x00, 0xFE, 0xFF},
        {0xFB, 0x00, 0xFE, 0xFC, 0x00, 0x7C, 0x10, 0x00, 0xFE, 0xFF},
        {0xFB, 0x00, 0xF8, 0xFC, 0x00, 0x7C, 0x00, 0x80, 0xFF, 0xFF},             /* very slow decay */
        {0xFB, 0x00, 0xF8, 0xFC, 0x00, 0x7C, 0x00, 0x00, 0xFF, 0xFF},
        {0xFB, 0x00, 0xF8, 0xFC, 0x00, 0x7C, 0x00, 0x00, 0xFE, 0xFF},
        {0xFB, 0x00, 0xFA, 0xFC, 0x00, 0x30, 0x02, 0x00, 0x24, 0x00, 0x80, 0xFF}, /* rise */
        {0xFB, 0x00, 0xFA, 0xFC, 0x00, 0x38, 0x03, 0x00, 0x18, 0x00, 0x80, 0xFF},
        {0xFB, 0x00, 0xFA, 0xFC, 0x00, 0x20, 0x01, 0x00, 0x5C, 0x03, 0x00, 0xFE, 0x00, 0x80, 0xFF},
        {0xFB, 0x00, 0xFC, 0xFC, 0x00, 0x08, 0x03, 0x00, 0x24, 0x04, 0x00, 0xFE, 0xFF},
        {0xFB, 0x00, 0xFC, 0xFC, 0x00, 0x7C, 0x00, 0xC0, 0xFF, 0xFF},
        {0xFB, 0x00, 0xFA, 0xFC, 0x00, 0x68, 0x00, 0x60, 0x00, 0xFF},
        {0xFB, 0x00, 0xFA, 0xFC, 0x00, 0x7C, 0x05, 0x00, 0xF4, 0x00, 0x40, 0xFF}, /* attack and decay */
        {0xFB, 0x00, 0xFA, 0xFC, 0x00, 0x7C, 0x09, 0x00, 0xF8, 0x00, 0x40, 0xFF},
        {0xFB, 0x00, 0xFE, 0xFC, 0x00, 0x7C, 0x06, 0x00, 0xFC, 0x00, 0x00, 0xFF},
    };
    uint16_t at = 0xD8C1;
    for (int i = 0; i < 16; i++) {
        put16(rom, BANK(2, 0xD8A1 + 2 * i), at);
        int n = 16; /* up to the last $FF (the rest of the row is padding) */
        while (n > 0 && envelopes[i][n - 1] != 0xFF)
            n--;
        memcpy(&rom[BANK(2, at)], envelopes[i], (size_t)n);
        at = (uint16_t)(at + n);
    }
    for (int w = 0; w < 45; w++)
        for (int i = 0; i < 32; i++)
            rom[BANK(2, 0xD9A6 + w * 32 + i)] = wave_sample(w, i);
}

void bios_synth_copyright(uint8_t *rom, const uint8_t *text, int len)
{
    memcpy(&rom[BANK(0x1F, 0xDFB6)], text, (size_t)len);
}
