/*
 * CD audio. See cdaudio.h.
 */
#include "cd/cdaudio.h"

#include <string.h>

#include "cd/cdrom.h"

#define PLAYBACK_DELAY_US 224000 /* seek done to sound (measured by DShadoff, per the reference) */

static uint32_t sector_count(const CdAudio *a)
{
    return a->cd->image ? a->cd->image->sector_count : 0;
}

void cdaudio_reset(CdAudio *a)
{
    struct CdRom *cd = a->cd;
    memset(a, 0, sizeof(*a));
    a->cd = cd;
    a->state = CD_AUDIO_IDLE;
    a->stop_event = CD_AUDIO_STOP_EVENT_STOP;
}

void cdaudio_clock(CdAudio *a, uint32_t cycles)
{
    uint32_t remaining = cycles;
    if (a->seek_cycles > 0) {
        if (remaining >= (uint32_t)a->seek_cycles) {
            remaining -= (uint32_t)a->seek_cycles;
            a->seek_cycles = 0;
            a->seek_start_lba = a->current_lba;
            scsi_audio_seek_completed(&a->cd->scsi);
        } else {
            a->seek_cycles -= (int32_t)remaining;
            remaining = 0;
        }
    }
    if (a->playback_delay_cycles > 0 && remaining > 0) {
        if (remaining >= (uint32_t)a->playback_delay_cycles)
            a->playback_delay_cycles = 0;
        else
            a->playback_delay_cycles -= (int32_t)remaining;
    }
}

/* The sector can be played: audio, including the pregap stored before an audio track
 * that follows another audio track. */
static int playable_audio_sector(const CdImage *img, uint32_t lba)
{
    if (lba >= img->sector_count)
        return 0;
    int t = cd_track_of_lba(img, lba, 1);
    if (t < 0 || img->tracks[t].type != CD_TRACK_AUDIO)
        return 0;
    if (lba >= img->tracks[t].start_lba)
        return 1;
    return img->tracks[t].lead_in_stored && t > 0 && img->tracks[t - 1].type == CD_TRACK_AUDIO;
}

#define SECTOR_SAMPLES (2352 / 4)

/* PC version: the heard sample, skip samples ahead of the console's (cdaudio.h) */
static void heard_sample(CdAudio *a, uint64_t skip, int16_t *left, int16_t *right)
{
    *left = *right = 0;
    if (a->current_lba < a->start_lba || a->stop_lba < a->start_lba)
        return;
    uint64_t len = (uint64_t)(a->stop_lba - a->start_lba + 1) * SECTOR_SAMPLES;
    uint64_t p = (uint64_t)(a->current_lba - a->start_lba) * SECTOR_SAMPLES + a->current_sample + skip;
    if (p >= len) {
        if (a->stop_event != CD_AUDIO_STOP_EVENT_LOOP)
            return; /* past the end: silence until the console gets there */
        p %= len;
    }
    uint32_t lba = a->start_lba + (uint32_t)(p / SECTOR_SAMPLES), s = (uint32_t)(p % SECTOR_SAMPLES);
    if (!a->pc_sector_ok || a->pc_sector_lba != lba) {
        a->pc_sector_lba = lba;
        a->pc_sector_ok = 1;
        if (!playable_audio_sector(a->cd->image, lba) || cd_read_audio(a->cd->image, lba, (uint8_t *)a->pc_sector) != 0)
            memset(a->pc_sector, 0, sizeof(a->pc_sector));
    }
    *left = a->pc_sector[s * 2 + 0];
    *right = a->pc_sector[s * 2 + 1];
}

static void generate_samples(CdAudio *a)
{
    CdRom *cd = a->cd;
    if (!a->sector_attempted || a->sector_lba != a->current_lba) {
        a->sector_lba = a->current_lba;
        if (playable_audio_sector(cd->image, a->current_lba)) {
            a->sector_valid = cd_read_audio(cd->image, a->current_lba, (uint8_t *)a->sector) == 0;
            if (a->sector_valid)
                cdrom_set_current_sector(cd, a->current_lba);
        } else {
            memset(a->sector, 0, sizeof(a->sector));
            a->sector_valid = 1;
            cdrom_set_current_sector(cd, a->current_lba);
        }
        a->sector_attempted = 1;
    }
    if (a->sector_valid) {
        a->left_sample = a->sector[a->current_sample * 2 + 0];
        a->right_sample = a->sector[a->current_sample * 2 + 1];
    } else {
        a->left_sample = a->right_sample = 0;
    }
    if (a->pc_skip)
        heard_sample(a, a->pc_skip, &a->left_sample, &a->right_sample);
    if (cdrom_fader_enabled(cd, 0)) {
        double fader = cdrom_fader_value(cd);
        a->left_sample = (int16_t)(a->left_sample * fader);
        a->right_sample = (int16_t)(a->right_sample * fader);
    }

    a->current_sample++;
    if (a->current_sample == 2352 / 4) {
        a->current_sample = 0;
        a->current_lba++;
        if (a->current_lba > a->stop_lba) {
            if (a->current_lba >= sector_count(a))
                a->current_lba = sector_count(a) - 1;
            switch (a->stop_event) {
            case CD_AUDIO_STOP_EVENT_STOP:
                a->state = CD_AUDIO_STOPPED;
                break;
            case CD_AUDIO_STOP_EVENT_LOOP:
                a->current_lba = a->start_lba;
                break;
            case CD_AUDIO_STOP_EVENT_IRQ:
                a->state = CD_AUDIO_STOPPED;
                scsi_start_status(&cd->scsi, SCSI_STATUS_GOOD, 1);
                break;
            }
        }
        cdrom_set_current_sector(cd, a->current_lba);
    }
}

