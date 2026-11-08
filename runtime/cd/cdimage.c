/*
 * CUE/BIN image reader. See cdimage.h.
 */
#include "cd/cdimage.h"
#include "util/u8file.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#define cd_fseek _fseeki64
#else
#define cd_fseek fseeko
#endif

typedef struct ParsedTrack {
    int file;
    CdTrackType type;
    int has_index0;
    uint32_t index0;
    int has_pregap;
    uint32_t pregap;
    uint32_t index1;
} ParsedTrack;

static void set_error(CdImage *img, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(img->error, sizeof(img->error), fmt, ap);
    va_end(ap);
}

void cd_lba_to_msf(uint32_t lba, uint8_t *m, uint8_t *s, uint8_t *f)
{
    *m = (uint8_t)(lba / 75 / 60);
    *s = (uint8_t)(lba / 75 % 60);
    *f = (uint8_t)(lba % 75);
}

/* Compares the start of p with the keyword (case-insensitive). */
static int keyword(const char *p, const char *kw)
{
    size_t n = strlen(kw);
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)p[i]) != kw[i])
            return 0;
    return 1;
}

/* Reads "mm:ss:ff" and returns the LBA, or -1. */
static long parse_msf(const char *p)
{
    int m, s, f;
    char c1, c2;
    if (sscanf(p, " %d %c %d %c %d", &m, &c1, &s, &c2, &f) != 5 || c1 != ':' || c2 != ':')
        return -1;
    if (m < 0 || m > 99 || s < 0 || s >= 60 || f < 0 || f >= 75)
        return -1;
    return ((long)m * 60 + s) * 75 + f;
}

static int open_bin(CdImage *img, const char *dir, const char *name)
{
    char path[1024];
    int absolute = name[0] == '/' || name[0] == '\\' || (name[0] && name[1] == ':');
    if (absolute || !dir[0])
        snprintf(path, sizeof(path), "%s", name);
    else
        snprintf(path, sizeof(path), "%s/%s", dir, name);

    if (img->file_count >= CD_MAX_TRACKS) {
        set_error(img, "too many files in CUE");
        return -1;
    }
    FILE *f = u8_fopen(path, "rb");
    if (!f) {
        set_error(img, "could not open %s", path);
        snprintf(img->bad_file, sizeof(img->bad_file), "%s", path);
        return -1;
    }
    cd_fseek(f, 0, SEEK_END);
#ifdef _MSC_VER
    int64_t size = _ftelli64(f);
#else
    int64_t size = ftello(f);
#endif
    if (size <= 0) {
        fclose(f);
        set_error(img, "empty file: %s", path);
        snprintf(img->bad_file, sizeof(img->bad_file), "%s", path);
        return -1;
    }
    img->files[img->file_count] = f;
    img->file_sizes[img->file_count] = (uint64_t)size;
    return img->file_count++;
}

