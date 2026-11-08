/*
 * CD-ROM² drive: $1800 registers, interrupts, save RAM (BRAM). See cdrom.h.
 */
#include "cd/cdrom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu/huc6280.h"

/* Contents of a "formatted" save RAM (header the BIOS recognizes) */
static const uint8_t BRAM_HEADER[8] = {'H', 'U', 'B', 'M', 0x00, 0xA0, 0x10, 0x80};

void cdrom_unimplemented(CdRom *cd, const char *what)
{
    if (!cd->unimplemented)
        fprintf(stderr, "CD: the program used something not yet implemented: %s\n", what);
    cd->unimplemented = 1;
}

#define MASTER_CLOCK_RATE 21477273u
#define SLOW_FADE_SECONDS 6.5
#define FAST_FADE_SECONDS 2.5

void cdrom_set_current_sector(CdRom *cd, uint32_t lba)
{
    uint32_t count = cd->image ? cd->image->sector_count : 0;
    if (count == 0)
        return;
    cd->scsi.current_sector = lba < count ? lba : count - 1;
}

/* ---- Volume fade ($180F) ---- */

int cdrom_fader_enabled(const CdRom *cd, int adpcm)
{
    return cd->fader_enabled && cd->fader_adpcm == (adpcm != 0);
}

double cdrom_fader_value(const CdRom *cd)
{
    uint64_t elapsed = *cd->master_cycles - cd->fader_start_cycles;
    if (elapsed >= cd->fader_cycles)
        return 0.0;
    double v = cd->fader_start_level - (double)elapsed / (double)cd->fader_cycles;
    return v > 0.0 ? v : 0.0;
}

static void write_fader(CdRom *cd, uint8_t value)
{
    if (getenv("PCE_CD_LOG"))
        fprintf(stderr, "[%.2f s] fade $180F = %02X\n", (double)*cd->master_cycles / MASTER_CLOCK_RATE, value);
    /* Fade already on: continues from the current volume (see cdrom.h). */
    cd->fader_start_level = (cd->fader_enabled && !cd->fader_geargrafx) ? cdrom_fader_value(cd) : 1.0;
    cd->fader = value;
    cd->fader_enabled = (value & 0x08) != 0;
    cd->fader_adpcm = (value & 0x02) != 0;
    cd->fader_fast = (value & 0x04) != 0;
    cd->fader_start_cycles = *cd->master_cycles;
    double seconds = cd->fader_fast ? FAST_FADE_SECONDS : SLOW_FADE_SECONDS;
    cd->fader_cycles = (uint64_t)(seconds * MASTER_CLOCK_RATE);
}

/* ---- Interrupts (IRQ2 line) ---- */

static void assert_irq2(CdRom *cd)
{
    if (cd->enabled_irqs & cd->active_irqs)
        cd->cpu->irq_request |= HU_IRQ2;
    else
        cd->cpu->irq_request &= (uint8_t)~HU_IRQ2;
}

void cdrom_set_irq(CdRom *cd, uint8_t irq)
{
    if (cd->active_irqs & irq)
        return;
    cd->active_irqs |= irq;
    assert_irq2(cd);
}

void cdrom_clear_irq(CdRom *cd, uint8_t irq)
{
    if ((cd->active_irqs & irq) == 0)
        return;
    cd->active_irqs &= (uint8_t)~irq;
    assert_irq2(cd);
}

/* ---- Drive ---- */

void cdrom_init(CdRom *cd, CdImage *image, HuC6280 *cpu, const uint64_t *master_cycles)
{
    memset(cd, 0, sizeof(*cd));
    cd->image = image;
    cd->cpu = cpu;
    cd->master_cycles = master_cycles;
    cd->scsi.cd = cd;
    cd->audio.cd = cd;
    adpcm_init(&cd->adpcm, cd);
}

void cdrom_reset(CdRom *cd)
{
    /* RAM zeroed (like gg_trace, which pins the randomized values at reset);
     * save RAM empty and formatted. */
    memset(cd->ram, 0, sizeof(cd->ram));
    memset(cd->bram, 0, sizeof(cd->bram));
    memcpy(cd->bram, BRAM_HEADER, sizeof(BRAM_HEADER));
    cd->bram_enabled = 1;
    cd->reset = 0;
    cd->active_irqs = 0;
    cd->enabled_irqs = 0;
    cd->cdaudio_sample_toggle = 0;
    cd->cdaudio_sample = 0;
    cd->cdaudio_sample_last_clock = 0;
    cd->fader = 0;
    cd->fader_enabled = cd->fader_adpcm = cd->fader_fast = 0;
    cd->fader_start_cycles = cd->fader_cycles = 0;
    cd->fader_start_level = 1.0;
    cd->unimplemented = 0;
    scsi_reset(&cd->scsi, 0);
    cdaudio_reset(&cd->audio);
    adpcm_reset(&cd->adpcm);
}

