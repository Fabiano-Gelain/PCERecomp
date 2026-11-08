// Opens a CUE/BIN image with the Geargrafx reader (CdRomCueBinImage, default options) and writes
// the disc summary and a CRC32 of each track, in the same format as our tools/cd/cd_info.c.
//
// Usage: gg_cdinfo <file.cue> <out.txt>
// (Geargrafx writes log messages to stdout, so the result goes to a file)
#include <cstdio>
#include <cstring>

#include "geargrafx.h"
#include "cdrom_cuebin_image.h"

bool g_mcp_stdio_mode = false;

static u32 crc32_update(u32 crc, const u8* p, size_t n)
{
    crc = ~crc;
    for (size_t i = 0; i < n; i++)
    {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

int main(int argc, char** argv)
{
    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0))
    {
        printf("gg_cdinfo: reads a CUE/BIN image with Geargrafx and writes the disc summary and per-track CRC32\n");
        printf("(same format as tools/cd/cd_info.c).\n");
        printf("usage: %s <file.cue> <out.txt>\n", argv[0]);
        return 0;
    }
    if (argc < 3)
    {
        printf("usage: %s <file.cue> <out.txt>\n", argv[0]);
        return 2;
    }
    FILE* out = fopen(argv[2], "w");
    if (!out)
        return 2;

    CdRomCueBinImage* img = new CdRomCueBinImage();
    img->Init();
    if (!img->LoadFromFile(argv[1], false))
    {
        fprintf(out, "error\n");
        fclose(out);
        return 1;
    }

    CdRomImage::TableOfContents* toc = img->GetTOC();
    fprintf(out, "tracks %d\n", (int)toc->tracks.size());
    fprintf(out, "sectors %u\n", toc->sector_count);
    fprintf(out, "duration %02u:%02u:%02u\n", toc->total_length.minutes, toc->total_length.seconds,
        toc->total_length.frames);

    for (size_t i = 0; i < toc->tracks.size(); i++)
    {
        const CdRomImage::Track& t = toc->tracks[i];
        fprintf(out, "track %02d %s start=%u end=%u sectors=%u", (int)i + 1, TrackTypeName(t.type),
            t.start_lba, t.end_lba, t.sector_count);
        if (t.has_lead_in)
            fprintf(out, " pause=%u stored=%d", t.lead_in_lba, t.lead_in_stored ? 1 : 0);
        else
            fprintf(out, " pause=-");
        fprintf(out, " byte=%llu", (unsigned long long)t.file_offset);

        u32 crc = 0, bad = 0, bad_count = 0;
        u8 buf[2352];
        if (t.type == GG_CDROM_AUDIO_TRACK)
        {
            u32 first = t.lead_in_stored ? t.lead_in_lba : t.start_lba;
            for (u32 lba = first; lba <= t.end_lba; lba++)
            {
                if (img->ReadSamples(lba, 0, (s16*)buf, 1176))
                    crc = crc32_update(crc, buf, 2352);
                else if (bad_count++ == 0)
                    bad = lba;
            }
        }
        else
        {
            for (u32 lba = t.start_lba; lba <= t.end_lba; lba++)
            {
                if (img->ReadSector(lba, buf))
                    crc = crc32_update(crc, buf, 2048);
                else if (bad_count++ == 0)
                    bad = lba;
            }
        }
        fprintf(out, " crc=%08X", crc);
        if (bad_count)
            fprintf(out, " failed=%u first=%u", bad_count, bad);
        fprintf(out, "\n");
    }
    fclose(out);
    delete img;
    return 0;
}
