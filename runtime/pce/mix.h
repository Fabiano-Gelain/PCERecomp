/*
 * Mix of the frame's sound: chip (PSG), ADPCM voices and CD music, each with its own volume.
 *
 * Without gains (AudioMix zeroed, the default), the mix is Geargrafx's: a plain sum, changing nothing -
 * this is what the traces and the sound tests against Geargrafx use. The PC version turns the gains on
 * with the Mednafen balance (which has measurements from the real hardware) times the player's volumes.
 *
 * Mednafen balance in CD games (reference/mednafen_src: pce.cpp SetCDSettings, pce_psg.cpp
 * SetVolume, pcecd.cpp PCECD_SetSettings, scsicd.cpp RunCDDA), relative to the Geargrafx level:
 *   chip:   Mednafen lowers the PSG to 0.678 in CD games; without that its level is the same as
 *           Geargrafx's (channel at maximum: 8/6 * 62 * 128 = 10581 versus 62 * 341 * 0.5 = 10571)
 *   music:  CDDABaseVolume 0.5 of the disc sample (Geargrafx uses the whole sample)
 *   ADPCM:  ADPCMBaseVolume 0.42735 * 16 = 6.84 per decoder step (Geargrafx uses 10)
 * Relative to the chip, the music ends up at 0.5 / 0.678 = 74% and the ADPCM practically equal to Geargrafx.
 *
 * The chip plays music and effects at the same time: the BIOS sound driver has one program for each, and
 * at each sample the PSG records which channels carry the effects (Psg.sfx_mask, read from the driver's
 * memory by pce.c). Player volumes: CD music, chip music (music channels), effects (effect
 * channels), voices (ADPCM) and master.
 */
#ifndef PCE_MIX_H
#define PCE_MIX_H

#include <stdint.h>

#define MIX_MEDNAFEN_PSG 0.678f
#define MIX_MEDNAFEN_CDDA 0.5f
#define MIX_MEDNAFEN_ADPCM (0.42735f * 16.0f / 10.0f)

typedef struct AudioMix {
    int on;    /* 0 = Geargrafx mix (gains ignored) */
    float psg_music, psg_sfx; /* chip channels with the music / with the effects (applied in the PSG) */
    float adpcm, cdda;
    int chip_filter; /* filtered chip sound (psg_set_filter) */
    int hide_cd_pop; /* mutes the old track when the game switches music with a fade on (cdaudio.h: pc_mute) */
} AudioMix;

/* Player volumes, in % (0-100) */
typedef struct AudioVolumes {
    int master, music, chip_music, effects, voices; /* music = CD music */
    int chip_filter; /* chip filter (psg.h: Psg.filter), 0 or 1 */
} AudioVolumes;

/* Mednafen balance times the volumes. */
void audio_mix_mednafen(AudioMix *mix, const AudioVolumes *vol);

/* buf (chip, interleaved stereo, already with its volumes) += adpcm + cdda, with the mix gains (NULL or
 * off = plain sum). Sources shorter than count are treated as silence at the end. */
void audio_mix_frame(int16_t *buf, int count, const int16_t *adpcm, int n_adpcm, const int16_t *cdda, int n_cdda,
                     const AudioMix *mix);

#endif
