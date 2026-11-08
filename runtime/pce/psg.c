/*
 * PSG (sound). See psg.h.
 */
#include "pce/psg.h"

#include <math.h>
#include <string.h>

#define MASTER_CLOCK_RATE 21477273u
#define LFO_ZERO_DIVIDER 0x100
#define ANALYTIC_MAX_PERIOD 5 /* very short periods: use the wave average */
#define WAVE_SAMPLES 32
#define SAMPLE_SCALE 2
#define DAC_OFFSET 0 /* HuC6280 (the HuC6280A would use 31) */
#define OUTPUT_SCALE 0.5f

static const int16_t k_lfo_depth[4] = { 0, 1, 4, 16 };
static const uint8_t k_volume_scale[16] = {
    0x00, 0x03, 0x05, 0x07, 0x09, 0x0B, 0x0D, 0x0F,
    0x10, 0x13, 0x15, 0x17, 0x19, 0x1B, 0x1D, 0x1F,
};

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

/* ---- Volume ---- */

static void compute_volume_lut(Psg *p)
{
    /* 32 steps of 1.5 dB; the last one is silence. */
    double amplitude = 65535.0 / 6.0 / 32.0;
    double step = 48.0 / 32.0;
    for (int i = 0; i < 31; i++) {
        p->volume_lut[i] = (uint16_t)amplitude;
        amplitude /= pow(10.0, step / 20.0);
    }
    p->volume_lut[31] = 0;
}

static void update_channel_volume(Psg *p, int channel)
{
    PsgChannel *c = &p->ch[channel];
    int main_left = k_volume_scale[p->main_vol_left & 0x0F];
    int main_right = k_volume_scale[p->main_vol_right & 0x0F];
    int ch_left = k_volume_scale[c->vol_left & 0x0F];
    int ch_right = k_volume_scale[c->vol_right & 0x0F];
    int ch_volume = c->control & 0x1F;
    int left = imin(0x1F, (0x1F - main_left) + (0x1F - ch_left) + (0x1F - ch_volume));
    int right = imin(0x1F, (0x1F - main_right) + (0x1F - ch_right) + (0x1F - ch_volume));
    c->gain_left = p->volume_lut[left];
    c->gain_right = p->volume_lut[right];
}

/* ---- Samples ---- */

static int16_t scale_sample(uint8_t data, uint16_t gain)
{
    return (int16_t)((((int32_t)data * SAMPLE_SCALE) - DAC_OFFSET) * gain);
}

static int64_t divide_rounded(int64_t value, int64_t divisor)
{
    if (value >= 0)
        return (value + (divisor >> 1)) / divisor;
    return (value - (divisor >> 1)) / divisor;
}

static int16_t waveform_sample(const Psg *p, int channel, uint16_t frequency, uint16_t gain)
{
    const PsgChannel *c = &p->ch[channel];
    if (frequency <= ANALYTIC_MAX_PERIOD) {
        int32_t numerator = ((int32_t)p->wave_sum[channel] * SAMPLE_SCALE) - (DAC_OFFSET * WAVE_SAMPLES);
        return (int16_t)divide_rounded((int64_t)numerator * gain, WAVE_SAMPLES);
    }
    return scale_sample(c->wave_data[c->wave_index], gain);
}

static uint16_t advance_waveform(PsgChannel *c, int cycles)
{
    uint16_t frequency = c->frequency ? c->frequency : 0x1000;
    int counter = c->counter - cycles;
    if (counter <= 0) {
        int steps = 1 + ((-counter) / frequency);
        c->counter = counter + steps * frequency;
        c->wave_index = (uint8_t)((c->wave_index + steps) & 0x1F);
    } else {
        c->counter = counter;
    }
    return frequency;
}

/* ---- LFO ---- */

static uint16_t lfo_frequency(const Psg *p)
{
    return p->lfo_frequency ? p->lfo_frequency : LFO_ZERO_DIVIDER;
}

static int lfo_configured(const Psg *p)
{
    return p->lfo_enabled && p->ch[0].enabled && p->ch[1].enabled;
}

static int lfo_running(const Psg *p)
{
    return lfo_configured(p) && !(p->lfo_control & 0x80);
}

static uint16_t lfo_period(const Psg *p, uint16_t frequency, uint8_t data)
{
    int32_t modulation = ((int32_t)data - 16) * k_lfo_depth[p->lfo_control & 3];
    uint16_t period = (uint16_t)((uint32_t)((int32_t)frequency + modulation) & 0x0FFF);
    return period ? period : 0x1000;
}