/* Builds the TOC from the tracks read from the CUE, file by file (same rule as Geargrafx). */
static int layout(CdImage *img, const ParsedTrack *pt, int count)
{
    int i = 0;
    while (i < count) {
        int file = pt[i].file;
        uint32_t start_sector = img->track_count ? img->tracks[img->track_count - 1].end_lba + 1 : 0;
        uint32_t total_pregap = 0;
        int first = 1;

        for (; i < count && pt[i].file == file; i++, first = 0) {
            const ParsedTrack *p = &pt[i];
            CdTrack *t = &img->tracks[img->track_count];
            memset(t, 0, sizeof(*t));
            t->type = p->type;
            t->sector_size = p->type == CD_TRACK_MODE1_2048 ? CD_SECTOR_DATA : CD_SECTOR_RAW;
            t->file = file;

            if (p->has_pregap)
                total_pregap += p->pregap;
            t->start_lba = p->index1 + total_pregap + start_sector;
            if (p->has_pregap) {
                t->has_lead_in = 1;
                t->lead_in_lba = p->index1 + total_pregap - p->pregap + start_sector;
            } else if (p->has_index0) {
                t->has_lead_in = 1;
                t->lead_in_lba = p->index0 + total_pregap + start_sector;
            }

            uint64_t offset = 0;
            if (!first) {
                CdTrack *prev = t - 1;
                prev->end_lba = (t->has_lead_in ? t->lead_in_lba : t->start_lba) - 1;
                prev->sector_count = prev->end_lba - prev->start_lba + 1;
                offset = prev->file_offset + (uint64_t)prev->sector_count * prev->sector_size;
            }
            t->file_offset = offset;
            if (t->has_lead_in && !p->has_pregap) {
                t->file_offset += (uint64_t)(t->start_lba - t->lead_in_lba) * t->sector_size;
                t->lead_in_stored = 1;
            }
            img->track_count++;
        }

        /* The last track of the file runs to the end of the file (a partial sector counts as a sector). */
        CdTrack *last = &img->tracks[img->track_count - 1];
        uint64_t size = img->file_sizes[file];
        if (last->file_offset > size) {
            set_error(img, "track %d starts after the end of the file", img->track_count);
            return -1;
        }
        uint64_t rest = size - last->file_offset;
        uint64_t n = (rest + last->sector_size - 1) / last->sector_size;
        if (n == 0 || n > 0xFFFFFFFFu - last->start_lba) {
            set_error(img, "track %d has no sectors", img->track_count);
            return -1;
        }
        last->sector_count = (uint32_t)n;
        last->end_lba = last->start_lba + last->sector_count - 1;
    }
    img->sector_count = img->tracks[img->track_count - 1].end_lba + 1;
    return 0;
}

int cd_open(CdImage *img, const char *cue_path)
{
    memset(img, 0, sizeof(*img));

    FILE *cue = u8_fopen(cue_path, "rb");
    if (!cue) {
        set_error(img, "could not open %s", cue_path);
        snprintf(img->bad_file, sizeof(img->bad_file), "%s", cue_path);
        return -1;
    }

    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", cue_path);
    char *slash = NULL;
    for (char *c = dir; *c; c++)
        if (*c == '/' || *c == '\\')
            slash = c;
    if (slash)
        *slash = 0;
    else
        dir[0] = 0;

    ParsedTrack pt[CD_MAX_TRACKS];
    int count = 0, in_track = 0, current_file = -1, ok = 1;
    char line[1100];
    while (ok && fgets(line, sizeof(line), cue)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (keyword(p, "file")) {
            in_track = 0;
            char name[1024];
            char *q1 = strchr(p, '"'), *q2 = strrchr(p, '"');
            if (q1 && q2 && q2 > q1) {
                snprintf(name, sizeof(name), "%.*s", (int)(q2 - q1 - 1), q1 + 1);
            } else if (sscanf(p + 4, " %1023s", name) != 1) {
                set_error(img, "invalid FILE line: %s", line);
                ok = 0;
                break;
            }
            current_file = open_bin(img, dir, name);
            if (current_file < 0)
                ok = 0;
        } else if (keyword(p, "track")) {
            if (current_file < 0) {
                set_error(img, "TRACK before FILE");
                ok = 0;
                break;
            }
            if (count >= CD_MAX_TRACKS) {
                set_error(img, "too many tracks");
                ok = 0;
                break;
            }
            int number;
            char type[64] = "";
            if (sscanf(p + 5, " %d %63s", &number, type) != 2) {
                set_error(img, "invalid TRACK line: %s", line);
                ok = 0;
                break;
            }
            for (char *c = type; *c; c++)
                *c = (char)tolower((unsigned char)*c);
            ParsedTrack *t = &pt[count++];
            memset(t, 0, sizeof(*t));
            t->file = current_file;
            if (strcmp(type, "audio") == 0) {
                t->type = CD_TRACK_AUDIO;
            } else if (strcmp(type, "mode1/2048") == 0) {
                t->type = CD_TRACK_MODE1_2048;
            } else if (strcmp(type, "mode1/2352") == 0) {
                t->type = CD_TRACK_MODE1_2352;
            } else {
                set_error(img, "unsupported track type: %s", type);
                ok = 0;
            }
            in_track = 1;
        } else if (keyword(p, "pregap")) {
            long lba = parse_msf(p + 6);
            if (in_track && lba >= 0) {
                pt[count - 1].has_pregap = 1;
                pt[count - 1].pregap = (uint32_t)lba;
            }
        } else if (keyword(p, "index")) {
            if (!in_track) {
                set_error(img, "INDEX outside TRACK");
                ok = 0;
                break;
            }
            char *end;
            long number = strtol(p + 5, &end, 10);
            long lba = parse_msf(end);
            if (lba < 0)
                continue;
            if (number == 0) {
                pt[count - 1].has_index0 = 1;
                pt[count - 1].index0 = (uint32_t)lba;
            } else if (number == 1) {
                pt[count - 1].index1 = (uint32_t)lba;
            }
        }
    }
    fclose(cue);

    if (ok && count == 0) {
        set_error(img, "no tracks in CUE");
        ok = 0;
    }
    if (ok) {
        for (int f = 0; f < img->file_count && ok; f++) {
            int used = 0;
            for (int i = 0; i < count; i++)
                used |= pt[i].file == f;
            if (!used) {
                set_error(img, "CUE file %d has no tracks", f + 1);
                ok = 0;
            }
        }
    }
    if (ok && layout(img, pt, count) != 0)
        ok = 0;
    if (!ok) {
        char err[sizeof(img->error)], bad[sizeof(img->bad_file)];
        memcpy(err, img->error, sizeof(err));
        memcpy(bad, img->bad_file, sizeof(bad));
        cd_close(img);
        memcpy(img->error, err, sizeof(err));
        memcpy(img->bad_file, bad, sizeof(bad));
        return -1;
    }
    return 0;
}

