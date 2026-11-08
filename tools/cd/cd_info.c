/*
 * cd_info: opens a CUE/BIN image with our reader (runtime/cd/cdimage.c) and
 * prints the disc's table of contents and a CRC32 of each track's contents.
 * gg_cdinfo (tools/reference_trace) prints the same, as read by Geargrafx;
 * tools/cd/run_cd_test.py compares the two outputs (so the output format must match gg_cdinfo's).
 *
 * Usage: cd_info <file.cue> [--sector <n> <output.bin>]
 *      --sector writes the 2048 bytes of data sector n (n = sectors since the start of the 1st data track).
 * Data track CRC: the 2048 user bytes of each sector, from the start to the end of the track.
 * Audio track CRC: the 2352 bytes of each sector, including the stored pregap (INDEX 00).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cd/cdimage.h"

static const char *const TYPE_NAMES[] = {"AUDIO", "MODE1/2048", "MODE1/2352"};

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

int main(int argc, char **argv)
{
    int help = argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0);
    if (argc < 2 || help) {
        printf("usage: %s <file.cue> [--sector <n> <output.bin>]\n"
               "  prints the disc's table of contents and the CRC32 of each track (compare with gg_cdinfo: tools/cd/run_cd_test.py)\n"
               "  --sector writes the 2048 bytes of data sector n (counted from the start of the 1st data track)\n",
               argv[0]);
        return help ? 0 : 2;
    }
    static CdImage img;
    if (cd_open(&img, argv[1]) != 0) {
        printf("error: %s\n", img.error);
        return 1;
    }
    if (argc >= 5 && strcmp(argv[2], "--sector") == 0) {
        int i = 0;
        while (i < img.track_count && img.tracks[i].type == CD_TRACK_AUDIO)
            i++;
        uint8_t buf[CD_SECTOR_DATA];
        FILE *out = i < img.track_count ? fopen(argv[4], "wb") : NULL;
        int ok = out && cd_read_sector(&img, img.tracks[i].start_lba + (uint32_t)atoi(argv[3]), buf) == 0 &&
                 fwrite(buf, 1, sizeof(buf), out) == sizeof(buf);
        if (out)
            fclose(out);
        cd_close(&img);
        return ok ? 0 : 1;
    }

    uint8_t m, s, f;
    cd_lba_to_msf(img.sector_count + 150, &m, &s, &f);
    printf("tracks %d\n", img.track_count);
    printf("sectors %u\n", img.sector_count);
    printf("duration %02u:%02u:%02u\n", m, s, f);

    for (int i = 0; i < img.track_count; i++) {
        const CdTrack *t = &img.tracks[i];
        printf("track %02d %s start=%u end=%u sectors=%u", i + 1, TYPE_NAMES[t->type], t->start_lba,
               t->end_lba, t->sector_count);
        if (t->has_lead_in)
            printf(" pause=%u stored=%d", t->lead_in_lba, t->lead_in_stored);
        else
            printf(" pause=-");
        printf(" byte=%llu", (unsigned long long)t->file_offset);

        uint32_t crc = 0;
        uint32_t bad = 0, bad_count = 0;
        uint8_t buf[CD_SECTOR_RAW];
        if (t->type == CD_TRACK_AUDIO) {
            uint32_t first = t->lead_in_stored ? t->lead_in_lba : t->start_lba;
            for (uint32_t lba = first; lba <= t->end_lba; lba++) {
                if (cd_read_audio(&img, lba, buf) == 0)
                    crc = crc32_update(crc, buf, CD_SECTOR_RAW);
                else if (bad_count++ == 0)
                    bad = lba;
            }
        } else {
            for (uint32_t lba = t->start_lba; lba <= t->end_lba; lba++) {
                if (cd_read_sector(&img, lba, buf) == 0)
                    crc = crc32_update(crc, buf, CD_SECTOR_DATA);
                else if (bad_count++ == 0)
                    bad = lba;
            }
        }
        printf(" crc=%08X", crc);
        if (bad_count)
            printf(" failed=%u first=%u", bad_count, bad);
        printf("\n");
    }
    cd_close(&img);
    return 0;
}
