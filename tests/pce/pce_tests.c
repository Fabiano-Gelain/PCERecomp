/*
 * Console tests (memory map, I/O page, ST0-2, CSL/CSH) with synthetic ROMs
 * assembled right here.
 *
 * Usage: pce_tests
 * Exit code 0 = all passed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "cpu/huc6280_recomp.h"
#include "input_script.h"
#include "pce/coverage.h"
#include "pce/lockstep.h"
#include "pce/pce.h"

static int g_fails;

#define CHECK(cond, ...)                         \
    do {                                         \
        if (!(cond)) {                           \
            printf("  FAILED (line %d): ", __LINE__); \
            printf(__VA_ARGS__);                 \
            printf("\n");                        \
            g_fails++;                           \
        }                                        \
    } while (0)

/* ROM in which the first byte of each bank is the bank number. */
static uint8_t *make_numbered_rom(size_t banks)
{
    uint8_t *rom = (uint8_t *)calloc(banks, PCE_BANK_SIZE);
    for (size_t b = 0; b < banks; b++)
        rom[b * PCE_BANK_SIZE] = (uint8_t)b;
    return rom;
}

static uint8_t bank_id(Pce *pce, unsigned bank)
{
    return pce_bus_read(pce, (uint32_t)bank << 13);
}

static void test_rom_mirroring(void)
{
    printf("ROM mirroring\n");
    static Pce pce; /* too large for the stack (Vce.frame_index) */
    memset(&pce, 0, sizeof(pce));

    uint8_t *rom = make_numbered_rom(32); /* 256 KB, like Bomberman */
    CHECK(pce_load_rom_memory(&pce, rom, 32 * PCE_BANK_SIZE) == 0, "load 256 KB");
    for (unsigned b = 0; b < 0x80; b++)
        CHECK(bank_id(&pce, b) == b % 32, "256 KB: bank %02X read %02X", b, bank_id(&pce, b));
    free(rom);

    rom = make_numbered_rom(48); /* 384 KB: special layout */
    CHECK(pce_load_rom_memory(&pce, rom, 48 * PCE_BANK_SIZE) == 0, "load 384 KB");
    CHECK(bank_id(&pce, 0x1F) == 0x1F, "384 KB: bank 1F");
    CHECK(bank_id(&pce, 0x20) == 0x00, "384 KB: bank 20 should mirror 00");
    CHECK(bank_id(&pce, 0x40) == 0x20, "384 KB: bank 40 should be 20");
    CHECK(bank_id(&pce, 0x4F) == 0x2F, "384 KB: bank 4F should be 2F");
    CHECK(bank_id(&pce, 0x50) == 0x20, "384 KB: bank 50 should mirror 20");
    free(rom);

    /* A 512-byte copier header is stripped. */
    uint8_t *with_header = (uint8_t *)calloc(1, 512 + 32 * PCE_BANK_SIZE);
    rom = make_numbered_rom(32);
    memcpy(with_header + 512, rom, 32 * PCE_BANK_SIZE);
    CHECK(pce_load_rom_memory(&pce, with_header, 512 + 32 * PCE_BANK_SIZE) == 0, "load with header");
    CHECK(bank_id(&pce, 0x05) == 0x05 && pce.rom_size == 32 * PCE_BANK_SIZE, "header not stripped");
    free(rom);
    free(with_header);
    pce_free(&pce);
}

static void test_ram_and_unmapped(void)
{
    printf("RAM, empty banks and ROM writes\n");
    static Pce pce; /* too large for the stack (Vce.frame_index) */
    memset(&pce, 0, sizeof(pce));
    uint8_t *rom = make_numbered_rom(32);
    pce_load_rom_memory(&pce, rom, 32 * PCE_BANK_SIZE);
    pce_reset(&pce);

    pce_bus_write(&pce, (0xF8u << 13) | 0x0123, 0x5A);
    for (unsigned b = 0xF8; b <= 0xFB; b++)
        CHECK(pce_bus_read(&pce, (b << 13) | 0x0123) == 0x5A, "RAM: bank %02X does not mirror F8", b);
    pce_bus_write(&pce, (0xFBu << 13) | 0x1FFF, 0x77);
    CHECK(pce.ram[0x1FFF] == 0x77, "RAM: write through mirror FB");

    CHECK(pce_bus_read(&pce, 0x90u << 13) == 0xFF, "empty bank 90 should read FF");
    CHECK(pce_bus_read(&pce, 0xF7u << 13) == 0xFF, "bank F7 (no backup RAM) should read FF");
    pce_bus_write(&pce, 3u << 13, 0x99);
    CHECK(bank_id(&pce, 3) == 3, "writing to ROM should change nothing");
    free(rom);
    pce_free(&pce);
}

