// Generates a reference trace with Geargrafx (used as a library, unmodified).
//
// Usage: gg_trace [--input <script>] [--from-frame N] <rom> <instruction count> <output file>
//        (with --from-frame, recording starts only after N frames; <instructions> counts only recorded ones)
//        gg_trace --frame [--input <script>] [--audio <out.raw>] <rom> <frames> <out.ppm>
//        (--audio records the sound of all frames: signed 16-bit, interleaved stereo, 44100 Hz)
//        (--hashes <file> writes one line per frame: "<frame> <width>x<height> <RGB image crc>
//         <sample count> <audio crc>", to find the first differing frame in a long recording)
//
// Trace format: one line per instruction, with the state BEFORE executing it (same as
// our runtime's `pce_trace`):
//   E000 A:00 X:00 Y:00 S:00 P:04 V:0 M:00,00,00,00,00,00,00,00
// V = speed (0 = CSL, 1 = CSH), M = MPR0..MPR7.
//
// CD: with --bios <system card>, <rom> can be a .cue. The emulated console is a plain CD-ROM²
// (without the Super CD-ROM² extra RAM). --bram <file> starts with this
// save memory (2 KB raw).
//
// Input script: see tools/trace/README.md. Buttons change at the end of the given frame,
// between two instructions.
//
// The values Geargrafx randomizes at reset (registers, MPRs, RAM, palette)
// are set to 0, as in our runtime.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "geargrafx.h"

// Defined by the Geargrafx main program (MCP mode); the core only reads it.
bool g_mcp_stdio_mode = false;

static void input_pump() {}

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

struct InputEvent
{
    long frame;
    int buttons;
};

// Same bits as our runtime and Geargrafx: I, II, SELECT, RUN, up, right, down, left.
static const char* const k_button_names[8] = { "I", "II", "SELECT", "RUN", "UP", "RIGHT", "DOWN", "LEFT" };

static bool load_input(const char* path, std::vector<InputEvent>& events)
{
    FILE* f = std::fopen(path, "r");
    if (!f)
        return false;
    char line[256];
    while (std::fgets(line, sizeof(line), f))
    {
        char* p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == 0)
            continue;
        InputEvent ev;
        char names[200] = "";
        if (std::sscanf(p, "%ld %199s", &ev.frame, names) < 2)
            continue;
        ev.buttons = 0;
        for (char* tok = std::strtok(names, "+"); tok; tok = std::strtok(NULL, "+"))
            for (int b = 0; b < 8; b++)
                if (std::strcmp(tok, k_button_names[b]) == 0)
                    ev.buttons |= 1 << b;
        events.push_back(ev);
    }
    std::fclose(f);
    return true;
}

static void apply_buttons(GeargrafxCore* core, int buttons)
{
    for (int b = 0; b < 8; b++)
    {
        if (buttons & (1 << b))
            core->KeyPressed(GG_CONTROLLER_1, (GG_Keys)(1 << b));
        else
            core->KeyReleased(GG_CONTROLLER_1, (GG_Keys)(1 << b));
    }
}

// Applies the script events whose frame has already been reached.
static void update_input(GeargrafxCore* core, const std::vector<InputEvent>& events, size_t* next, long frames_done)
{
    while (*next < events.size() && events[*next].frame <= frames_done)
    {
        apply_buttons(core, events[*next].buttons);
        (*next)++;
    }
}

static void print_usage(const char* exe)
{
    std::printf("gg_trace: runs a ROM (or a CD .cue with --bios) on Geargrafx and writes a reference trace or frame.\n");
    std::printf("usage: %s [options] <rom> <instructions> <out.trace>\n", exe);
    std::printf("       %s --frame [options] <rom> <frames> <out.ppm>\n", exe);
    std::printf("  --input <file>      input script (see tools/trace/README.md)\n");
    std::printf("  --from-frame <n>    trace: start recording after n frames\n");
    std::printf("  --audio <file>      frame: write all audio (s16 stereo, 44100 Hz)\n");
    std::printf("  --hashes <file>     frame: write one line per frame with image/audio CRCs\n");
    std::printf("  --bios <file>       System Card BIOS (needed for a .cue)\n");
    std::printf("  --bram <file>       start with this CD save memory (2 KB raw)\n");
    std::printf("  -h, --help          show this help\n");
}