static void update_waveform_output(PsgChannel *c, uint8_t data)
{
    if (c->enabled && !c->noise_enabled) {
        c->left_sample = scale_sample(data, c->gain_left);
        c->right_sample = scale_sample(data, c->gain_right);
    }
}

/* Processes the cycles accumulated since the last time, updating each channel's sample. */
/* Advances channel i by batch PSG cycles. */
static void advance_channel(Psg *p, int i, int batch, int configured)
{
    PsgChannel *c = &p->ch[i];
    PsgChannel *lfo_src = &p->ch[1];
    PsgChannel *lfo_dest = &p->ch[0];

    /* The noise generator (LFSR) of channels 5 and 6 always runs. */
    if (i >= 4) {
        int noise_new = c->noise_counter - batch;
        if (noise_new <= 0) {
            int steps = (int)(1 + ((uint32_t)(-noise_new) / c->noise_freq));
            c->noise_counter = (int32_t)(noise_new + steps * c->noise_freq);
            for (int s = 0; s < steps; s++) {
                uint32_t seed = c->noise_seed;
                uint32_t bit = (seed ^ (seed >> 1) ^ (seed >> 11) ^ (seed >> 12) ^ (seed >> 17)) & 1;
                c->noise_seed = (seed >> 1) | (bit << 17);
            }
        } else {
            c->noise_counter = noise_new;
        }
    }

    if (!c->enabled)
        return;
    if (c->noise_enabled && i >= 4) {
        if (!c->dda_enabled)
            advance_waveform(c, batch);
    } else if (c->dda_enabled) {
    } else if (configured && i == 0) {
        uint16_t freq = lfo_dest->frequency ? lfo_dest->frequency : 0x1000;
        freq = lfo_period(p, freq, lfo_src->wave_data[lfo_src->wave_index]);
        int dest_new = lfo_dest->counter - batch;
        if (dest_new <= 0) {
            int steps = 1 + ((-dest_new) / freq);
            lfo_dest->counter = dest_new + steps * freq;
            lfo_dest->wave_index = (uint8_t)((lfo_dest->wave_index + steps) & 0x1F);
        } else {
            lfo_dest->counter = dest_new;
        }
    } else if (i == 1 && (configured || (p->lfo_control & 0x80))) {
    } else {
        advance_waveform(c, batch);
    }
}

/* Output of channel i in the current state (left_sample/right_sample). */
static void channel_output(Psg *p, int i, int configured)
{
    PsgChannel *c = &p->ch[i];
    c->left_sample = 0;
    c->right_sample = 0;
    if (!c->enabled)
        return;

    int8_t data;
    if (c->noise_enabled && i >= 4) {
        data = (c->noise_seed & 1) ? 0x1F : 0;
    } else if (c->dda_enabled) {
        data = c->dda;
    } else if ((configured && i == 0) || (i == 1 && (configured || (p->lfo_control & 0x80)))) {
        data = (int8_t)c->wave_data[c->wave_index]; /* LFO destination (channel 1) or source (channel 2) */
    } else {
        uint16_t freq = c->frequency ? c->frequency : 0x1000;
        c->left_sample = waveform_sample(p, i, freq, c->gain_left);
        c->right_sample = waveform_sample(p, i, freq, c->gain_right);
        return;
    }
    c->left_sample = scale_sample((uint8_t)data, c->gain_left);
    c->right_sample = scale_sample((uint8_t)data, c->gain_right);
}

/* Chip filter: PSG cycles until the next change in the output of any channel (the sync slice in which no
 * channel changes; very fast waves, which already come out as the average, do not count). */
static int next_change(const Psg *p, int configured, int running)
{
    const PsgChannel *lfo_src = &p->ch[1];
    int n = 1 << 30;
    if (running && !lfo_src->dda_enabled)
        n = imax(1, lfo_src->counter);
    for (int i = 0; i < 6; i++) {
        const PsgChannel *c = &p->ch[i];
        if (!c->enabled || (!c->gain_left && !c->gain_right)) /* silent: the output does not change */
            continue;
        if (c->noise_enabled && i >= 4)
            n = imin(n, imax(1, c->noise_counter));
        else if (c->dda_enabled || (i == 1 && (configured || (p->lfo_control & 0x80))))
            continue;
        else if ((configured && i == 0) || (c->frequency ? c->frequency : 0x1000) > ANALYTIC_MAX_PERIOD)
            n = imin(n, imax(1, c->counter));
    }
    return n;
}

