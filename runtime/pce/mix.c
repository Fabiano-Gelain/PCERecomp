/*
 * Mix of the frame's sound. See mix.h.
 */
#include "pce/mix.h"

#include <math.h>

static float percent(int v)
{
    return (float)(v < 0 ? 0 : v > 100 ? 100 : v) / 100.0f;
}

void audio_mix_mednafen(AudioMix *mix, const AudioVolumes *vol)
{
    float master = percent(vol->master);
    mix->on = 1;
    mix->psg_music = MIX_MEDNAFEN_PSG * percent(vol->chip_music) * master;
    mix->psg_sfx = MIX_MEDNAFEN_PSG * percent(vol->effects) * master;
    mix->adpcm = MIX_MEDNAFEN_ADPCM * percent(vol->voices) * master;
    mix->cdda = MIX_MEDNAFEN_CDDA * percent(vol->music) * master;
    mix->chip_filter = vol->chip_filter;
}

void audio_mix_frame(int16_t *buf, int count, const int16_t *adpcm, int n_adpcm, const int16_t *cdda, int n_cdda,
                     const AudioMix *mix)
{
    if (!mix || !mix->on) {
        for (int i = 0; i < count; i++) {
            int32_t v = (int32_t)buf[i] + (i < n_adpcm ? adpcm[i] : 0) + (i < n_cdda ? cdda[i] : 0);
            buf[i] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
        }
        return;
    }
    for (int i = 0; i < count; i++) {
        float v = buf[i] + (i < n_adpcm ? adpcm[i] * mix->adpcm : 0.0f) +
                  (i < n_cdda ? cdda[i] * mix->cdda : 0.0f);
        long r = lrintf(v);
        buf[i] = (int16_t)(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
    }
}