int main(int argc, char** argv)
{
    bool frame_mode = false;
    const char* input_path = NULL;
    long from_frame = 0;
    const char* audio_path = NULL;
    const char* bios_path = NULL;
    const char* hashes_path = NULL;
    const char* bram_path = NULL;
    std::vector<const char*> pos;
    for (int i = 1; i < argc; i++)
    {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        else if (std::strcmp(argv[i], "--frame") == 0)
            frame_mode = true;
        else if (std::strcmp(argv[i], "--input") == 0 && i + 1 < argc)
            input_path = argv[++i];
        else if (std::strcmp(argv[i], "--audio") == 0 && i + 1 < argc)
            audio_path = argv[++i];
        else if (std::strcmp(argv[i], "--bios") == 0 && i + 1 < argc)
            bios_path = argv[++i];
        else if (std::strcmp(argv[i], "--hashes") == 0 && i + 1 < argc)
            hashes_path = argv[++i];
        else if (std::strcmp(argv[i], "--bram") == 0 && i + 1 < argc)
            bram_path = argv[++i];
        else if (std::strcmp(argv[i], "--from-frame") == 0 && i + 1 < argc)
            from_frame = std::strtol(argv[++i], NULL, 10);
        else
            pos.push_back(argv[i]);
    }
    if (pos.size() < 3)
    {
        print_usage(argv[0]);
        return 2;
    }
    long count = std::strtol(pos[1], NULL, 10);

    std::vector<InputEvent> events;
    if (input_path && !load_input(input_path, events))
    {
        std::printf("could not read the input script %s\n", input_path);
        return 1;
    }
    size_t next_event = 0;

    GeargrafxCore* core = new GeargrafxCore();
    core->Init(input_pump);
    core->GetHuC6280()->SetResetValue(0);
    core->GetMemory()->SetResetValues(0, 0, 0, 0);
    core->GetHuC6260()->SetResetValue(0);
    // CD: the first-read delay (randomized between 0 and 1/60 s) is also set to 0.
    core->GetScsiController()->SetResetValue(0);

    if (bios_path)
    {
        core->GetMedia()->SetCDROMType(GG_CDROM_STANDARD);
        if (!core->GetMedia()->LoadBios(bios_path, true))
        {
            std::printf("could not load the BIOS %s\n", bios_path);
            return 1;
        }
    }
    if (!core->LoadMedia(pos[0]))
    {
        std::printf("could not load %s\n", pos[0]);
        return 1;
    }
    core->ResetMedia(false);
    if (bram_path)
    {
        // CD save memory (2 KB raw); reset formats it, so it is loaded afterwards.
        std::ifstream bram(bram_path, std::ios::binary);
        if (!bram || !core->GetMemory()->LoadRam(bram, 0x800))
        {
            std::printf("could not load the save %s\n", bram_path);
            return 1;
        }
    }
    update_input(core, events, &next_event, 0);

    std::vector<u8> frame(2048 * 1024 * 4);
    std::vector<s16> samples(GG_AUDIO_BUFFER_SIZE * 4);

    if (frame_mode)
    {
        // Runs <frames> frames and saves the last one as PPM (RGB).
        FILE* audio = audio_path ? std::fopen(audio_path, "wb") : NULL;
        FILE* hashes = hashes_path ? std::fopen(hashes_path, "w") : NULL;
        for (long i = 0; i < count; i++)
        {
            int sample_count = 0;
            core->RunToVBlank(frame.data(), samples.data(), &sample_count);
            if (audio)
                std::fwrite(samples.data(), sizeof(s16), sample_count, audio);
            if (hashes)
            {
                int w = core->GetHuC6260()->GetCurrentWidth();
                int h = core->GetHuC6260()->GetCurrentHeight();
                u32 crc_image = 0;
                for (int p = 0; p < w * h; p++)
                    crc_image = crc32_update(crc_image, &frame[p * 4], 3);
                u32 crc_audio = crc32_update(0, (const u8*)samples.data(), sample_count * sizeof(s16));
                std::fprintf(hashes, "%ld %dx%d %08X %d %08X\n", i + 1, w, h, crc_image, sample_count, crc_audio);
            }
            update_input(core, events, &next_event, i + 1);
        }
        if (audio)
            std::fclose(audio);
        if (hashes)
            std::fclose(hashes);
        int w = core->GetHuC6260()->GetCurrentWidth();
        int h = core->GetHuC6260()->GetCurrentHeight();
        FILE* ppm = std::fopen(pos[2], "wb");
        if (!ppm)
            return 1;
        std::fprintf(ppm, "P6\n%d %d\n255\n", w, h);
        for (int i = 0; i < w * h; i++)
            std::fwrite(&frame[i * 4], 1, 3, ppm);
        std::fclose(ppm);
        delete core;
        return 0;
    }

    FILE* out = std::fopen(pos[2], "w");
    if (!out)
    {
        std::printf("could not create %s\n", pos[2]);
        return 1;
    }

    GeargrafxCore::GG_Debug_Run debug = {};
    debug.step_debugger = true;

    HuC6280* cpu = core->GetHuC6280();
    Memory* mem = core->GetMemory();
    HuC6280::HuC6280_State* st = cpu->GetState();
    s32* vpos = core->GetHuC6260()->GetState()->VPOS;
    long frames_done = 0;

    for (long i = 0; i < count;)
    {
        if (frames_done >= from_frame)
        {
            i++;
            std::fprintf(out, "%04X A:%02X X:%02X Y:%02X S:%02X P:%02X V:%d M:%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X\n",
            st->PC->GetValue(), st->A->GetValue(), st->X->GetValue(), st->Y->GetValue(),
            st->S->GetValue(), st->P->GetValue(), *st->SPEED,
            mem->GetMpr(0), mem->GetMpr(1), mem->GetMpr(2), mem->GetMpr(3),
            mem->GetMpr(4), mem->GetMpr(5), mem->GetMpr(6), mem->GetMpr(7));
        }

        s32 before = *vpos;
        int sample_count = 0;
        core->RunToVBlank(frame.data(), samples.data(), &sample_count, &debug);
        s32 after = *vpos;

        // A frame ends when the VCE reaches line 256.
        bool frame_end = (after >= before) ? (before < 256 && after >= 256) : (before < 256);
        if (frame_end)
        {
            frames_done++;
            update_input(core, events, &next_event, frames_done);
        }
    }

    std::fclose(out);
    delete core;
    return 0;
}