void cdrom_clock(CdRom *cd, uint32_t cycles)
{
    scsi_clock(&cd->scsi, cycles);
}

void cdrom_clock_audio(void *ctx, uint32_t cycles)
{
    CdRom *cd = (CdRom *)ctx;
    adpcm_clock(&cd->adpcm, cycles);
    cdaudio_clock(&cd->audio, cycles);
}

void cdrom_sample_audio(void *ctx)
{
    CdRom *cd = (CdRom *)ctx;
    adpcm_sample(&cd->adpcm);
    cdaudio_sample(&cd->audio);
}

void cdrom_mix_audio(CdRom *cd, int16_t *buf, int count, const AudioMix *mix)
{
    static int16_t adpcm[2048], cdda[CDAUDIO_BUFFER_SIZE];
    int n_adpcm = adpcm_end_frame(&cd->adpcm, adpcm);
    int n_cdda = cdaudio_end_frame(&cd->audio, cdda);
    cd->audio.pc_hide = mix && mix->on && mix->hide_cd_pop; /* takes effect from the next frame */
    cd->adpcm.pc_filter = mix && mix->on; /* voice without the hiss (adpcm.h) */
    audio_mix_frame(buf, count, adpcm, n_adpcm, cdda, n_cdda, mix);
}

uint8_t cdrom_read(CdRom *cd, uint16_t offset)
{
    uint16_t reg = offset & 0x3FF;
    switch (reg) {
    case 0x00:
        return scsi_get_status(&cd->scsi);
    case 0x01:
        return scsi_read_data(&cd->scsi);
    case 0x02:
        return (uint8_t)((cd->enabled_irqs & 0x7F) | (scsi_signal(&cd->scsi, SCSI_ACK) ? 0x80 : 0x00));
    case 0x03:
        /* Reading this register locks the save RAM. */
        cd->bram_enabled = 0;
        return (uint8_t)(cd->active_irqs | 0x10 | (cd->cdaudio_sample_toggle ? 0x00 : 0x02));
    case 0x04:
        return cd->reset;
    case 0x05:
        return (uint8_t)(cd->cdaudio_sample & 0xFF);
    case 0x06:
        return (uint8_t)((cd->cdaudio_sample >> 8) & 0xFF);
    case 0x07:
        return cd->bram_enabled ? 0x80 : 0x00;
    case 0x08: {
        uint8_t v = scsi_read_data(&cd->scsi);
        scsi_auto_ack(&cd->scsi);
        return v;
    }
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E:
        return adpcm_read(&cd->adpcm, reg);
    case 0x0F:
        return cd->fader;
    default:
        return 0xFF; /* includes $C0-$C3: no Super CD-ROM² signature */
    }
}

void cdrom_write(CdRom *cd, uint16_t offset, uint8_t value)
{
    uint16_t reg = offset & 0x3FF;
    switch (reg) {
    case 0x00:
        scsi_start_selection(&cd->scsi);
        break;
    case 0x01:
        scsi_write_data(&cd->scsi, value);
        break;
    case 0x02:
        if (value & 0x80)
            scsi_set_signal(&cd->scsi, SCSI_ACK);
        else
            scsi_clear_signal(&cd->scsi, SCSI_ACK);
        cd->enabled_irqs = value & 0x7F;
        assert_irq2(cd);
        break;
    case 0x04:
        cd->reset = value & 0x0F;
        if (value & 0x02) {
            scsi_set_signal(&cd->scsi, SCSI_RST);
            cd->active_irqs &= 0x8F;
            assert_irq2(cd);
        } else {
            scsi_clear_signal(&cd->scsi, SCSI_RST);
        }
        break;
    case 0x05: {
        /* Latches the current CD audio sample (alternates right/left). */
        uint64_t now = *cd->master_cycles;
        if (now - cd->cdaudio_sample_last_clock >= 700) {
            cd->cdaudio_sample_last_clock = now;
            cd->cdaudio_sample_toggle = !cd->cdaudio_sample_toggle;
            cd->cdaudio_sample = cd->cdaudio_sample_toggle ? cd->audio.right_sample : cd->audio.left_sample;
        }
        break;
    }
    case 0x07:
        cd->bram_enabled = (value & 0x80) != 0;
        break;
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E:
        adpcm_write(&cd->adpcm, reg, value);
        break;
    case 0x0F:
        write_fader(cd, value);
        break;
    default:
        break;
    }
}