static void test_pad_port(void)
{
    printf("joypad port ($1000)\n");
    static Pce pce; /* too large for the stack (Vce.frame_index) */
    memset(&pce, 0, sizeof(pce));
    uint8_t *rom = make_numbered_rom(32);
    pce_load_rom_memory(&pce, rom, 32 * PCE_BANK_SIZE);
    pce_reset(&pce);
    const uint32_t port = (0xFFu << 13) | 0x1000;

    /* After reset (SEL=1, CLR=1): bits 4,5 = 1; bit 6 = 0 (US); bit 7 = 1 (no CD); lines = 0. */
    CHECK(pce_bus_read(&pce, port) == 0xB0, "reset: read %02X, expected B0", pce_bus_read(&pce, port));

    pce_bus_write(&pce, port, 0x00); /* SEL=0: buttons */
    CHECK(pce_bus_read(&pce, port) == 0xBF, "nothing pressed: %02X", pce_bus_read(&pce, port));
    pce.pad_buttons = PCE_PAD_RUN | PCE_PAD_UP;
    CHECK(pce_bus_read(&pce, port) == 0xB7, "RUN pressed: %02X", pce_bus_read(&pce, port));
    pce_bus_write(&pce, port, 0x01); /* SEL=1: directions */
    CHECK(pce_bus_read(&pce, port) == 0xBE, "UP pressed: %02X", pce_bus_read(&pce, port));

    pce.region_japan = 1;
    CHECK(pce_bus_read(&pce, port) & 0x40, "Japanese console should set bit 6");
    free(rom);
    pce_free(&pce);
}

/* Program in bank 0 (address $E000 with MPR7 = 0). */
static void test_cpu_on_bus(void)
{
    printf("CPU on the bus: ST0-2, CSL/CSH, VDC/VCE wait states, TIA\n");
    static const uint8_t prog[] = {
        0x78,                   /* E000 SEI */
        0x54,                   /* E001 CSL */
        0xA9, 0xFF, 0x53, 0x01, /* E002 LDA #$FF / TAM #$01 -> $0000 = I/O */
        0xA9, 0xF8, 0x53, 0x02, /* E006 LDA #$F8 / TAM #$02 -> $2000 = RAM */
        0x03, 0x05,             /* E00A ST0 #$05 */
        0x13, 0xAA,             /* E00C ST1 #$AA */
        0x23, 0xBB,             /* E00E ST2 #$BB */
        0xD4,                   /* E010 CSH */
        0x8D, 0x02, 0x04,       /* E011 STA $0402 (VCE) */
        0xAD, 0x00, 0x10,       /* E014 LDA $1000 */
        0xE3, 0x00, 0x20, 0x02, 0x00, 0x04, 0x00, /* E017 TIA $2000,$0002,4 */
        0x80, 0xFE,             /* E01E BRA * */
    };
    uint8_t *rom = (uint8_t *)calloc(32, PCE_BANK_SIZE);
    memcpy(rom, prog, sizeof(prog));
    rom[0x1FFE] = 0x00; /* reset vector = $E000 */
    rom[0x1FFF] = 0xE0;

    static Pce pce; /* too large for the stack (Vce.frame_index) */
    memset(&pce, 0, sizeof(pce));
    pce_load_rom_memory(&pce, rom, 32 * PCE_BANK_SIZE);
    pce_reset(&pce);
    CHECK(pce.cpu.pc == 0xE000, "reset: PC=%04X", pce.cpu.pc);

    pce_step(&pce);                         /* SEI */
    CHECK(pce_step(&pce) == 3 * 12, "CSL should take 3 cycles x 12");
    for (int i = 0; i < 4; i++)
        pce_step(&pce);
    CHECK(pce.cpu.mpr[0] == 0xFF && pce.cpu.mpr[1] == 0xF8, "TAM: MPR0=%02X MPR1=%02X", pce.cpu.mpr[0], pce.cpu.mpr[1]);

    /* ST0 = 4 cycles + 1 VDC wait = 5, at low speed (x12). */
    CHECK(pce_step(&pce) == 5 * 12, "ST0 should take 5 cycles x 12");
    pce_step(&pce);
    pce_step(&pce);
    /* ST0 #$05 selects the CR register; ST1/ST2 write $BBAA (mask $1FFF). */
    CHECK(pce.vdc.ar == 0x05 && pce.vdc.reg[5] == 0x1BAA,
          "ST0/1/2 should write to the VDC: AR=%02X CR=%04X", pce.vdc.ar, pce.vdc.reg[5]);

    CHECK(pce_step(&pce) == 3 * 3 && pce.cpu.speed == 1, "CSH should take 3 cycles x 3");
    /* STA abs = 5 cycles + 1 VCE wait, at high speed (x3). */
    CHECK(pce_step(&pce) == 6 * 3, "STA to the VCE should take 6 cycles x 3");
    CHECK(pce.vce.color_address == 0xF8, "VCE: color address should be F8, was %03X", pce.vce.color_address);

    pce_step(&pce); /* LDA $1000 */
    CHECK(pce.cpu.a == 0xB0, "LDA $1000: A=%02X", pce.cpu.a);

    /* TIA: 17 + 6x4 cycles + 2 wait cycles for VDC writes during a block = 49. */
    pce.ram[0] = 0x11; pce.ram[1] = 0x22; pce.ram[2] = 0x33; pce.ram[3] = 0x44;
    uint32_t before = pce.vdc_writes;
    CHECK(pce_step(&pce) == 49 * 3, "TIA to the VDC should take 49 cycles x 3");
    CHECK(pce.vdc_writes - before == 4 && pce.vdc.reg[5] == (0x4433 & 0x1FFF),
          "TIA should alternate $0002/$0003: CR=%04X", pce.vdc.reg[5]);
    CHECK(pce.cpu.pc == 0xE01E, "PC after TIA: %04X", pce.cpu.pc);

    free(rom);
    pce_free(&pce);
}

