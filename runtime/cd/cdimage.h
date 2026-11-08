/*
 * CUE/BIN CD image: reads the .cue file, opens the .bin of each track
 * and builds the disc table of contents (TOC) with the position of each track in sectors (LBA).
 *
 * A CD sector is 2352 bytes. In data tracks (MODE1/2352) the 2048 useful bytes
 * start at byte 16 (sync and header come before); in audio tracks
 * all 2352 bytes are sound: 588 16-bit stereo samples, 44100 Hz.
 * LBA 0 = first sector of track 1 (time 00:02:00 on a real disc).
 *
 * Positions follow Geargrafx (reference/Geargrafx/src/cdrom_cuebin_image.cpp,
 * default options), because it is the one that provides the reference traces:
 *  - files are laid out in sequence: the 1st sector of a file comes right after the
 *    last sector of the previous file;
 *  - INDEX 00 (pause before the track) is stored in the file and occupies LBAs;
 *  - PREGAP is a pause that occupies LBAs but is not in the file.
 * Only BINARY files and AUDIO, MODE1/2048 and MODE1/2352 tracks (MODE2 does not exist on the PC Engine).
 */
#ifndef PCE_CDIMAGE_H
#define PCE_CDIMAGE_H

#include <stdint.h>
#include <stdio.h>

#define CD_MAX_TRACKS 99
#define CD_SECTOR_RAW 2352
#define CD_SECTOR_DATA 2048

typedef enum CdTrackType {
    CD_TRACK_AUDIO,
    CD_TRACK_MODE1_2048,
    CD_TRACK_MODE1_2352
} CdTrackType;

typedef struct CdTrack {
    CdTrackType type;
    uint32_t sector_size;  /* bytes per sector in the file: 2352 or 2048 */
    uint32_t start_lba;    /* INDEX 01: where the track actually starts */
    uint32_t end_lba;      /* last sector (inclusive) */
    uint32_t sector_count; /* end_lba - start_lba + 1 */
    int has_lead_in;       /* there is a pause before the track (INDEX 00 or PREGAP) */
    uint32_t lead_in_lba;  /* start of the pause */
    int lead_in_stored;    /* the pause is stored in the file (INDEX 00) */
    uint64_t file_offset;  /* byte, in the file, of sector start_lba */
    int file;              /* index into CdImage.files */
} CdTrack;

typedef struct CdImage {
    int track_count;
    CdTrack tracks[CD_MAX_TRACKS];
    uint32_t sector_count; /* sectors on the disc (last end_lba + 1) */
    int file_count;
    FILE *files[CD_MAX_TRACKS];
    uint64_t file_sizes[CD_MAX_TRACKS];
    char error[600];       /* reason for the cd_open failure */
    char bad_file[1024];   /* file (.cue or .bin) that failed to open or is empty, if that was the reason */
} CdImage;

/* Opens the .cue and the .bin files it names (relative to the .cue folder). 0 = ok; otherwise -1 and
 * img->error explains. On failure there is no need to call cd_close. */
int cd_open(CdImage *img, const char *cue_path);
void cd_close(CdImage *img);

/* Track (0 = track 1) that contains the LBA, or -1. With include_lead_in, the pause before
 * the track counts as part of it. */
int cd_track_of_lba(const CdImage *img, uint32_t lba, int include_lead_in);

/* Reads the 2048 useful bytes of a data sector. 0 = ok, -1 = LBA outside a data track
 * or outside the file. */
int cd_read_sector(CdImage *img, uint32_t lba, uint8_t *out2048);

/* Reads the 2352 raw bytes of an audio sector (including the stored INDEX 00 pause).
 * 0 = ok, -1 = invalid LBA or sector is not in the file. */
int cd_read_audio(CdImage *img, uint32_t lba, uint8_t *out2352);

/* Time as minutes:seconds:frames (75 frames per second). */
void cd_lba_to_msf(uint32_t lba, uint8_t *m, uint8_t *s, uint8_t *f);

#endif
