/*
 * CD-ROM² ADPCM (MSM5205 + 64 KB of its own RAM): plays compressed voices and
 * effects (4 bits per sample). The CPU writes to ADPCM RAM byte by byte through
 * the registers, or by DMA straight from the CD reader; it then starts playback
 * of a segment. Interrupts: segment "half" and "end" (IRQ2 line, via cdrom.h).
 *
 * Registers ($1800 + n): $08/$09 address (low/high), $0A data (r/w, with wait),
 * $0B DMA, $0C status (r), $0D control, $0E sample rate.
 * ADPCM RAM is divided into slices of 9 master cycles (refresh, write, write,
 * read); a CPU access waits for its slice.
 *
 * The output goes through a DC-removal filter and a low-pass filter, as in the reference.
 *
 * Behavior reference: reference/Geargrafx/src/adpcm*.
 */
#ifndef PCE_ADPCM_H
#define PCE_ADPCM_H

#include <stdint.h>

struct CdRom;

typedef struct Adpcm {
    struct CdRom *cd;
    uint8_t ram[0x10000];
    int16_t step_delta[49 * 8];
    uint8_t read_latency[36], write_latency[36];
    uint8_t read_value, write_value;
    int32_t read_cycles, write_cycles;
    uint16_t read_address, write_address, address;
    uint8_t sample_rate;
    int32_t cycles_per_sample; /* in 1/65536 of a master cycle */
    uint8_t control;
    uint8_t dma;
    int32_t dma_cycles;
    int end_irq, half_irq;
    int playing, play_pending;
    int nibble_toggle;
    uint32_t length;
    int16_t sample; /* decoder output, 12 bits (centered at 2048) */
    uint8_t step_index;
    int32_t cycle_counter;

    /* Output (44100 Hz): removes the DC level, smooths and applies the fade */
    float filter_state, dc_prev_x, dc_prev_y, gain_smooth;
    /* PC version (pc_filter = PC mix enabled): instead of the decoder value at the instant of the sample
     * (Geargrafx: at 16 kHz in 44.1 kHz each value lasts 2 or 3 samples, irregularly, and that hisses), the
     * average of the value over the time since the previous sample (area / cycles) and Mednafen's 2-pole filter
     * (pcecd.cpp, PCECD_ProcessADPCMBuffer: poles at -6367 +- 6364j Hz) in place of the 1-pole one.
     * The decoder also clamps at 0 and 4095 instead of wrapping around (Geargrafx and Mednafen wrap): the voice
     * in Luta Gemma goes past the limit 18 times when wrapping (each one a pop from end to end) and only 4 times,
     * by up to 182, when clamping, i.e. it was made to be clamped. */
    int pc_filter;
    int64_t area;          /* sum of (value - 2048) x master cycles since the last output sample */
    uint32_t area_cycles;
    float lp_c1, lp_c2, lp_g, lp_y1, lp_y2;
    int16_t buffer[2048];
    int buffer_index;
    uint64_t played_samples; /* diagnostic: output samples while ADPCM is playing */
} Adpcm;

void adpcm_init(Adpcm *a, struct CdRom *cd);
void adpcm_reset(Adpcm *a);
void adpcm_clock(Adpcm *a, uint32_t cycles);
uint8_t adpcm_read(Adpcm *a, uint16_t reg);
void adpcm_write(Adpcm *a, uint16_t reg, uint8_t value);
/* One output sample (called at 44100 Hz) and the end of the frame (returns how many values, 2 per sample). */
void adpcm_sample(Adpcm *a);
int adpcm_end_frame(Adpcm *a, int16_t *buf);

#endif