static void vdc_reg(Pce *pce, uint8_t reg, uint16_t value)
{
    const uint32_t io = 0xFFu << 13;
    pce_bus_write(pce, io | 0x0000, reg);
    pce_bus_write(pce, io | 0x0002, (uint8_t)value);
    pce_bus_write(pce, io | 0x0003, (uint8_t)(value >> 8));
}

static void test_vram_and_palette(void)
{
    printf("VRAM and palette through the registers\n");
    static Pce pce; /* too large for the stack (Vce.frame_index) */
    memset(&pce, 0, sizeof(pce));
    uint8_t *rom = make_numbered_rom(32);
    pce_load_rom_memory(&pce, rom, 32 * PCE_BANK_SIZE);
    pce_reset(&pce);
    const uint32_t io = 0xFFu << 13;

    vdc_reg(&pce, 0x05, 0x0800);     /* CR: increment of 32 */
    vdc_reg(&pce, 0x00, 0x0100);     /* MAWR */
    vdc_reg(&pce, 0x02, 0x1234);     /* VWR -> VRAM[$100] */
    vdc_reg(&pce, 0x02, 0xABCD);     /* VWR -> VRAM[$120] (waits for the previous write) */
    vdc_reg(&pce, 0x01, 0x0120);     /* MARR: triggers the read */
    pce_bus_write(&pce, io | 0x0000, 0x02);
    uint8_t lo = pce_bus_read(&pce, io | 0x0002);
    uint8_t hi = pce_bus_read(&pce, io | 0x0003);
    CHECK(pce.vdc.vram[0x100] == 0x1234, "VRAM[$100]=%04X", pce.vdc.vram[0x100]);
    CHECK((lo | (hi << 8)) == 0xABCD, "read of VRAM[$120]: %02X%02X", hi, lo);

    pce_bus_write(&pce, io | 0x0402, 0x05); /* color address $005 */
    pce_bus_write(&pce, io | 0x0403, 0x00);
    pce_bus_write(&pce, io | 0x0404, 0xFF); /* color $1FF (white) */
    pce_bus_write(&pce, io | 0x0405, 0x01);
    CHECK(pce.vce.color[5] == 0x1FF && pce.vce.color_address == 6, "palette: color[5]=%03X", pce.vce.color[5]);
    CHECK(vce_rgb(0x1FF) == 0xFFFFFFFF && vce_rgb(0x038) == 0xFFFF0000, "color conversion");

    free(rom);
    pce_free(&pce);
}