static void sync(Psg *p)
{
    int remaining = p->elapsed_cycles;
    p->elapsed_cycles = 0;
    PsgChannel *lfo_src = &p->ch[1];
    PsgChannel *lfo_dest = &p->ch[0];

    /* Chip filter: the current output (a register may have changed since the last sync) */
    if (p->filter && remaining > 0)
        for (int i = 0; i < 6; i++)
            channel_output(p, i, lfo_configured(p));

    while (remaining > 0) {
        int batch = remaining;
        int configured = lfo_configured(p);
        int running = lfo_running(p);

        if (running && !lfo_src->dda_enabled && !lfo_dest->dda_enabled) {
            int source_counter = imax(1, lfo_src->counter);
            int dest_counter = imax(1, lfo_dest->counter);
            int lfo_cycles = dest_counter < source_counter ? source_counter - 1 : dest_counter;
            batch = imin(batch, lfo_cycles);
        }
        if (p->filter) {
            batch = imin(batch, next_change(p, configured, running));
            for (int i = 0; i < 6; i++) {
                p->acc[i][0] += (int64_t)p->ch[i].left_sample * batch * 6;
                p->acc[i][1] += (int64_t)p->ch[i].right_sample * batch * 6;
            }
        }
        remaining -= batch;

        if (running && !lfo_src->dda_enabled) {
            uint16_t freq = lfo_src->frequency ? lfo_src->frequency : 0x1000;
            int32_t period = freq * lfo_frequency(p);
            int counter_new = lfo_src->counter - batch;
            if (counter_new <= 0) {
                int steps = 1 + ((-counter_new) / period);
                lfo_src->counter = counter_new + steps * period;
                lfo_src->wave_index = (uint8_t)((lfo_src->wave_index + steps) & 0x1F);
            } else {
                lfo_src->counter = counter_new;
            }
        }

        for (int i = 0; i < 6; i++) {
            advance_channel(p, i, batch, configured);
            channel_output(p, i, configured);
        }
    }
}

static int16_t filter_output(const Psg *p, int i, int side)
{
    if (p->quiet[i][side] >= PSG_FILTER_TAPS)
        return 0;
    const float *r = &p->ring[i][side][p->ring_pos + 1];
    float sum = 0.0f;
    for (int k = 0; k < PSG_FILTER_TAPS; k++)
        sum += r[k] * p->fir[k];
    return (int16_t)(sum < -32768.0f ? -32768.0f : sum > 32767.0f ? 32767.0f : sum);
}

static void take_sample(Psg *p)
{
    if (p->filter) {
        for (int i = 0; i < 6; i++) {
            p->ch[i].output[p->buffer_index + 0] = filter_output(p, i, 0);
            p->ch[i].output[p->buffer_index + 1] = filter_output(p, i, 1);
        }
    } else {
        sync(p);
        for (int i = 0; i < 6; i++) {
            p->ch[i].output[p->buffer_index + 0] = p->ch[i].left_sample;
            p->ch[i].output[p->buffer_index + 1] = p->ch[i].right_sample;
        }
    }
    p->sfx_mask[p->buffer_index / 2] = p->sfx_channels ? p->sfx_channels(p->sfx_ctx) : 0;
    p->buffer_index += 2;
    if (p->buffer_index >= PSG_BUFFER_SIZE)
        p->buffer_index = 0; /* should not happen: ~1470 values per frame */
    if (p->extra_sample)
        p->extra_sample(p->extra_ctx);
}

/* ---- Interface ---- */

void psg_reset(Psg *p)
{
    memset(p, 0, sizeof(*p));
    compute_volume_lut(p);
    /* Chip filter: 20 kHz low-pass at the 4 x 44100 rate (sinc with a Blackman window) */
    const double pi = 3.14159265358979323846, cut = 20000.0 / (PSG_SAMPLE_RATE * 4.0);
    double total = 0.0;
    for (int k = 0; k < PSG_FILTER_TAPS; k++) {
        double n = k - (PSG_FILTER_TAPS - 1) / 2.0, x = 2.0 * pi * cut * n;
        double w = 0.42 - 0.5 * cos(2.0 * pi * k / (PSG_FILTER_TAPS - 1)) + 0.08 * cos(4.0 * pi * k / (PSG_FILTER_TAPS - 1));
        p->fir[k] = (float)(sin(x) / x * w);
        total += p->fir[k];
    }
    for (int k = 0; k < PSG_FILTER_TAPS; k++)
        p->fir[k] = (float)(p->fir[k] / total);
    for (int i = 0; i < 6; i++) {
        p->ch[i].noise_freq = 0x1F << 6;
        p->ch[i].noise_seed = 1;
        update_channel_volume(p, i);
    }
}