void cd_close(CdImage *img)
{
    for (int i = 0; i < img->file_count; i++)
        if (img->files[i])
            fclose(img->files[i]);
    memset(img, 0, sizeof(*img));
}

int cd_track_of_lba(const CdImage *img, uint32_t lba, int include_lead_in)
{
    for (int i = 0; i < img->track_count; i++) {
        const CdTrack *t = &img->tracks[i];
        if (lba >= t->start_lba && lba <= t->end_lba)
            return i;
        if (include_lead_in && t->has_lead_in && lba >= t->lead_in_lba && lba < t->start_lba)
            return i;
    }
    return -1;
}

static int read_at(CdImage *img, int file, uint64_t offset, uint8_t *out, uint32_t size)
{
    if (offset + size > img->file_sizes[file])
        return -1;
    FILE *f = img->files[file];
    if (cd_fseek(f, (int64_t)offset, SEEK_SET) != 0)
        return -1;
    return fread(out, 1, size, f) == size ? 0 : -1;
}

int cd_read_sector(CdImage *img, uint32_t lba, uint8_t *out2048)
{
    int i = cd_track_of_lba(img, lba, 0);
    if (i < 0)
        return -1;
    const CdTrack *t = &img->tracks[i];
    uint64_t offset = t->file_offset + (uint64_t)(lba - t->start_lba) * t->sector_size;
    if (t->sector_size == CD_SECTOR_RAW)
        offset += 16;
    return read_at(img, t->file, offset, out2048, CD_SECTOR_DATA);
}

int cd_read_audio(CdImage *img, uint32_t lba, uint8_t *out2352)
{
    if (lba >= img->sector_count)
        return -1;
    int i = cd_track_of_lba(img, lba, 1);
    if (i < 0)
        return -1;
    const CdTrack *t = &img->tracks[i];
    uint64_t offset;
    if (lba < t->start_lba) {
        if (!t->lead_in_stored)
            return -1;
        offset = t->file_offset - (uint64_t)(t->start_lba - lba) * t->sector_size;
    } else {
        offset = t->file_offset + (uint64_t)(lba - t->start_lba) * t->sector_size;
    }
    return read_at(img, t->file, offset, out2352, CD_SECTOR_RAW);
}