void cdaudio_sample(CdAudio *a)
{
    a->left_sample = a->right_sample = 0;
    if (a->state == CD_AUDIO_PLAYING && a->seek_cycles == 0 && a->playback_delay_cycles == 0) {
        generate_samples(a);
        a->played_samples++;
    }
    int mute = a->pc_mute && a->pc_hide;
    a->buffer[a->buffer_index + 0] = mute ? 0 : a->left_sample;
    a->buffer[a->buffer_index + 1] = mute ? 0 : a->right_sample;
    a->buffer_index += 2;
    if (a->buffer_index >= CDAUDIO_BUFFER_SIZE)
        a->buffer_index = 0; /* should not happen: ~1470 values per frame */
}

void cdaudio_pc_play(CdAudio *a, int16_t *buf, int n)
{
    int playing = a->state == CD_AUDIO_PLAYING && a->seek_cycles == 0 && a->playback_delay_cycles == 0 &&
                  !(a->pc_mute && a->pc_hide);
    double fader = cdrom_fader_enabled(a->cd, 0) ? cdrom_fader_value(a->cd) : 1.0; /* console paused */
    for (int i = 0; i < n; i++) {
        int16_t l = 0, r = 0;
        if (playing) {
            heard_sample(a, a->pc_skip++, &l, &r);
        }
        buf[i * 2 + 0] = (int16_t)(l * fader);
        buf[i * 2 + 1] = (int16_t)(r * fader);
    }
    if (a->stop_event == CD_AUDIO_STOP_EVENT_LOOP && a->stop_lba >= a->start_lba) {
        uint64_t len = (uint64_t)(a->stop_lba - a->start_lba + 1) * SECTOR_SAMPLES;
        if (a->pc_skip >= 2 * len)
            a->pc_skip = a->pc_skip % len + len; /* the same position, without growing forever (and never back to 0) */
    }
}

int cdaudio_end_frame(CdAudio *a, int16_t *buf)
{
    int n = a->buffer_index;
    memcpy(buf, a->buffer, (size_t)n * sizeof(int16_t));
    a->buffer_index = 0;
    return n;
}

void cdaudio_start(CdAudio *a, uint32_t lba, int pause)
{
    CdRom *cd = a->cd;
    a->pc_mute = 0;
    a->pc_skip = 0;
    if (cd_track_of_lba(cd->image, lba, 0) < 0)
        return;
    uint32_t current = cd->scsi.current_sector;
    a->seek_start_lba = current;
    a->playback_delay_cycles = 0;
    uint32_t seek = cd_time_to_cycles(cd_seek_time_ms(current, lba) * 1000);
    uint32_t delay = cd_time_to_cycles(PLAYBACK_DELAY_US);
    if (seek > delay) {
        a->seek_cycles = (int32_t)(seek - delay);
        if (!pause)
            a->playback_delay_cycles = (int32_t)delay;
    } else {
        a->seek_cycles = (int32_t)seek;
    }
    a->start_lba = lba;
    a->current_lba = lba;
    if (a->seek_cycles == 0)
        a->seek_start_lba = a->current_lba;
    a->current_sample = 0;
    a->stop_lba = sector_count(a) - 1;
    a->stop_event = CD_AUDIO_STOP_EVENT_STOP;
    a->state = pause ? CD_AUDIO_PAUSED : CD_AUDIO_PLAYING;
    cdrom_set_current_sector(cd, a->current_lba);
    a->sector_attempted = 0;
    a->sector_valid = 0;
}

void cdaudio_stop(CdAudio *a)
{
    a->playback_delay_cycles = 0;
    a->pc_mute = 0;
    a->pc_skip = 0;
    a->state = CD_AUDIO_STOPPED;
}

void cdaudio_pause(CdAudio *a)
{
    a->playback_delay_cycles = 0;
    a->pc_mute = 0;
    a->pc_skip = 0;
    a->state = CD_AUDIO_PAUSED;
}

void cdaudio_set_idle(CdAudio *a)
{
    a->seek_cycles = 0;
    a->playback_delay_cycles = 0;
    a->pc_mute = 0;
    a->pc_skip = 0;
    a->state = CD_AUDIO_IDLE;
}

void cdaudio_set_stop_lba(CdAudio *a, uint32_t lba, CdAudioStopEvent event)
{
    uint32_t count = sector_count(a);
    if (count == 0)
        return;
    if (lba >= count)
        lba = count - 1;
    a->stop_lba = lba;
    a->stop_event = event;
    if (a->state == CD_AUDIO_PAUSED)
        a->playback_delay_cycles = (int32_t)cd_time_to_cycles(PLAYBACK_DELAY_US);
    a->state = CD_AUDIO_PLAYING;
}

int cdaudio_is_seeking(const CdAudio *a)
{
    return a->seek_cycles > 0;
}

CdAudioState cdaudio_subcode_state(const CdAudio *a)
{
    if (a->state == CD_AUDIO_PLAYING && a->seek_cycles > 0)
        return CD_AUDIO_PAUSED;
    return a->state;
}

uint32_t cdaudio_subcode_lba(const CdAudio *a)
{
    if (a->state == CD_AUDIO_PLAYING && a->seek_cycles > 0)
        return a->seek_start_lba;
    return a->current_lba;
}