/* Program with JSR, RTS, JMP, JMP (ind) and a timer interrupt; checks the coverage. */
static void test_coverage(void)
{
    printf("coverage recorder\n");
    static const struct { uint16_t addr; const char *hex; } prog[] = {
        { 0xE000, "78 D4 A9 FF 53 01 A9 F8 53 02 A2 FF 9A" }, /* SEI CSH; I/O at $0000; RAM at $2000; S=$FF */
        { 0xE00D, "20 30 E0" },          /* JSR $E030 */
        { 0xE010, "4C 20 E0" },          /* JMP $E020 */
        { 0xE020, "6C 40 E0" },          /* JMP ($E040) -> $E050 */
        { 0xE030, "60" },                /* RTS */
        { 0xE040, "50 E0" },             /* pointer (data, not code) */
        { 0xE050, "A9 03 8D 02 14" },    /* disable IRQ1/IRQ2 */
        { 0xE055, "9C 00 0C A9 01 8D 01 0C" }, /* timer: reload 0, enable */
        { 0xE05D, "58 80 FE" },          /* CLI; BRA * */
        { 0xE070, "8D 03 14 40" },       /* timer handler: acknowledge; RTI */
        { 0xFFFA, "70 E0" },             /* timer vector */
        { 0xFFFE, "00 E0" },             /* reset vector */
    };
    uint8_t *rom = (uint8_t *)calloc(32, PCE_BANK_SIZE);
    for (size_t i = 0; i < sizeof(prog) / sizeof(prog[0]); i++) {
        const char *h = prog[i].hex;
        for (uint16_t a = prog[i].addr; *h; a++, h += (h[2] ? 3 : 2))
            rom[a & 0x1FFF] = (uint8_t)strtol((char[]){ h[0], h[1], 0 }, NULL, 16);
    }

    static Pce pce; /* too large for the stack (Vce.frame_index) */
    memset(&pce, 0, sizeof(pce));
    pce_load_rom_memory(&pce, rom, 32 * PCE_BANK_SIZE);
    pce_reset(&pce);
    Coverage cov;
    CHECK(coverage_init(&cov) == 0, "coverage_init");
    pce.coverage = &cov;
    for (int i = 0; i < 3000; i++)
        pce_step(&pce);

    /* Page 7 ($E000-$FFFF) with bank 0: physical address = offset. */
    CHECK(coverage_has_exec(&cov, 7, 0x0000) && coverage_has_exec(&cov, 7, 0x0030) &&
          coverage_has_exec(&cov, 7, 0x0050) && coverage_has_exec(&cov, 7, 0x005E) &&
          coverage_has_exec(&cov, 7, 0x0070) && coverage_has_exec(&cov, 7, 0x0073),
          "executed instructions should be marked");
    CHECK(!coverage_has_exec(&cov, 7, 0x0040) && !coverage_has_exec(&cov, 7, 0x0001 + 0x10),
          "data and mid-instruction bytes should not be marked");
    CHECK(coverage_has_edge(&cov, 'C', 7, 0x000D, 7, 0x0030), "JSR $E030");
    CHECK(coverage_has_edge(&cov, 'R', 7, 0x0030, 7, 0x0010), "RTS -> $E010");
    CHECK(coverage_has_edge(&cov, 'J', 7, 0x0010, 7, 0x0020), "JMP $E020");
    CHECK(coverage_has_edge(&cov, 'N', 7, 0x0020, 7, 0x0050), "JMP ($E040) -> $E050");
    CHECK(coverage_has_edge(&cov, 'I', 7, 0x005E, 7, 0x0070), "timer interrupt during BRA");
    CHECK(coverage_has_edge(&cov, 'T', 7, 0x0073, 7, 0x005E), "RTI back to BRA");
    CHECK(cov.edge_count == 6, "there should be exactly 6 edges, got %zu", cov.edge_count);

    coverage_free(&cov);
    free(rom);
    pce_free(&pce);
}

