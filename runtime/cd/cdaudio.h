/*
 * CD audio (CD-DA): the drive plays audio tracks from the disc, 44100 Hz stereo.
 *
 * The program issues (SCSI commands, scsi.c): play from a sector (with head seek;
 * or just position, paused), set the stop sector and what to do on reaching it
 * (stop, repeat from the start, or stop and notify the program), pause, stop.
 * After the seek there is a measured delay of 224 ms until the sound starts.
 * The volume can fade out gradually (fade, register $180F, see cdrom.c).
 *
 * One sample is generated per output sample (44100 Hz), together with the PSG (psg.h).
 *
 * Behavior reference: reference/Geargrafx/src/cdrom_audio*.
 */
#ifndef PCE_CDAUDIO_H
#define PCE_CDAUDIO_H

#include <stdint.h>

struct CdRom;

/* States (also the READ SUBCODE Q response) */
typedef enum CdAudioState {
    CD_AUDIO_PLAYING = 0, CD_AUDIO_IDLE = 1, CD_AUDIO_PAUSED = 2, CD_AUDIO_STOPPED = 3
} CdAudioState;

typedef enum CdAudioStopEvent {
    CD_AUDIO_STOP_EVENT_STOP, CD_AUDIO_STOP_EVENT_LOOP, CD_AUDIO_STOP_EVENT_IRQ
} CdAudioStopEvent;

#define CDAUDIO_BUFFER_SIZE 2048 /* values per frame (samples x 2 channels), with headroom */

typedef struct CdAudio {
    struct CdRom *cd;
    CdAudioState state;
    uint32_t start_lba, stop_lba, current_lba;
    uint32_t seek_start_lba;
    uint32_t current_sample; /* sample within the sector (0..587) */
    CdAudioStopEvent stop_event;
    int32_t seek_cycles;
    int32_t playback_delay_cycles;
    int16_t left_sample, right_sample;
    int16_t sector[2352 / 2];
    uint32_t sector_lba;
    int sector_attempted, sector_valid;
    int16_t buffer[CDAUDIO_BUFFER_SIZE];
    int buffer_index;
    uint64_t played_samples; /* diagnostic: samples played from the disc */
    /* PC version of a game: pc_mute = the game routine that plays a track restored the volume with a fade on and the
     * old track still playing (flagged by the game's project, which knows the routine); lasts until the next music
     * command (~40 ms). pc_hide (from the mix, AudioMix.hide_cd_pop) = silence is output meanwhile. They change
     * nothing in the simulated console. */
    int pc_mute, pc_hide;
    int pc_mutes; /* how many times pc_mute was flagged (pce_trace --game-fixes: can last less than a frame) */
    /* PC version: the music plays with the options menu open (console paused, cdaudio_pc_play) and then
     * continues from where it stopped: the heard track stays pc_skip samples ahead of the console position (on
     * wrap-around with repeat, from the start of the segment; without repeat, silence after the end until the console
     * gets there). Reset on every music command. The heard sector goes into pc_sector (the console's stays in
     * sector). */
    uint64_t pc_skip;
    int16_t pc_sector[2352 / 2];
    uint32_t pc_sector_lba;
    int pc_sector_ok;
} CdAudio;

void cdaudio_reset(CdAudio *a);
void cdaudio_clock(CdAudio *a, uint32_t cycles);
void cdaudio_sample(CdAudio *a);
/* Copies the frame's samples to buf and returns how many values were written. */
int cdaudio_end_frame(CdAudio *a, int16_t *buf);
/* PC version: n stereo samples (2n values) of the music, with the console paused (options menu), at the current fade
 * volume; only advances pc_skip. */
void cdaudio_pc_play(CdAudio *a, int16_t *buf, int n);

void cdaudio_start(CdAudio *a, uint32_t lba, int pause);
void cdaudio_stop(CdAudio *a);
void cdaudio_pause(CdAudio *a);
void cdaudio_set_idle(CdAudio *a);
void cdaudio_set_stop_lba(CdAudio *a, uint32_t lba, CdAudioStopEvent event);
int cdaudio_is_seeking(const CdAudio *a);
CdAudioState cdaudio_subcode_state(const CdAudio *a);
uint32_t cdaudio_subcode_lba(const CdAudio *a);

#endif
