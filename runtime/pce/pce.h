/*
 * The console (HuCard): CPU + physical memory map + I/O page.
 *
 * Physical map (8 KB banks, 21 bits):
 *   $00-$7F  HuCard ROM (mirrored according to its size)
 *   $80-$87  CD-ROM² RAM (CD only; see cd/cdrom.h)
 *   $F7      CD-ROM² save RAM (CD only)
 *   $F8      work RAM, 8 KB ($F9-$FB are mirrors on the plain PC-Engine)
 *   $FF      hardware page: VDC, VCE, PSG, timer, pad, interrupts, CD
 *   rest     unmapped (reads $FF, writes ignored)
 *
 * Reference: reference/Geargrafx/src/memory.cpp, memory_inline.h, media.cpp.
 * VDC and VCE: see vdc.h and vce.h. Sound: psg.h.
 */
#ifndef PCE_PCE_H
#define PCE_PCE_H

#include <stddef.h>
#include <stdint.h>

#include "cpu/huc6280.h"
#include "pce/mix.h"
#include "pce/psg.h"
#include "pce/vce.h"
#include "pce/vdc.h"

#define PCE_BANK_SIZE 0x2000u
#define PCE_RAM_BANK 0xF8
#define PCE_IO_BANK 0xFF
#define PCE_RAM_GEN_PAGES ((0x10000 + 0x2000) / 256)

/* Pad buttons (1 = pressed). */
enum {
    PCE_PAD_I = 0x01, PCE_PAD_II = 0x02, PCE_PAD_SELECT = 0x04, PCE_PAD_RUN = 0x08,
    PCE_PAD_UP = 0x10, PCE_PAD_RIGHT = 0x20, PCE_PAD_DOWN = 0x40, PCE_PAD_LEFT = 0x80,
};

typedef struct Pce {
    HuC6280 cpu;
    Vce vce;
    Vdc vdc;
    Psg psg;
    uint64_t master_cycles; /* 21.477 MHz master clock */
    int frame_ready;        /* a new frame has finished (cleared by whoever reads it) */
    /* Deferred video: master cycles the VCE/VDC have not run yet, and how many can stay
     * behind without anything beyond counters and ordinary dots happening (vce_quiet_cycles). Every I/O to
     * the VDC/VCE catches up first (pce_sync_video); VRAM, sprites and palette are never deferred. */
    uint32_t video_pending, video_quiet;

    uint8_t *rom;
    size_t rom_size;
    uint32_t rom_crc32; /* CRC32 of the ROM (without header), to identify the game */
    const uint8_t *rom_map[0x80]; /* physical bank -> 8 KB of the ROM */

    uint8_t ram[PCE_BANK_SIZE];

    uint8_t io_buffer; /* last value on the I/O bus ("open" reads) */

    /* Pad ($1000) */
    uint8_t pad_sel, pad_clr;
    uint8_t pad_buttons;
    uint8_t region_japan; /* 0 = TurboGrafx-16 (US) */
    uint8_t cd_attached;

    /* Timer ($0C00): counts every 1024 fast-CPU cycles (3072 master cycles) */
    int timer_enabled;
    uint8_t timer_reload, timer_counter;
    int32_t timer_cycles;

    /* Sound of the last frame (interleaved stereo, 44100 Hz), filled in by pce_run_frame */
    int16_t audio[PSG_BUFFER_SIZE];
    int audio_count;
    /* Mix volumes (pce/mix.h). Zeroed = Geargrafx mix; the PC version turns it on. Does not change
     * anything in the console, only the sound that comes out of it. */
    AudioMix mix;

    /* Write counter per 256-byte span of CD RAM (0-255) and of work RAM
     * (256-287): recompiled code running in RAM checks its bytes only when the span
     * has changed (huc6280_recomp.h, HuRamSig). Starts at 1; reset adds 1 to all of them. */
    uint32_t ram_gen[PCE_RAM_GEN_PAGES];
    uint32_t ram_epoch; /* changes on every reset (and between different consoles in the same program) */

    /* CD-ROM² (cdrom.h): NULL = HuCard-only console */
    struct CdRom *cd;
    struct CdImage *cd_image;

    /* Counters for tests/diagnostics */
    uint32_t vdc_writes, vce_writes;

    /* Coverage recorder (coverage.h); NULL = off */
    struct Coverage *coverage;
    /* Origin of the RAM bytes (cd/origin.h), turned on by coverage in CD games; NULL = off */
    struct Origin *origin;
    /* BIOS routine profile (bios_profile.h); NULL = off */
    struct BiosProfile *bios_profile;
    /* BIOS routines rewritten in C (bios/hle.h); NULL = real BIOS only */
    struct Hle *hle;
    int hle_owned; /* hle allocated by pce_load_cd (synthetic BIOS): freed in pce_free */
    /* Diagnostics: marks (1 per byte, 256 KB) the bytes of banks $00-$1F (BIOS) read by the game
     * code as data. NULL = off. */
    uint8_t *bios_reads;
    /* Log of writes to the hardware (C BIOS verification); NULL = off */
    struct IoLog *io_log;
    /* Observer called before (after = 0) and after (after = 1) each instruction, in the
     * interpreter and in the recompiled code (e.g. recording a trace). NULL = none. */
    void (*on_instruction)(struct Pce *pce, int after);
    void *user;
    /* PC option that changes the game itself (e.g. speeding up text; the game project turns it on): called before each
     * instruction, before the observer, in the interpreter and in the recompiled code. NULL = the game as on the console.
     * The lockstep sets the same one on the reference. */
    void (*game_patch)(struct Pce *pce);

    /* Recompiled code (recomp.h): looks up the function for the current bank/address; NULL = interpreter only */
    void (*(*recomp_lookup)(int page, int bank, int offset))(HuC6280 *cpu);
    uint64_t recomp_calls;        /* entries into recompiled functions */
    uint64_t interpreted_steps;   /* instructions executed by the interpreter */
    uint32_t *interp_hist;        /* NULL or 256 x 8 KB: interpreted instructions per bank:offset */
} Pce;

