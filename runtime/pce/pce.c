/*
 * Console (HuCard): memory map and I/O page. See pce.h.
 */
#include "pce/pce.h"

#include "cd/cdrom.h"
#include "cd/origin.h"
#include "cpu/recomp_sig.h"
#include "bios/hle.h"
#include "bios/synth.h"
#include "pce/bios_profile.h"
#include "pce/coverage.h"
#include "util/u8file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TIMER_DIVISOR (1024 * 3) /* master cycles per timer step */

/* ---- ROM ---- */

static void map_rom(Pce *pce)
{
    size_t banks = (pce->rom_size + PCE_BANK_SIZE - 1) / PCE_BANK_SIZE;
    for (size_t i = 0; i < 0x80; i++) {
        size_t bank;
        if (banks == 0x30) {
            /* 384 KB: 256 KB in $00-$3F (mirrored) and 128 KB in $40-$7F (mirrored). */
            bank = i < 0x40 ? (i & 0x1F) : 0x20 + (i & 0x0F);
        } else if (banks == 0x40) {
            bank = i < 0x40 ? (i & 0x3F) : 0x20 + (i & 0x1F);
        } else if (banks == 0x60) {
            bank = i < 0x40 ? (i & 0x3F) : 0x40 + (i & 0x1F);
        } else {
            bank = i % banks;
        }
        pce->rom_map[i] = pce->rom + bank * PCE_BANK_SIZE;
    }
}

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

int pce_load_rom_memory(Pce *pce, const uint8_t *data, size_t size)
{
    if (size % PCE_BANK_SIZE == 512) { /* copier header */
        data += 512;
        size -= 512;
    }
    if (size == 0 || size > 0x80 * PCE_BANK_SIZE)
        return -1;

    /* Rounds up to whole banks; whatever is left over stays $FF. */
    size_t banks = (size + PCE_BANK_SIZE - 1) / PCE_BANK_SIZE;
    free(pce->rom);
    pce->rom = (uint8_t *)malloc(banks * PCE_BANK_SIZE);
    if (!pce->rom)
        return -1;
    memset(pce->rom, 0xFF, banks * PCE_BANK_SIZE);
    memcpy(pce->rom, data, size);
    pce->rom_size = size;
    pce->rom_crc32 = crc32(data, size);
    map_rom(pce);
    return 0;
}

