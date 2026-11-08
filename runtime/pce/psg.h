/*
 * HuC6280 PSG (sound): 6 wave channels of 32 5-bit samples, with left/right
 * volume, DDA mode (direct sample), noise on channels 5 and 6 and LFO
 * (channel 2 modulates the frequency of channel 1).
 *
 * The PSG runs at 1/6 of the master clock (3.58 MHz). The output samples are generated
 * at 44100 Hz, interleaved stereo (left, right), and delivered every frame.
 *
 * Behavior reference: reference/Geargrafx/src/huc6280_psg*, audio*.
 */
#ifndef PCE_PSG_H
#define PCE_PSG_H

#include <stdint.h>

#define PSG_SAMPLE_RATE 44100
#define PSG_BUFFER_SIZE 2048 /* values per frame (samples x 2 channels), with headroom */
#define PSG_FILTER_TAPS 64   /* chip filter: coefficients, at the 4 x 44100 rate */

typedef struct PsgChannel {
    uint8_t enabled;
    uint16_t frequency;
    uint8_t control;
    uint8_t amplitude;
    uint8_t vol_left, vol_right;
    uint16_t gain_left, gain_right;
    uint8_t wave;
    uint8_t wave_index;
    uint8_t wave_data[32];
    uint8_t noise_control;
    uint8_t noise_enabled;
    uint32_t noise_freq;
    uint32_t noise_seed;
    int32_t noise_counter;
    int32_t counter;
    int8_t dda;
    int8_t dda_enabled;
    int16_t output[PSG_BUFFER_SIZE];
    int16_t left_sample, right_sample;
} PsgChannel;

typedef struct Psg {
    PsgChannel ch[6];
    uint8_t channel_select;
    uint8_t main_vol, main_vol_left, main_vol_right;
    uint8_t lfo_enabled;
    uint16_t lfo_frequency;
    uint8_t lfo_control;
    int32_t elapsed_cycles;
    int32_t buffer_index;
    uint16_t volume_lut[32];
    uint16_t wave_sum[6];
    float hpf_prev_input[2], hpf_prev_output[2];

    /* Sampling clock */
    uint64_t sample_clock_counter;
    uint32_t cycle_counter;

    /* Other sound sources (CD-ROM²: ADPCM and CD audio), advanced in the same time slices
     * and sampled at the same instants as the PSG, like the reference's Audio::ClockSources/SampleSources.
     * Set after psg_reset. */
    void (*extra_clock)(void *ctx, uint32_t master_cycles);
    void (*extra_sample)(void *ctx);
    void *extra_ctx;

    /* Music x effects separation (only for the PC version's mix; does not change the PSG): at each sample,
     * sfx_channels(sfx_ctx) says which channels carry the sound driver's effects program (bit i =
     * channel i), stored in sfx_mask. NULL = no separation (sfx_mask stays zeroed). */
    uint8_t (*sfx_channels)(void *ctx);
    void *sfx_ctx;
    uint8_t sfx_mask[PSG_BUFFER_SIZE / 2];

    /* Chip filter (PC version only; psg_set_filter). Off (the default), each sample is the value of the
     * wave at its instant, as in Geargrafx: harmonics above 22 kHz come back as hiss. On, each channel
     * is integrated exactly in slices of 1/(4 x 44100) s and goes through a low-pass filter (20 kHz)
     * before becoming 44100 Hz, as Mednafen does with its high rate. Does not change the PSG registers. */
    uint8_t filter;
    uint32_t sub_index;          /* slice within the sample (0-3) */
    uint64_t sub_clock_counter;  /* like sample_clock_counter, at the 4 x 44100 rate */
    uint32_t sub_master;         /* main clock cycles in the current slice */
    int64_t acc[6][2];           /* integral of the slice (value x main clock cycles) */
    float ring[6][2][2 * PSG_FILTER_TAPS]; /* last slices (each written twice: wrap-free reads) */
    int ring_pos;
    uint8_t quiet[6][2];         /* consecutive zero slices (with PSG_FILTER_TAPS, the output is zero: skips the math) */
    float fir[PSG_FILTER_TAPS];
} Psg;

void psg_reset(Psg *psg);
void psg_write(Psg *psg, uint16_t offset, uint8_t value);
/* Advances N master clock cycles (generates samples when it is time). */
void psg_clock(Psg *psg, uint32_t master_cycles);
/* End of frame: copies the frame's samples to buf (interleaved stereo) and returns
 * how many values were written (2 per sample). */
int psg_end_frame(Psg *psg, int16_t *buf);
/* The same, with separate volumes for the music channels and the effects channels (sfx_mask). */
int psg_end_frame_split(Psg *psg, int16_t *buf, float music_gain, float sfx_gain);
/* Turns the chip filter on/off (see Psg.filter); switch at the end of a frame. */
void psg_set_filter(Psg *psg, int on);

#endif
