/*
 * CD-ROM² drive (the "interface" attached behind the console): 64 KB RAM,
 * save RAM (backup RAM), registers at $1800-$1BFF, SCSI controller
 * with the drive (scsi.h), CD audio (cdaudio.h) and ADPCM (adpcm.h).
 *
 * Physical map added to the console:
 *   $80-$87  CD RAM, 64 KB
 *   $F7      save RAM: 2 KB, only accessible once unlocked ($1807)
 * Registers ($1800 + n, mirrored every $400):
 *   $00 SCSI status (r) / start of selection (w)    $01 SCSI data (r/w)
 *   $02 enabled interrupts + ACK (r/w)              $03 active interrupts (r; locks the save RAM)
 *   $04 drive reset (r/w)                           $05/$06 CD audio sample (r; $05 w = latch)
 *   $07 save RAM unlocked (r/w)                     $08 SCSI data with automatic ACK (r)
 *   $08-$0E ADPCM                                   $0F fading volume (r/w)
 *   $C0-$C3 Super CD-ROM² signature (here: plain CD-ROM², reads $FF)
 * CD interrupts go out on the CPU's IRQ2 line.
 *
 * Fade ($180F): bit 3 enables, bit 1 selects ADPCM (1) or CD audio (0), bit 2 fast
 * (2.5 s) or slow (6.5 s); the volume falls in a straight line to zero.
 * Writing again with the fade already on: the volume continues from where it is (only turning the
 * fade off, bit 3 = 0, restores full volume). Mednafen does this (reference/mednafen_src,
 * pce/pcecd.cpp), and the behavior was confirmed in a game (the music stays silent after the fade).
 * Geargrafx restarts from full volume on every write, which makes the music come back for ~1 s;
 * fader_geargrafx = 1 imitates that, only for comparison tests against it.
 * Unresolved difference: the slow fade lasts 6.5 s in Geargrafx and 6.0 s in Mednafen (we use 6.5).
 *
 * Sound: ADPCM and CD audio advance and generate samples together with the PSG (psg.h calls
 * cdrom_clock_audio and cdrom_sample_audio); at the end of the frame the three are summed.
 *
 * Parts that the reference has but we have not done yet call cdrom_unimplemented: it sets
 * cd->unimplemented and prints the message once -- nothing is imitated without the reference.
 *
 * Behavior reference: reference/Geargrafx/src/cdrom*.cpp/h, memory.cpp (map, save RAM),
 * audio.cpp (mix).
 */
#ifndef PCE_CDROM_H
#define PCE_CDROM_H

#include <stdint.h>

#include "cd/adpcm.h"
#include "cd/cdaudio.h"
#include "cd/cdimage.h"
#include "cd/scsi.h"
#include "pce/mix.h"

struct HuC6280;

#define CD_RAM_SIZE 0x10000
#define CD_BRAM_SIZE 0x800

/* CD interrupts ($1802/$1803) */
#define CD_IRQ_ADPCM_HALF 0x04
#define CD_IRQ_ADPCM_END 0x08
#define CD_IRQ_STATUS_AND_MSG_IN 0x20
#define CD_IRQ_DATA_IN 0x40

typedef struct CdRom {
    CdImage *image;  /* disc in the reader (NULL = empty) */
    struct HuC6280 *cpu;
    const uint64_t *master_cycles; /* console clock, for the fade and the sample latch */

    uint8_t ram[CD_RAM_SIZE];
    uint8_t bram[CD_BRAM_SIZE];
    int bram_enabled;

    uint8_t reset;
    uint8_t active_irqs, enabled_irqs;
    int cdaudio_sample_toggle;
    int16_t cdaudio_sample;
    uint64_t cdaudio_sample_last_clock;

    uint8_t fader;
    int fader_enabled, fader_adpcm, fader_fast;
    uint64_t fader_start_cycles, fader_cycles;
    double fader_start_level;   /* volume (0..1) at the time of the last write to $180F */
    int fader_geargrafx;        /* 1 = re-enabling the fade restarts at full volume (only to compare with Geargrafx) */

    Scsi scsi;
    CdAudio audio;
    Adpcm adpcm;

    int unimplemented; /* the program used something not done yet (see above) */
} CdRom;

/* Powers on the drive (image may be NULL). */
void cdrom_init(CdRom *cd, CdImage *image, struct HuC6280 *cpu, const uint64_t *master_cycles);
void cdrom_reset(CdRom *cd);
void cdrom_clock(CdRom *cd, uint32_t cycles);
uint8_t cdrom_read(CdRom *cd, uint16_t offset);
void cdrom_write(CdRom *cd, uint16_t offset, uint8_t value);

/* Sound: called by the PSG (psg.h), in the same time slices and samples as it. */
void cdrom_clock_audio(void *cd, uint32_t cycles);
void cdrom_sample_audio(void *cd);
/* End of frame: adds ADPCM and CD audio to the PSG sound (count values in buf), with the volumes of
 * mix (NULL = Geargrafx mix; see pce/mix.h). */
void cdrom_mix_audio(CdRom *cd, int16_t *buf, int count, const AudioMix *mix);

/* Used by the SCSI controller, ADPCM and CD audio */
void cdrom_set_irq(CdRom *cd, uint8_t irq);
void cdrom_clear_irq(CdRom *cd, uint8_t irq);
void cdrom_unimplemented(CdRom *cd, const char *what);
/* Read head position (clamped to the last sector of the disc). */
void cdrom_set_current_sector(CdRom *cd, uint32_t lba);
int cdrom_fader_enabled(const CdRom *cd, int adpcm);
double cdrom_fader_value(const CdRom *cd);

#endif