void psg_write(Psg *p, uint16_t offset, uint8_t value)
{
    sync(p);
    PsgChannel *c = p->channel_select < 6 ? &p->ch[p->channel_select] : NULL;

    switch (offset & 0x0F) {
    case 0: /* channel select */
        p->channel_select = value & 7;
        break;
    case 1: /* main volume */
        p->main_vol = value;
        p->main_vol_left = (value >> 4) & 0x0F;
        p->main_vol_right = value & 0x0F;
        for (int i = 0; i < 6; i++)
            update_channel_volume(p, i);
        break;
    case 2:
        if (c)
            c->frequency = (uint16_t)((c->frequency & 0x0F00) | value);
        break;
    case 3:
        if (c)
            c->frequency = (uint16_t)((c->frequency & 0x00FF) | ((value & 0x0F) << 8));
        break;
    case 4: /* channel control: enable, DDA, volume */
        if (c) {
            int enabled = (value & 0x80) != 0;
            int dda_enabled = (value & 0x40) != 0;
            int dda_falling = c->dda_enabled && !dda_enabled;
            if (c->dda_enabled || dda_enabled)
                c->wave_index = 0;
            if (dda_falling)
                c->counter = c->frequency ? c->frequency : 0x1000;
            c->control = value;
            c->enabled = (uint8_t)enabled;
            c->dda_enabled = (int8_t)dda_enabled;
            update_channel_volume(p, p->channel_select);
            if (dda_falling)
                update_waveform_output(c, c->wave_data[0]);
        }
        break;
    case 5: /* channel left/right volume */
        if (c) {
            c->amplitude = value;
            c->vol_left = (value >> 4) & 0x0F;
            c->vol_right = value & 0x0F;
            update_channel_volume(p, p->channel_select);
        }
        break;
    case 6: /* wave data (or direct sample in DDA mode) */
        if (c) {
            uint8_t data = value & 0x1F;
            c->wave = data;
            if (c->control & 0x40) {
                c->dda = (int8_t)data;
            } else {
                p->wave_sum[p->channel_select] -= c->wave_data[c->wave_index] & 0x1F;
                c->wave_data[c->wave_index] = data;
                p->wave_sum[p->channel_select] += c->wave_data[c->wave_index];
                if (!c->enabled)
                    c->wave_index = (uint8_t)((c->wave_index + 1) & 0x1F);
                update_waveform_output(c, data);
            }
        }
        break;
    case 7: /* noise (channels 5 and 6 only) */
        if (p->channel_select > 3 && p->channel_select < 6) {
            c->noise_control = value;
            c->noise_enabled = (value & 0x80) != 0;
            uint8_t freq = value & 0x1F;
            c->noise_freq = freq == 0x1F ? 32u : (uint32_t)(((~freq) & 0x1F) << 6);
        }
        break;
    case 8:
        p->lfo_frequency = value;
        break;
    case 9:
        if (value & 0x80) {
            PsgChannel *src = &p->ch[1];
            uint16_t freq = src->frequency ? src->frequency : 0x1000;
            src->counter = freq * (p->lfo_enabled ? lfo_frequency(p) : 1);
            src->wave_index = 0;
            src->dda = (int8_t)src->wave_data[0];
        }
        p->lfo_control = value;
        p->lfo_enabled = value & 3;
        break;
    default:
        break;
    }
}

static void clock_sources(Psg *p, uint32_t cycles)
{
    uint32_t total = p->cycle_counter + cycles;
    p->elapsed_cycles += (int32_t)(total / 6);
    p->cycle_counter = total % 6;
    if (p->extra_clock)
        p->extra_clock(p->extra_ctx, cycles);
}

/* Chip filter: closes the current slice (exact integral up to this main clock cycle: the part of the PSG
 * cycle in progress enters with the current output and is subtracted from the next slice). */
static void close_piece(Psg *p)
{
    sync(p);
    int configured = lfo_configured(p);
    int rest = (int)p->cycle_counter;
    float inv = p->sub_master ? 1.0f / (float)p->sub_master : 0.0f;
    p->ring_pos = (p->ring_pos + 1) % PSG_FILTER_TAPS;
    for (int i = 0; i < 6; i++) {
        channel_output(p, i, configured);
        int16_t v[2] = { p->ch[i].left_sample, p->ch[i].right_sample };
        for (int side = 0; side < 2; side++) {
            int64_t part = (int64_t)v[side] * rest;
            float value = (float)(p->acc[i][side] + part) * inv;
            p->acc[i][side] = -part;
            if (value != 0.0f)
                p->quiet[i][side] = 0;
            else if (p->quiet[i][side] < PSG_FILTER_TAPS)
                p->quiet[i][side]++;
            p->ring[i][side][p->ring_pos] = value;
            p->ring[i][side][p->ring_pos + PSG_FILTER_TAPS] = value;
        }
    }
    p->sub_master = 0;
}