/* Instructions in the last bytes of a window record the bank of the next window (type S). */
static void test_coverage_span(void)
{
    printf("coverage: instructions at the end of a window\n");
    uint8_t *rom = (uint8_t *)calloc(32, PCE_BANK_SIZE);
    static const uint8_t main_code[] = { 0xA9, 0x01, 0x53, 0x40, 0x4C, 0xFA, 0xDF }; /* MPR6=1; JMP $DFFA */
    static const uint8_t end_code[] = { 0xEA, 0xEA, 0x4C, 0x00, 0xE0 };             /* NOP NOP JMP $E000 */
    memcpy(rom, main_code, sizeof(main_code));
    memcpy(rom + 1 * PCE_BANK_SIZE + 0x1FFA, end_code, sizeof(end_code));
    rom[0x1FFE] = 0x00;
    rom[0x1FFF] = 0xE0;

    static Pce pce; /* too large for the stack (Vce.frame_index) */
    memset(&pce, 0, sizeof(pce));
    pce_load_rom_memory(&pce, rom, 32 * PCE_BANK_SIZE);
    pce_reset(&pce);
    Coverage cov;
    coverage_init(&cov);
    pce.coverage = &cov;
    for (int i = 0; i < 40; i++)
        pce_step(&pce);

    const uint32_t b1 = 1u << 13;
    CHECK(coverage_has_edge(&cov, 'S', 6, b1 | 0x1FFA, 7, 0) && coverage_has_edge(&cov, 'S', 6, b1 | 0x1FFB, 7, 0) &&
          coverage_has_edge(&cov, 'S', 6, b1 | 0x1FFC, 7, 0), "S for the 3 instructions at $DFFA-$DFFC");
    CHECK(coverage_has_edge(&cov, 'J', 7, 0x0004, 6, b1 | 0x1FFA), "JMP $DFFA");
    CHECK(cov.edge_count == 5, "there should be 5 edges, got %zu", cov.edge_count);
    coverage_free(&cov);
    free(rom);
    pce_free(&pce);
}

/* Hand-written "recompiled code" for the $E000 program: INX / BRA $E000.
 * g_fake_bug = 1 makes INX add 2 (a deliberate bug) for the lockstep to catch. */
static int g_fake_bug;

static void fake_recompiled(HuC6280 *c)
{
    if (c->pc == 0xE001)
        goto bra;
    for (;;) {
        {
            int t = hu_pre(c, 0xE8, 0xE001);
            int x;
            hu_op_E8(c, 0, 0, 0, 0, 0, 0, t, &x);
            if (g_fake_bug)
                c->x++;
            if (hu_post(c, x))
                return;
        }
    bra: {
            int t = hu_pre(c, 0x80, 0xE003); /* BRA is 2 bytes: E001 + 2 = E003; E003 - 3 = E000 */
            int x;
            hu_op_80(c, 0xFD, 0, 0, 0, 0, 0, t, &x);
            if (hu_post(c, x))
                return;
        }
    }
}

static HuRecompFn fake_lookup(int page, int bank, int offset)
{
    return (page == 7 && bank == 0 && offset <= 1) ? fake_recompiled : NULL;
}

static int run_lockstep(int bug, Lockstep *ls)
{
    static Pce test, ref;
    uint8_t *rom = (uint8_t *)calloc(32, PCE_BANK_SIZE);
    rom[0] = 0xE8; /* INX */
    rom[1] = 0x80; /* BRA $E000 */
    rom[2] = 0xFD;
    rom[0x1FFE] = 0x00;
    rom[0x1FFF] = 0xE0;
    memset(&test, 0, sizeof(test));
    memset(&ref, 0, sizeof(ref));
    pce_load_rom_memory(&test, rom, 32 * PCE_BANK_SIZE);
    pce_load_rom_memory(&ref, rom, 32 * PCE_BANK_SIZE);
    pce_reset(&test);
    pce_reset(&ref);
    test.recomp_lookup = fake_lookup;
    lockstep_init(ls, &test, &ref);
    g_fake_bug = bug;
    for (int f = 0; f < 3 && !ls->diverged; f++) {
        pce_run_frame(&test, 1000000);
        lockstep_end_frame(ls);
    }
    int recomp_calls = (int)test.recomp_calls;
    free(rom);
    pce_free(&test);
    pce_free(&ref);
    return recomp_calls;
}