/* Loads a HuCard ROM (strips the 512-byte copier header if present).
 * Returns 0 on success. */
int pce_load_rom_file(Pce *pce, const char *path);
int pce_load_rom_memory(Pce *pce, const uint8_t *data, size_t size);
/* Console with CD-ROM²: the BIOS (System Card) takes the place of the HuCard and the disc (.cue)
 * goes to the drive. Returns 0 on success; otherwise -1 and a message on stderr. */
int pce_load_cd(Pce *pce, const char *bios_path, const char *cue_path);
/* CD-ROM² save RAM (2 KB raw, same format as Geargrafx). Load after pce_reset
 * (reset formats the memory). They return 0 on success; -1 without a CD or on a file error. */
int pce_load_bram(Pce *pce, const char *path);
int pce_save_bram(const Pce *pce, const char *path);
void pce_free(Pce *pce);

/* Console reset: the CPU reads the vector at $FFFE of bank 0. */
void pce_reset(Pce *pce);

/* Executes one instruction; returns the master clock cycles spent. */
int pce_step(Pce *pce);

/* Runs until the VCE finishes a frame (or until max_instructions). Returns 1 if it finished. */
int pce_run_frame(Pce *pce, long max_instructions);
/* Closes the frame's sound in audio/audio_count: PSG + CD with the mix volumes (whoever runs the frame
 * without pce_run_frame calls this directly). */
void pce_end_frame_audio(Pce *pce);

/* Full copy of a console (for C BIOS verification: running two versions from the same
 * state). The ROM and the CD image are shared; hooks, coverage and profiles are not copied.
 * Free with pce_free_clone. Returns 0 on success. */
int pce_clone(Pce *dst, const Pce *src);
void pce_free_clone(Pce *pce);
/* Advances only the hardware (video, sound, CD, timer) by N master cycles, with the CPU stopped between two
 * instructions (to compare two consoles at the same instant). */
void pce_advance_hardware(Pce *pce, uint64_t master_cycles);
void pce_sync_video(Pce *pce); /* the VCE/VDC catch up with the rest of the console */

/* Writes to the hardware (I/O page and ST0-2), in order: address (13 bits; ST0-2 = $2000 + port) and value. */
#define IO_LOG_MAX 65536
typedef struct IoLog {
    uint32_t count;     /* may exceed IO_LOG_MAX: only the first ones are kept */
    uint16_t addr[IO_LOG_MAX];
    uint8_t value[IO_LOG_MAX];
} IoLog;

/* Direct access to the physical bus (21 bits), as the CPU does it. */
uint8_t pce_bus_read(Pce *pce, uint32_t addr);
void pce_bus_write(Pce *pce, uint32_t addr, uint8_t value);

#endif