static void clock_filtered(Psg *p, uint32_t cycles)
{
    const uint32_t rate = PSG_SAMPLE_RATE * 4;
    while (cycles > 0) {
        uint64_t to_piece = (MASTER_CLOCK_RATE - p->sub_clock_counter + rate - 1) / rate;
        uint32_t step = to_piece > 0 && to_piece < cycles ? (uint32_t)to_piece : cycles;
        clock_sources(p, step);
        p->sub_master += step;
        p->sub_clock_counter += (uint64_t)step * rate;
        cycles -= step;
        while (p->sub_clock_counter >= MASTER_CLOCK_RATE) {
            p->sub_clock_counter -= MASTER_CLOCK_RATE;
            close_piece(p);
            if (++p->sub_index == 4) {
                p->sub_index = 0;
                take_sample(p);
            }
        }
    }
}

void psg_set_filter(Psg *p, int on)
{
    on = on != 0;
    if (on == p->filter)
        return;
    sync(p);
    /* Same sample instants: the 4 x 44100 counter is 4 times the 44100 one */
    if (on) {
        uint64_t c = p->sample_clock_counter * 4;
        p->sub_index = (uint32_t)(c / MASTER_CLOCK_RATE);
        p->sub_clock_counter = c % MASTER_CLOCK_RATE;
        p->sub_master = (uint32_t)(p->sub_clock_counter / (PSG_SAMPLE_RATE * 4));
        memset(p->acc, 0, sizeof(p->acc));
        memset(p->ring, 0, sizeof(p->ring));
        memset(p->quiet, PSG_FILTER_TAPS, sizeof(p->quiet));
    } else {
        p->sample_clock_counter = ((uint64_t)p->sub_index * MASTER_CLOCK_RATE + p->sub_clock_counter) / 4;
    }
    p->filter = (uint8_t)on;
}

void psg_clock(Psg *p, uint32_t cycles)
{
    if (cycles == 0)
        return;
    if (p->filter) {
        clock_filtered(p, cycles);
        return;
    }
    uint64_t counter = p->sample_clock_counter + (uint64_t)cycles * PSG_SAMPLE_RATE;
    if (counter < MASTER_CLOCK_RATE) {
        clock_sources(p, cycles);
        p->sample_clock_counter = counter;
        return;
    }
    while (cycles > 0) {
        uint32_t step = cycles;
        uint64_t to_sample = (MASTER_CLOCK_RATE - p->sample_clock_counter + PSG_SAMPLE_RATE - 1) / PSG_SAMPLE_RATE;
        if (to_sample > 0 && to_sample < step)
            step = (uint32_t)to_sample;
        clock_sources(p, step);
        p->sample_clock_counter += (uint64_t)step * PSG_SAMPLE_RATE;
        cycles -= step;
        while (p->sample_clock_counter >= MASTER_CLOCK_RATE) {
            p->sample_clock_counter -= MASTER_CLOCK_RATE;
            take_sample(p);
        }
    }
}

static int end_frame(Psg *p, int16_t *buf, const float *gains)
{
    sync(p);
    int samples = p->buffer_index;
    for (int s = 0; s < samples; s++) {
        int side = s & 1;
        float raw = 0.0f;
        if (gains) {
            uint8_t mask = p->sfx_mask[s / 2];
            for (int i = 0; i < 6; i++)
                raw += p->ch[i].output[s] * gains[(mask >> i) & 1];
        } else {
            for (int i = 0; i < 6; i++)
                raw += p->ch[i].output[s];
        }
        raw *= OUTPUT_SCALE;

        /* High-pass filter (removes the DC level), like the console's output capacitor. */
        const float hpf_r = 0.9985f;
        float out = raw - p->hpf_prev_input[side] + hpf_r * p->hpf_prev_output[side];
        p->hpf_prev_input[side] = raw;
        p->hpf_prev_output[side] = out;
        if (out < -32768.0f)
            out = -32768.0f;
        if (out > 32767.0f)
            out = 32767.0f;
        buf[s] = (int16_t)out;
    }
    p->buffer_index = 0;
    return samples;
}

int psg_end_frame(Psg *p, int16_t *buf)
{
    return end_frame(p, buf, NULL);
}

int psg_end_frame_split(Psg *p, int16_t *buf, float music_gain, float sfx_gain)
{
    const float gains[2] = { music_gain, sfx_gain };
    return end_frame(p, buf, gains);
}