static void test_lockstep(void)
{
    printf("lockstep: recompiled code vs interpreter\n");
    static Lockstep ls;
    int calls = run_lockstep(0, &ls);
    CHECK(calls > 0, "the 'recompiled' code should have run");
    CHECK(!ls.diverged, "correct version should not diverge:\n%s", ls.report);
    CHECK(ls.frames == 3 && ls.instructions > 10000, "should compare 3 frames (%llu) and many instructions (%llu)",
          (unsigned long long)ls.frames, (unsigned long long)ls.instructions);

    run_lockstep(1, &ls);
    CHECK(ls.diverged, "the INX bug should be detected");
    CHECK(ls.instructions == 1, "should stop at the first compared instruction (stopped after %llu)",
          (unsigned long long)ls.instructions);
    CHECK(strstr(ls.report, "X=02") && strstr(ls.report, "X=01"), "report should show X=02 vs X=01:\n%s",
          ls.report);
}

/* Short beep on one channel with a game-like waveform (channel 6, ~1962 Hz) for a quarter of a second;
 * returns the left side in out, from sample 12000 on (once the output high-pass filter has settled). */
static int psg_beep(int filter, int16_t *out, int max)
{
    static const uint8_t wave[32] = { 0x1F, 0x1F, 0x1E, 0x1C, 0x1B, 0x19, 0x17, 0x16, 0x16, 0x16, 0x17,
                                      0x19, 0x1B, 0x1C, 0x1E, 0x1F, 0x01, 0x01, 0x02, 0x04, 0x05, 0x07,
                                      0x09, 0x0A, 0x0A, 0x0A, 0x09, 0x07, 0x06, 0x04, 0x02, 0x01 };
    static Psg p;
    static int16_t frame[PSG_BUFFER_SIZE];
    psg_reset(&p);
    psg_set_filter(&p, filter);
    psg_write(&p, 1, 0xFF);
    psg_write(&p, 0, 5);
    psg_write(&p, 4, 0x40);
    psg_write(&p, 4, 0x00);
    for (int i = 0; i < 32; i++)
        psg_write(&p, 6, wave[i]);
    psg_write(&p, 2, 0x39);
    psg_write(&p, 3, 0x00);
    psg_write(&p, 5, 0xFF);
    psg_write(&p, 4, 0x9D);
    int n = 0, skip = 12000;
    while (n < max) {
        for (int c = 0; c < 357954; c += 114)
            psg_clock(&p, 114);
        int count = psg_end_frame(&p, frame);
        for (int s = 0; s + 1 < count && n < max; s += 2) {
            if (skip > 0)
                skip--;
            else
                out[n++] = frame[s];
        }
    }
    return n;
}

/* Energy outside the beep harmonics (below 22 kHz) relative to the harmonics, in dB: least squares
 * with the DC level and the 11 harmonics (sine and cosine). The aliasing hiss is what remains. */
static double beep_noise_db(const int16_t *x, int n, double *rms)
{
    enum { H = 11, U = 2 * H + 1 };
    const double f0 = 21477273.0 / 6.0 / (32.0 * 0x39), pi = 3.14159265358979323846;
    static double a[U][U + 1];
    memset(a, 0, sizeof(a));
    double basis[U], total = 0.0;
    for (int s = 0; s < n; s++) {
        basis[0] = 1.0;
        for (int k = 1; k <= H; k++) {
            basis[2 * k - 1] = cos(2.0 * pi * k * f0 * s / 44100.0);
            basis[2 * k] = sin(2.0 * pi * k * f0 * s / 44100.0);
        }
        for (int r = 0; r < U; r++) {
            for (int c = 0; c < U; c++)
                a[r][c] += basis[r] * basis[c];
            a[r][U] += basis[r] * x[s];
        }
        total += (double)x[s] * x[s];
    }
    for (int c = 0; c < U; c++) /* Gaussian elimination (the matrix is symmetric and well conditioned) */
        for (int r = 0; r < U; r++)
            if (r != c) {
                double f = a[r][c] / a[c][c];
                for (int k = c; k <= U; k++)
                    a[r][k] -= f * a[c][k];
            }
    double resid = 0.0;
    for (int s = 0; s < n; s++) {
        double fit = a[0][U] / a[0][0];
        for (int k = 1; k <= H; k++)
            fit += a[2 * k - 1][U] / a[2 * k - 1][2 * k - 1] * cos(2.0 * pi * k * f0 * s / 44100.0) +
                   a[2 * k][U] / a[2 * k][2 * k] * sin(2.0 * pi * k * f0 * s / 44100.0);
        resid += (x[s] - fit) * (x[s] - fit);
    }
    *rms = sqrt(total / n);
    return 10.0 * log10(resid / (total - resid));
}

