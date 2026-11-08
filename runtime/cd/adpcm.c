/*
 * CD-ROM² ADPCM. See adpcm.h.
 */
#include "cd/adpcm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cd/cdrom.h"

#define MASTER_CLOCK_RATE 21477273u
#define ADPCM_CLOCK_SPEED 32100.0f /* MSM5205 clock in Hz (default value from the reference) */

static const int8_t INDEX_SHIFT[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static int32_t cycles_per_sample(uint8_t rate)
{
    double frequency = (double)ADPCM_CLOCK_SPEED / (16.0 - (double)rate);
    return (int32_t)(uint32_t)(((double)MASTER_CLOCK_RATE * 65536.0 / frequency) + 0.5);
}

/* Wait until the next read slice (3) or write slice (1 or 2) of the 36-cycle period. */
static uint8_t compute_latency(int offset, int read)
{
    for (int d = 1; d <= 36; d++) {
        int slot = ((offset + d) / 9) & 3; /* 0 = refresh, 1/2 = write, 3 = read */
        if (read ? slot == 3 : (slot == 1 || slot == 2))
            return (uint8_t)d;
    }
    return 36;
}

static uint32_t next_slot_cycles(Adpcm *a, int read)
{
    uint8_t offset = (uint8_t)(*a->cd->master_cycles % 36);
    return read ? a->read_latency[offset] : a->write_latency[offset];
}

static void set_end_irq(Adpcm *a, int asserted)
{
    if (asserted)
        cdrom_set_irq(a->cd, CD_IRQ_ADPCM_END);
    else
        cdrom_clear_irq(a->cd, CD_IRQ_ADPCM_END);
    a->end_irq = asserted;
}

static void set_half_irq(Adpcm *a, int asserted)
{
    if (asserted)
        cdrom_set_irq(a->cd, CD_IRQ_ADPCM_HALF);
    else
        cdrom_clear_irq(a->cd, CD_IRQ_ADPCM_HALF);
    a->half_irq = asserted;
}

void adpcm_init(Adpcm *a, struct CdRom *cd)
{
    memset(a, 0, sizeof(*a));
    a->cd = cd;
    for (int step = 0; step < 49; step++) {
        /* In float precision, as in the reference (C++ picks the float overloads of pow/floor). */
        int value = (int)floorf(16.0f * powf(11.0f / 10.0f, (float)step));
        for (int nibble = 0; nibble < 8; nibble++)
            a->step_delta[(step << 3) + nibble] = (int16_t)((value / 8) + ((nibble & 1) ? value / 4 : 0) +
                                                            ((nibble & 2) ? value / 2 : 0) + ((nibble & 4) ? value : 0));
    }
    for (int i = 0; i < 36; i++) {
        a->read_latency[i] = compute_latency(i, 1);
        a->write_latency[i] = compute_latency(i, 0);
    }
    /* Mednafen filter: y = x + 1.9553 y1 - 0.9563 y2 at PCE_MASTER_CLOCK / 12 (gain removed at the end; it also
     * goes through a 1-pole filter at ~80 kHz, which doesn't matter). The same poles moved to 44100 Hz: z = r e^(+-jt)
     * becomes r^k e^(+-jkt), k = its rate / 44100; gain 1 at DC level. */
    const double fm = 21477272.7 / 12.0, a1 = 128143.0 / 65536.0, a2 = -62671.0 / 65536.0;
    double r = sqrt(-a2), t = acos(a1 / (2.0 * r)), k = fm / 44100.0;
    double rk = pow(r, k);
    a->lp_c1 = (float)(2.0 * rk * cos(t * k));
    a->lp_c2 = (float)(-rk * rk);
    a->lp_g = 1.0f - a->lp_c1 - a->lp_c2;
}

void adpcm_reset(Adpcm *a)
{
    a->read_value = a->write_value = 0;
    a->read_cycles = a->write_cycles = 0;
    a->read_address = a->write_address = a->address = 0;
    a->sample_rate = 0x0F;
    a->cycles_per_sample = cycles_per_sample(a->sample_rate);
    a->control = 0;
    a->dma = 0;
    a->dma_cycles = 0;
    a->end_irq = a->half_irq = 0;
    a->playing = a->play_pending = 0;
    a->nibble_toggle = 0;
    a->length = 0;
    a->sample = 2048;
    a->step_index = 0;
    a->cycle_counter = 0;
    a->buffer_index = 0;
    a->filter_state = a->dc_prev_x = a->dc_prev_y = 0.0f;
    a->area = 0;
    a->area_cycles = 0;
    a->lp_y1 = a->lp_y2 = 0.0f;
    a->gain_smooth = 1.0f;
    memset(a->ram, 0, sizeof(a->ram));
}

/* Control bit 7: resets the ADPCM while it is set. */
static void soft_reset(Adpcm *a)
{
    a->read_cycles = a->write_cycles = 0;
    a->read_address = a->write_address = a->address = 0;
    a->length = 0;
    a->nibble_toggle = 0;
    a->sample = 2048;
    a->step_index = 0;
    a->play_pending = 0;
    a->filter_state = a->dc_prev_x = a->dc_prev_y = 0.0f;
    a->area = 0;
    a->area_cycles = 0;
    a->lp_y1 = a->lp_y2 = 0.0f;
    a->gain_smooth = 1.0f;
    set_end_irq(a, 0);
    set_half_irq(a, 0);
}

static void check_reset(Adpcm *a)
{
    if (a->control & 0x80)
        soft_reset(a);
}

/* Control bit 4: the segment length becomes the current address. */
static void check_length(Adpcm *a)
{
    if (a->control & 0x10) {
        a->length = a->address;
        set_end_irq(a, 0);
    }
}

static void run_adpcm(Adpcm *a, uint32_t cycles)
{
    if (a->control & 0x80) {
        a->playing = (a->control & 0x20) != 0;
        a->play_pending = 0;
        return;
    }
    if (!a->playing && !a->play_pending)
        return;
    if (!(a->control & 0x20) || ((a->control & 0x40) && a->length == 0)) {
        a->play_pending = 0;
        a->playing = 0;
        return;
    }
    a->cycle_counter += (int32_t)(cycles << 16);
    if (a->cycle_counter < a->cycles_per_sample)
        return;
    a->cycle_counter -= a->cycles_per_sample;
    if (a->play_pending) {
        a->play_pending = 0;
        a->playing = 1;
        a->sample = 2048;
        a->step_index = 0;
    }
    uint8_t byte = a->ram[a->read_address];
    uint8_t nibble;
    a->nibble_toggle = !a->nibble_toggle;
    if (a->nibble_toggle) {
        nibble = (byte >> 4) & 0x0F;
    } else {
        nibble = byte & 0x0F;
        a->read_address++;
        a->length = (a->length - 1) & 0x1FFFF;
        set_half_irq(a, a->length <= 0x8000);
        if (a->length == 0)
            set_end_irq(a, 1);
    }
    int sign = (nibble & 0x08) ? -1 : 1;
    uint8_t value = nibble & 0x07;
    int16_t delta = (int16_t)(a->step_delta[(a->step_index << 3) + value] * sign);
    int next = a->sample + delta;
    if (a->pc_filter) /* clamps to 12 bits instead of wrapping around (adpcm.h) */
        a->sample = (int16_t)(next < 0 ? 0 : next > 0x0FFF ? 0x0FFF : next);
    else
        a->sample = (int16_t)(next & 0x0FFF);
    int index = a->step_index + INDEX_SHIFT[value];
    a->step_index = (uint8_t)(index < 0 ? 0 : index > 48 ? 48 : index);
}

static void update_read_write(Adpcm *a, uint32_t cycles)
{
    if (a->read_cycles > 0) {
        a->read_cycles -= (int32_t)cycles;
        if (a->read_cycles <= 0) {
            a->read_cycles = 0;
            a->read_value = a->ram[a->read_address++];
            if (!(a->control & 0x10)) {
                if (a->length > 0) {
                    a->length--;
                    set_half_irq(a, a->length < 0x8000);
                } else {
                    set_half_irq(a, 0);
                    set_end_irq(a, 1);
                }
            }
        }
    }
    if (a->write_cycles > 0) {
        a->write_cycles -= (int32_t)cycles;
        if (a->write_cycles <= 0) {
            a->write_cycles = 0;
            a->ram[a->write_address++] = a->write_value;
            set_half_irq(a, a->length < 0x8000);
            if (a->length == 0)
                set_end_irq(a, 1);
            if (!(a->control & 0x10))
                a->length = (a->length + 1) & 0x1FFFF;
        }
    }
}

static int scsi_data_ready(const Scsi *s)
{
    return s->data_size > 0 && s->data_offset < s->data_size;
}

/* DMA: copies the bytes the CD reader delivers straight into ADPCM RAM. */
static void update_dma(Adpcm *a, uint32_t cycles)
{
    Scsi *s = &a->cd->scsi;
    if ((a->dma & 0x03) == 0)
        return;
    if (a->dma_cycles > 0) {
        a->dma_cycles -= (int32_t)cycles;
        if (a->dma_cycles <= 0) {
            a->dma_cycles = 0;
            if (a->write_cycles == 0) {
                a->write_cycles = (int32_t)next_slot_cycles(a, 0);
                a->write_value = scsi_read_data(s);
                scsi_auto_ack(s);
                if (!scsi_data_ready(s))
                    a->dma &= (uint8_t)~0x01;
            } else {
                a->dma_cycles = 1;
            }
        }
        return;
    }
    if (!scsi_signal(s, SCSI_ACK) && !scsi_signal(s, SCSI_CD) && scsi_signal(s, SCSI_IO) && scsi_signal(s, SCSI_REQ))
        a->dma_cycles = 36;
}

void adpcm_clock(Adpcm *a, uint32_t cycles)
{
    /* Idle (no reset, not playing, no read/write/DMA in progress): nothing below changes. */
    if (!(a->control & 0x90) && !a->playing && !a->play_pending && a->read_cycles <= 0 && a->write_cycles <= 0 &&
        !(a->dma & 0x03)) {
        a->area += (int64_t)(a->sample - 2048) * cycles;
        a->area_cycles += cycles;
        return;
    }
    check_reset(a);
    check_length(a);
    int16_t before = a->sample;
    run_adpcm(a, cycles);
    /* New value since the threshold crossing (what is left in the counter), old value before it. */
    uint32_t after = 0;
    if (a->sample != before) {
        after = (uint32_t)(a->cycle_counter >> 16);
        if (after > cycles)
            after = cycles;
    }
    a->area += (int64_t)(before - 2048) * (cycles - after) + (int64_t)(a->sample - 2048) * after;
    a->area_cycles += cycles;
    update_read_write(a, cycles);
    update_dma(a, cycles);
    check_length(a);
    check_reset(a);
}

uint8_t adpcm_read(Adpcm *a, uint16_t reg)
{
    switch (reg) {
    case 0x0A:
        a->read_cycles = (int32_t)next_slot_cycles(a, 1);
        return a->read_value;
    case 0x0B:
        return a->dma;
    case 0x0C:
        return (uint8_t)((a->playing ? 0x08 : 0) | (a->end_irq ? 0x01 : 0) | (a->read_cycles > 0 ? 0x80 : 0) |
                         (a->write_cycles > 0 ? 0x04 : 0));
    case 0x0D:
        return a->control;
    case 0x0E:
        return a->sample_rate;
    default:
        return 0;
    }
}

static void write_control(Adpcm *a, uint8_t value)
{
    if ((value & 0x02) && !(a->control & 0x02))
        a->write_address = (uint16_t)(a->address - ((value & 0x01) ? 0 : 1));
    if ((value & 0x08) && !(a->control & 0x08))
        a->read_address = (uint16_t)(a->address - ((value & 0x04) ? 0 : 1));
    if ((value & 0x20) && !a->playing)
        a->play_pending = 1;
    a->control = value;
}

void adpcm_write(Adpcm *a, uint16_t reg, uint8_t value)
{
    switch (reg) {
    case 0x08:
        a->address = (uint16_t)((a->address & 0xFF00) | value);
        break;
    case 0x09:
        a->address = (uint16_t)((a->address & 0x00FF) | (value << 8));
        break;
    case 0x0A:
        a->write_cycles = (int32_t)next_slot_cycles(a, 0);
        a->write_value = value;
        break;
    case 0x0B:
        a->dma = scsi_data_ready(&a->cd->scsi) ? value : (uint8_t)(value & ~0x01);
        break;
    case 0x0D:
        write_control(a, value);
        break;
    case 0x0E:
        if (getenv("PCE_CD_LOG"))
            fprintf(stderr, "[%.2f s] ADPCM $180E = %02X (%.0f Hz)\n", (double)*a->cd->master_cycles / MASTER_CLOCK_RATE,
                    value, (double)ADPCM_CLOCK_SPEED / (16.0 - (double)(value & 0x0F)));
        a->sample_rate = value;
        a->cycles_per_sample = cycles_per_sample(value & 0x0F);
        break;
    default:
        break;
    }
}

void adpcm_sample(Adpcm *a)
{
    /* Float arithmetic, in the same order as the reference (Adpcm::Sample). */
    if (a->playing)
        a->played_samples++;
    float x = (float)((int)a->sample - 2048) * 10.0f;
    if (a->pc_filter && a->area_cycles)
        x = (float)((double)a->area / (double)a->area_cycles) * 10.0f;
    a->area = 0;
    a->area_cycles = 0;
    const float r = 0.997f;
    float y = x - a->dc_prev_x + r * a->dc_prev_y;
    a->dc_prev_x = x;
    a->dc_prev_y = y;
    if (a->pc_filter) {
        float out = a->lp_g * y + a->lp_c1 * a->lp_y1 + a->lp_c2 * a->lp_y2;
        a->lp_y2 = a->lp_y1;
        a->lp_y1 = out;
        a->filter_state = out;
    } else {
        const float alpha_lpf = 0.4f;
        a->filter_state += alpha_lpf * (y - a->filter_state);
    }
    float target_gain = cdrom_fader_enabled(a->cd, 1) ? (float)cdrom_fader_value(a->cd) : 1.0f;
    const float gain_smooth = 0.003f;
    a->gain_smooth += (target_gain - a->gain_smooth) * gain_smooth;
    int out = (int)(a->filter_state * a->gain_smooth);
    int16_t s = (int16_t)(out < -32768 ? -32768 : out > 32767 ? 32767 : out);
    a->buffer[a->buffer_index + 0] = s;
    a->buffer[a->buffer_index + 1] = s;
    a->buffer_index += 2;
    if (a->buffer_index >= (int)(sizeof(a->buffer) / sizeof(a->buffer[0])))
        a->buffer_index = 0; /* should not happen: ~1470 values per frame */
}

int adpcm_end_frame(Adpcm *a, int16_t *buf)
{
    int n = a->buffer_index;
    memcpy(buf, a->buffer, (size_t)n * sizeof(int16_t));
    a->buffer_index = 0;
    return n;
}