int pce_load_rom_file(Pce *pce, const char *path)
{
    FILE *f = u8_fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        return -1;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    int ok = buf && fread(buf, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    int r = ok ? pce_load_rom_memory(pce, buf, (size_t)n) : -1;
    free(buf);
    return r;
}

int pce_load_cd(Pce *pce, const char *bios_path, const char *cue_path)
{
    if (!bios_path) {
        /* Without the System Card: synthetic BIOS and all routines in C (runtime/bios) */
        uint8_t *rom = (uint8_t *)malloc(BIOS_SYNTH_SIZE);
        Hle *hle = (Hle *)malloc(sizeof(Hle));
        if (!rom || !hle) {
            free(rom);
            free(hle);
            return -1;
        }
        bios_synth_build(rom);
        int r = pce_load_rom_memory(pce, rom, BIOS_SYNTH_SIZE);
        free(rom);
        if (r != 0) {
            free(hle);
            return -1;
        }
        hle_init(hle, 0);
        hle_enable_synthetic(hle);
        pce->hle = hle;
        pce->hle_owned = 1;
    } else if (pce_load_rom_file(pce, bios_path) != 0) {
        fprintf(stderr, "could not load the BIOS %s\n", bios_path);
        return -1;
    }
    CdImage *img = (CdImage *)malloc(sizeof(CdImage));
    CdRom *cd = (CdRom *)malloc(sizeof(CdRom));
    if (!img || !cd || cd_open(img, cue_path) != 0) {
        fprintf(stderr, "could not open the CD %s: %s\n", cue_path, img ? img->error : "out of memory");
        free(img);
        free(cd);
        return -1;
    }
    cdrom_init(cd, img, &pce->cpu, &pce->master_cycles);
    pce->cd = cd;
    pce->cd_image = img;
    return 0;
}

int pce_load_bram(Pce *pce, const char *path)
{
    if (!pce->cd)
        return -1;
    FILE *f = u8_fopen(path, "rb");
    if (!f)
        return -1;
    uint8_t data[CD_BRAM_SIZE];
    size_t n = fread(data, 1, sizeof(data), f);
    int extra = fgetc(f) != EOF;
    fclose(f);
    if (n != sizeof(data) || extra)
        return -1;
    memcpy(pce->cd->bram, data, sizeof(data));
    return 0;
}

int pce_save_bram(const Pce *pce, const char *path)
{
    if (!pce->cd)
        return -1;
    FILE *f = u8_fopen(path, "wb");
    if (!f)
        return -1;
    size_t n = fwrite(pce->cd->bram, 1, CD_BRAM_SIZE, f);
    return (fclose(f) == 0 && n == CD_BRAM_SIZE) ? 0 : -1;
}

void pce_free(Pce *pce)
{
    free(pce->rom);
    pce->rom = NULL;
    pce->rom_size = 0;
    if (pce->cd_image)
        cd_close(pce->cd_image);
    free(pce->cd_image);
    free(pce->cd);
    pce->cd_image = NULL;
    pce->cd = NULL;
    if (pce->hle_owned) {
        free(pce->hle);
        pce->hle = NULL;
        pce->hle_owned = 0;
    }
}

/* ---- System clock ---- */

static void clock_timer(Pce *pce, uint32_t cycles)
{
    if (!pce->timer_enabled)
        return;
    pce->timer_cycles -= (int32_t)cycles;
    if (pce->timer_cycles <= 0) {
        pce->timer_cycles += TIMER_DIVISOR;
        if (pce->timer_counter == 0) {
            pce->timer_counter = pce->timer_reload;
            pce->cpu.irq_request |= HU_TIQ;
        } else {
            pce->timer_counter--;
        }
    }
}

/* Advances all the hardware (except the CPU) by N master cycles. While the CPU has a
 * pending VRAM access, it steps by 3 so the VDC finds the right window. */
void pce_sync_video(Pce *pce)
{
    if (pce->video_pending) {
        uint32_t cycles = pce->video_pending;
        pce->video_pending = 0;
        if (vce_clock(&pce->vce, &pce->vdc, cycles)) {
            pce->frame_ready = 1;
            pce->cpu.yield = 1; /* the recompiled code returns control at the end of the instruction */
        }
    }
    pce->video_quiet = vce_quiet_cycles(&pce->vce, &pce->vdc);
}

static void clock_hardware(void *ctx, uint32_t cycles)
{
    Pce *pce = (Pce *)ctx;
    while (cycles > 0) {
        if (!vdc_has_pending_access(&pce->vdc)) {
            pce->master_cycles += cycles;
            clock_timer(pce, cycles);
            /* Video runs along when it reaches a point where something happens (scanline event, phase,
             * transfer, HSYNC); before that, it only accumulates (same result, fewer calls). */
            pce->video_pending += cycles;
            if (pce->video_pending >= pce->video_quiet)
                pce_sync_video(pce);
            if (pce->cd)
                cdrom_clock(pce->cd, cycles);
            psg_clock(&pce->psg, cycles);
            break;
        }
        pce_sync_video(pce);
        uint32_t step = cycles > 3 ? 3 : cycles;
        pce->master_cycles += step;
        clock_timer(pce, step);
        vdc_process_cpu_access(&pce->vdc, step);
        if (vce_clock(&pce->vce, &pce->vdc, step)) {
            pce->frame_ready = 1;
            pce->cpu.yield = 1;
        }
        pce->video_quiet = vce_quiet_cycles(&pce->vce, &pce->vdc);
        if (pce->cd)
            cdrom_clock(pce->cd, step);
        psg_clock(&pce->psg, step);
        cycles -= step;
    }
}

/* ---- I/O page (bank $FF) ---- */

static uint8_t read_pad(Pce *pce)
{
    uint8_t v = 0x30;
    if (pce->region_japan)
        v |= 0x40;
    if (!pce->cd_attached)
        v |= 0x80;
    /* Active-low lines: SEL=1 -> directions, SEL=0 -> buttons.
     * With CLR=1 the four lines read 0 (Geargrafx input_inline.h, WriteO). */
    if (pce->pad_clr)
        return v;
    uint8_t pressed = pce->pad_sel ? (uint8_t)(pce->pad_buttons >> 4) : (uint8_t)(pce->pad_buttons & 0x0F);
    return (uint8_t)(v | (~pressed & 0x0F));
}

static uint8_t read_timer(Pce *pce)
{
    if (pce->timer_counter == 0 && pce->timer_cycles <= 5 * 3)
        return 0x7F;
    return pce->timer_counter;
}

/* VDC/VCE accesses cost 1 extra CPU cycle (2 on the VDC during a block
 * transfer), and the hardware advances right away: before the read, after the write. */
static uint8_t io_read(Pce *pce, uint16_t offset)
{
    HuC6280 *cpu = &pce->cpu;
    int block = cpu->in_block_transfer;
    switch (offset & 0x1C00) {
    case 0x0000: {
        huc6280_add_cycles(cpu, block ? 2 : 1);
        pce_sync_video(pce);
        uint8_t r = vdc_read(&pce->vdc, offset);
        pce_sync_video(pce); /* the read can change the VDC (e.g. start a VRAM access) */
        return r;
    }
    case 0x0400:
        huc6280_add_cycles(cpu, 1);
        pce_sync_video(pce);
        return vce_read(&pce->vce, offset);
    case 0x0800: /* PSG: write-only; reads the last value on the bus */
        return block ? 0 : pce->io_buffer;
    case 0x0C00:
        if (block)
            return 0;
        pce->io_buffer = (uint8_t)((read_timer(pce) & 0x7F) | (pce->io_buffer & 0x80));
        return pce->io_buffer;
    case 0x1000:
        if (block)
            return 0;
        pce->io_buffer = read_pad(pce);
        return pce->io_buffer;
    case 0x1400:
        if (block)
            return 0;
        if ((offset & 3) == 2)
            pce->io_buffer = (uint8_t)((cpu->irq_disable & 7) | (pce->io_buffer & 0xF8));
        else if ((offset & 3) == 3)
            pce->io_buffer = (uint8_t)((cpu->irq_request & 7) | (pce->io_buffer & 0xF8));
        return pce->io_buffer;
    case 0x1800:
        return pce->cd ? cdrom_read(pce->cd, offset) : 0xFF;
    default: /* $1C00 unused */
        return 0xFF;
    }
}

static void log_io(Pce *pce, uint16_t addr, uint8_t v)
{
    IoLog *log = pce->io_log;
    if (log->count < IO_LOG_MAX) {
        log->addr[log->count] = addr;
        log->value[log->count] = v;
    }
    log->count++;
}

static void io_write(Pce *pce, uint16_t offset, uint8_t v)
{
    HuC6280 *cpu = &pce->cpu;
    if (pce->io_log)
        log_io(pce, offset, v);
    switch (offset & 0x1C00) {
    case 0x0000:
        pce_sync_video(pce);
        vdc_write(&pce->vdc, offset, v);
        pce_sync_video(pce);
        pce->vdc_writes++;
        huc6280_add_cycles(cpu, cpu->in_block_transfer ? 2 : 1);
        break;
    case 0x0400:
        pce_sync_video(pce);
        vce_write(&pce->vce, offset, v);
        pce_sync_video(pce);
        pce->vce_writes++;
        huc6280_add_cycles(cpu, 1);
        break;
    case 0x0800:
        huc6280_clock_pending(cpu); /* the sound needs to know the exact moment */
        psg_write(&pce->psg, offset, v);
        pce->io_buffer = v;
        break;
    case 0x0C00:
        if (offset & 1) {
            int enabled = v & 1;
            if (enabled != pce->timer_enabled) {
                pce->timer_enabled = enabled;
                pce->timer_cycles = TIMER_DIVISOR;
                if (enabled)
                    pce->timer_counter = pce->timer_reload;
            }
        } else {
            pce->timer_reload = v & 0x7F;
        }
        pce->io_buffer = v;
        break;
    case 0x1000:
        pce->pad_sel = v & 1;
        pce->pad_clr = (v >> 1) & 1;
        pce->io_buffer = v;
        break;
    case 0x1400:
        if ((offset & 3) == 2)
            cpu->irq_disable = v & 7;
        else if ((offset & 3) == 3)
            cpu->irq_request &= (uint8_t)~HU_TIQ; /* acknowledges the timer interrupt */
        pce->io_buffer = v;
        break;
    case 0x1800:
        if (pce->cd)
            cdrom_write(pce->cd, offset, v);
        break;
    default:
        break;
    }
}

/* ---- Bus ---- */

uint8_t pce_bus_read(Pce *pce, uint32_t addr)
{
    uint8_t bank = (uint8_t)(addr >> 13);
    uint16_t offset = (uint16_t)(addr & 0x1FFF);
    if (bank < 0x80)
        return pce->rom_map[bank][offset];
    if (bank >= PCE_RAM_BANK && bank <= 0xFB)
        return pce->ram[offset];
    if (bank == PCE_IO_BANK)
        return io_read(pce, offset);
    if (pce->cd) {
        if (bank >= 0x80 && bank <= 0x87)
            return pce->cd->ram[(bank - 0x80) * PCE_BANK_SIZE + offset];
        /* Save RAM: 2 KB; the rest of the bank reads $FF. Locked = empty bank. */
        if (bank == 0xF7 && pce->cd->bram_enabled && offset < CD_BRAM_SIZE)
            return pce->cd->bram[offset];
    }
    return 0xFF;
}

void pce_bus_write(Pce *pce, uint32_t addr, uint8_t value)
{
    uint8_t bank = (uint8_t)(addr >> 13);
    uint16_t offset = (uint16_t)(addr & 0x1FFF);
    if (bank >= PCE_RAM_BANK && bank <= 0xFB) {
        pce->ram[offset] = value;
        pce->ram_gen[256 + (offset >> 8)]++;
    } else if (bank == PCE_IO_BANK) {
        io_write(pce, offset, value);
    } else if (pce->cd && bank >= 0x80 && bank <= 0x87) {
        pce->cd->ram[(bank - 0x80) * PCE_BANK_SIZE + offset] = value;
        pce->ram_gen[(bank - 0x80) * 32 + (offset >> 8)]++;
    }
    else if (pce->cd && bank == 0xF7 && pce->cd->bram_enabled && offset < CD_BRAM_SIZE)
        pce->cd->bram[offset] = value;
}

static uint8_t cpu_read(void *ctx, uint32_t addr)
{
    Pce *pce = (Pce *)ctx;
    uint8_t v = pce_bus_read(pce, addr);
    if (pce->origin)
        origin_read(pce->origin, pce, addr, v);
    if (pce->bios_reads && (addr >> 13) < 0x20 && pce->cpu.mpr[pce->cpu.pc >> 13] >= 0x20)
        pce->bios_reads[addr & 0x3FFFF] = 1;
    return v;
}

static void cpu_write(void *ctx, uint32_t addr, uint8_t value)
{
    Pce *pce = (Pce *)ctx;
    if (pce->origin)
        origin_write(pce->origin, pce, addr, value);
    if (pce->bios_profile)
        bios_profile_write(pce->bios_profile, addr, value);
    pce_bus_write(pce, addr, value);
}

/* ST0/ST1/ST2: the VDC receives the value 2 cycles after the start; the instruction has
 * one VDC wait cycle (5 in total). */
static void cpu_vdc_write(void *ctx, int port, uint8_t value)
{
    Pce *pce = (Pce *)ctx;
    if (pce->bios_profile)
        bios_profile_vdc(pce->bios_profile, port, value);
    if (pce->io_log)
        log_io(pce, (uint16_t)(0x2000 + port), value);
    huc6280_clock_counted(&pce->cpu, 2);
    pce_sync_video(pce);
    vdc_write(&pce->vdc, (uint16_t)port, value);
    pce_sync_video(pce);
    pce->vdc_writes++;
    huc6280_add_cycles(&pce->cpu, 1);
}

/* Before/after each instruction (interpreted or recompiled): coverage and observer. */
static void cpu_hook(void *ctx, int after)
{
    Pce *pce = (Pce *)ctx;
    if (pce->game_patch && !after)
        pce->game_patch(pce);
    if (pce->coverage) {
        if (after)
            coverage_after(pce->coverage, pce);
        else
            coverage_before(pce->coverage, pce);
    }
    if (pce->bios_profile) {
        if (after)
            bios_profile_after(pce->bios_profile, pce);
        else
            bios_profile_before(pce->bios_profile, pce);
    }
    if (pce->on_instruction)
        pce->on_instruction(pce, after);
}

void pce_reset(Pce *pce)
{
    HuC6280 *c = &pce->cpu;
    c->ctx = pce;
    c->read = cpu_read;
    c->write = cpu_write;
    c->vdc_write = cpu_vdc_write;
    c->clock = clock_hardware;
    c->hook = cpu_hook;
    c->yield = 0;
    pce->io_buffer = 0xFF;
    pce->pad_sel = 1;
    pce->pad_clr = 1;
    pce->timer_enabled = 0;
    pce->timer_reload = 0;
    pce->timer_counter = 0;
    pce->timer_cycles = 0;
    pce->master_cycles = 0;
    pce->frame_ready = 0;
    vce_reset(&pce->vce);
    psg_reset(&pce->psg);
    pce->audio_count = 0;
    vdc_reset(&pce->vdc, &pce->vce, c);
    pce->video_pending = 0;
    pce->video_quiet = vce_quiet_cycles(&pce->vce, &pce->vdc);
    huc6280_reset(c);
    /* All of RAM changed: invalidates the stored signatures of the recompiled code (HuRamSig). */
    static uint32_t epoch;
    pce->ram_epoch = ++epoch;
    for (int i = 0; i < PCE_RAM_GEN_PAGES; i++)
        pce->ram_gen[i]++;
    pce->cd_attached = pce->cd != NULL;
    if (pce->cd) {
        cdrom_reset(pce->cd);
        pce->psg.extra_clock = cdrom_clock_audio;
        pce->psg.extra_sample = cdrom_sample_audio;
        pce->psg.extra_ctx = pce->cd;
    }
}

int pce_step(Pce *pce)
{
    HuC6280 *c = &pce->cpu;
    uint64_t before = pce->master_cycles;
    if (pce->hle) {
        /* BIOS routine rewritten in C (bios/hle.h): a pseudo-instruction. */
        HleFn hle = hle_find(pce->hle, pce);
        if (hle) {
            /* The observer (lockstep) sees the pseudo-instruction as one instruction */
            if (pce->on_instruction)
                pce->on_instruction(pce, 0);
            pce->hle->cycles = 0;
            pce->hle->ram_free = 0;
            pce->hle->calls++;
            huc6280_hle_begin(c);
            if (c->pc < 0xE100)
                huc6280_add_cycles(c, 4); /* the JMP in the entry table */
            hle(pce);
            huc6280_hle_end(c, pce->hle->cycles);
            if (pce->on_instruction)
                pce->on_instruction(pce, 1);
            return (int)(pce->master_cycles - before);
        }
    }
    void (*fn)(HuC6280 *) = NULL;
    if (pce->recomp_lookup) {
        int page = c->pc >> 13;
        fn = pce->recomp_lookup(page, c->mpr[page], c->pc & 0x1FFF);
    }
    if (fn) {
        /* Recompiled code: runs until it leaves the function, enters an interrupt or c->yield. */
        uint64_t cycles = c->cycles;
        c->yield = 0;
        pce->recomp_calls++;
        fn(c);
        if (c->cycles == cycles) { /* executed nothing (e.g. the next window's bank is different) */
            pce->interpreted_steps++;
            if (pce->interp_hist)
                pce->interp_hist[c->mpr[c->pc >> 13] * 0x2000u + (c->pc & 0x1FFF)]++;
            huc6280_step(c);
        }
    } else {
        pce->interpreted_steps++;
        if (pce->interp_hist)
            pce->interp_hist[c->mpr[c->pc >> 13] * 0x2000u + (c->pc & 0x1FFF)]++;
        huc6280_step(c);
    }
    return (int)(pce->master_cycles - before);
}

int pce_run_frame(Pce *pce, long max_instructions)
{
    pce->frame_ready = 0;
    vdc_log_open(&pce->vdc);
    for (long i = 0; i < max_instructions && !pce->frame_ready; i++)
        pce_step(pce);
    int done = pce->frame_ready;
    pce->frame_ready = 0;
    if (done)
        pce_end_frame_audio(pce);
    return done;
}

/* PSG channels carrying the BIOS sound driver's effects program (its variables in RAM, the same in the
 * real BIOS and in the C one, bios/hle_psg.c): $22DF = programs on (0 = music only, 1 = effects only,
 * 2 = both); $2607+x = 1 when the effect has channel x. Games that do not use the driver: everything is music. */
static uint8_t driver_sfx_channels(void *ctx)
{
    const Pce *pce = (const Pce *)ctx;
    if (pce->ram[0x2DF] == 0)
        return 0;
    uint8_t mask = 0;
    for (int x = 0; x < 6; x++)
        if (pce->ram[0x607 + x] == 0x01)
            mask |= (uint8_t)(1 << x);
    return mask;
}

void pce_end_frame_audio(Pce *pce)
{
    if (pce->mix.on) {
        pce->audio_count = psg_end_frame_split(&pce->psg, pce->audio, pce->mix.psg_music, pce->mix.psg_sfx);
        if (pce->cd)
            cdrom_mix_audio(pce->cd, pce->audio, pce->audio_count, &pce->mix);
    } else {
        pce->audio_count = psg_end_frame(&pce->psg, pce->audio);
        if (pce->cd)
            cdrom_mix_audio(pce->cd, pce->audio, pce->audio_count, NULL);
    }
    /* Music x effects separation only when the mix is on (takes effect from the next frame) */
    pce->psg.sfx_channels = pce->mix.on && pce->cd ? driver_sfx_channels : NULL;
    psg_set_filter(&pce->psg, pce->mix.on && pce->mix.chip_filter);
    pce->psg.sfx_ctx = pce;
}

static uint64_t sig_sum(const Pce *pce, const HuRamSig *sig)
{
    uint64_t sum = (uint64_t)pce->ram_epoch << 40;
    for (uint16_t i = 0; i < sig->page_count; i++)
        sum += pce->ram_gen[sig->pages[i]];
    return sum;
}

int recomp_ram_sig_changed(void *ctx, const HuRamSig *sig)
{
    return sig_sum((const Pce *)ctx, sig) != sig->checked_sum;
}

uint8_t recomp_ram_byte(void *ctx, uint32_t phys)
{
    return pce_bus_read((Pce *)ctx, phys);
}

/* Recompiled code in RAM (huc6280_recomp.h): does the RAM hold the signature's bytes? */
int recomp_ram_sig_ok(void *ctx, HuRamSig *sig)
{
    Pce *pce = (Pce *)ctx;
    uint64_t sum = sig_sum(pce, sig);
    if (sum == sig->checked_sum)
        return sig->checked_ok;
    int ok = 1;
    for (uint16_t r = 0; r < sig->run_count && ok; r++) {
        const HuRamRun *run = &sig->runs[r];
        for (uint16_t k = 0; k < run->len; k++) {
            if (pce_bus_read(pce, run->phys + k) != run->bytes[k]) {
                ok = 0;
                break;
            }
        }
    }
    sig->checked_sum = sum;
    sig->checked_ok = ok;
    return ok;
}

int pce_clone(Pce *dst, const Pce *src)
{
    memcpy(dst, src, sizeof(*dst));
    dst->coverage = NULL;
    dst->origin = NULL;
    dst->bios_profile = NULL;
    dst->hle = NULL;
    dst->io_log = NULL;
    dst->on_instruction = NULL;
    dst->bios_reads = NULL;
    dst->user = NULL;
    dst->cpu.ctx = dst;
    dst->vdc.vce = &dst->vce;
    dst->vdc.cpu = &dst->cpu;
    if (src->cd) {
        CdRom *cd = (CdRom *)malloc(sizeof(CdRom));
        if (!cd)
            return -1;
        memcpy(cd, src->cd, sizeof(*cd));
        cd->cpu = &dst->cpu;
        cd->master_cycles = &dst->master_cycles;
        cd->scsi.cd = cd;
        cd->audio.cd = cd;
        cd->adpcm.cd = cd;
        dst->cd = cd;
        dst->psg.extra_ctx = cd;
    }
    return 0;
}

void pce_free_clone(Pce *pce)
{
    free(pce->cd); /* the ROM and the CD image belong to the original */
    pce->cd = NULL;
}

void pce_advance_hardware(Pce *pce, uint64_t master_cycles)
{
    /* Steps the size of an instruction: the CD and the ADPCM handle one event per step. */
    while (master_cycles > 0) {
        uint32_t step = master_cycles > 12 ? 12 : (uint32_t)master_cycles;
        clock_hardware(pce, step);
        master_cycles -= step;
    }
}