/* The chip filter removes the hiss (harmonics above 22 kHz folding back as noise) without changing the volume */
static void test_psg_filter(void)
{
    static int16_t raw[8192], filtered[8192];
    int n = psg_beep(0, raw, 8192);
    CHECK(psg_beep(1, filtered, 8192) == n, "chip filter: different sizes");
    double rms_raw, rms_filtered;
    double noise_raw = beep_noise_db(raw, n, &rms_raw);
    double noise_filtered = beep_noise_db(filtered, n, &rms_filtered);
    double level = 20.0 * log10(rms_filtered / rms_raw);
    printf("chip filter: hiss %.1f dB without the filter, %.1f dB with it; volume %+.2f dB\n", noise_raw, noise_filtered,
           level);
    CHECK(noise_raw > -15.0, "without the filter the hiss should show up (%.1f dB)", noise_raw);
    CHECK(noise_filtered < -40.0, "with the filter, hiss %.1f dB (expected below -40)", noise_filtered);
    CHECK(level > -1.0 && level < 1.0, "the filter changed the volume by %.2f dB", level);
}

/* Input script (tools/trace/input_script.h): buttons, game events ("# <name> <frame> <numbers>") read in
 * frame order and by name, plain comments ignored, and saving then loading again gives the same result. */
static void test_input_script(void)
{
    const char *path = "pce_tests_input.txt";
    FILE *f = fopen(path, "w");
    CHECK(f != NULL, "could not create %s", path);
    if (!f)
        return;
    fputs("# game: test\n# frames: 900\n# marks (F12): 10 20\n0 -\n# option 5 1\n"
          "# menu 40 3 735\n100 RUN\n# option 30 0\n120 I+UP\n# menu 200 1 512\n300 -\n", f);
    fclose(f);
    for (int round = 0; round < 2; round++) {
        InputScript s;
        CHECK(input_script_load(&s, path) == 0, "script did not load (round %d)", round);
        CHECK(s.count == 4 && s.ev_count == 4, "round %d: %d buttons and %d events (expected 4 and 4)", round, s.count,
              s.ev_count);
        CHECK(input_script_update(&s, 99, 0xFF) == 0 && input_script_update(&s, 120, 0) == 0x11,
              "round %d: wrong buttons", round);
        InputEvent e;
        CHECK(!input_script_event(&s, "option", 4, &e), "round %d: event before its frame", round);
        CHECK(input_script_event(&s, "option", 100, &e) && e.frame == 5 && e.n == 1 && e.v[0] == 1,
              "round %d: 1st option event", round);
        CHECK(input_script_event(&s, "menu", 100, &e) && e.frame == 40 && e.n == 2 && e.v[0] == 3 && e.v[1] == 735,
              "round %d: 1st menu event", round);
        CHECK(input_script_event(&s, "option", 100, &e) && e.frame == 30 && e.v[0] == 0, "round %d: 2nd option event",
              round);
        CHECK(!input_script_event(&s, "option", 1000, &e) && !input_script_event(&s, "menu", 199, &e),
              "round %d: extra event", round);
        CHECK(input_script_event(&s, "menu", 200, &e) && e.v[0] == 1 && e.v[1] == 512, "round %d: 2nd menu event",
              round);
        if (round == 0)
            CHECK(input_script_save(&s, path, "# saved again\n") == 0, "could not save the script");
        input_script_free(&s);
    }
    remove(path);
}

int main(void)
{
    test_input_script();
    test_psg_filter();
    test_rom_mirroring();
    test_ram_and_unmapped();
    test_pad_port();
    test_cpu_on_bus();
    test_vram_and_palette();
    test_coverage();
    test_coverage_span();
    test_lockstep();
    printf(g_fails ? "\nRESULT: FAILED (%d)\n" : "\nRESULT: OK\n", g_fails);
    return g_fails ? 1 : 0;
}
