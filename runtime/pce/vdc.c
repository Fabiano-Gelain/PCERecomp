/*
 * VDC (HuC6270). See vdc.h.
 */
#include "pce/vdc.h"

#include <stdlib.h>
#include <string.h>

enum { V_VDS, V_VDW, V_VCR, V_VSW };
enum { H_HDS, H_HDW, H_HDE, H_HSW };
enum { EV_NONE, EV_BYR, EV_BXR, EV_HDS, EV_RCR };

enum {
    R_MAWR = 0x00, R_MARR = 0x01, R_VWR = 0x02, R_CR = 0x05, R_RCR = 0x06, R_BXR = 0x07,
    R_BYR = 0x08, R_MWR = 0x09, R_HSR = 0x0A, R_HDR = 0x0B, R_VSR = 0x0C, R_VDR = 0x0D,
    R_VCR = 0x0E, R_DCR = 0x0F, R_SOUR = 0x10, R_DESR = 0x11, R_LENR = 0x12, R_DVSSR = 0x13,
};

#define ST_COLLISION 0x01
#define ST_OVERFLOW 0x02
#define ST_SCANLINE 0x04
#define ST_SAT_END 0x08
#define ST_VRAM_END 0x10
#define ST_VBLANK 0x20
#define ST_BUSY 0x40

#define CR_COLLISION 0x01
#define CR_OVERFLOW 0x02
#define CR_SCANLINE 0x04
#define CR_VBLANK 0x08

#define ACTIVE_DISPLAY_START 14
#define BOTTOM_BLANKING_START 256

static const uint16_t k_reg_mask[20] = {
    0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x1FFF, 0x03FF, 0x03FF, 0x01FF, 0x00FF,
    0x7F1F, 0x7F7F, 0xFF1F, 0x01FF, 0x00FF, 0x001F, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
};
static const int k_screen_tiles_x[8] = { 32, 64, 128, 128, 32, 64, 128, 128 };
static const int k_screen_tiles_y[8] = { 32, 32, 32, 32, 64, 64, 64, 64 };
static const int k_increment[4] = { 0x01, 0x20, 0x40, 0x80 };
static const int k_vram_read_delay[3] = { 24, 24, 15 };
static const int k_vram_write_delay[3] = { 21, 18, 12 };
#define BYR_LATCH_CLOCKS 36
static const int k_sprite_width[2] = { 16, 32 };
static const int k_sprite_height[4] = { 16, 32, 64, 64 };
static const int k_sprite_mask_width[2] = { 0xFFFF, 0xFFFE };
static const int k_sprite_mask_height[4] = { 0xFFFF, 0xFFFD, 0xFFF9, 0xFFF9 };

#define HSW(v) ((v)->reg[R_HSR] & 0x1F)
#define HDS(v) (((v)->reg[R_HSR] >> 8) & 0x7F)
#define HDW(v) ((v)->reg[R_HDR] & 0x7F)
#define HDE(v) (((v)->reg[R_HDR] >> 8) & 0x7F)
#define VSW(v) ((v)->reg[R_VSR] & 0x1F)
#define VDS(v) (((v)->reg[R_VSR] >> 8) & 0xFF)
#define VDW(v) ((v)->reg[R_VDR] & 0x1FF)
#define VCRV(v) ((v)->reg[R_VCR] & 0xFF)
#define CRV(v) ((v)->reg[R_CR] & 0x1FFF)

static void line_events(Vdc *v);
static void next_h_state(Vdc *v);
static void next_v_state(Vdc *v);

/* ---- Helpers ---- */

static int32_t hclock(const Vdc *v) { return v->vce->hpos; }
static int32_t divider(const Vdc *v) { return v->vce->clock_divider; }
static int32_t dots_to_clocks(const Vdc *v, int32_t dots) { return dots * divider(v); }

static void assert_irq1(Vdc *v, int on)
{
    if (on)
        v->cpu->irq_request |= HU_IRQ1;
    else
        v->cpu->irq_request &= (uint8_t)~HU_IRQ1;
}

static uint16_t read_vram(Vdc *v, uint16_t addr)
{
    if (addr < VDC_VRAM_SIZE)
        v->vram_openbus = v->vram[addr];
    return v->vram_openbus;
}

int vdc_has_pending_access(const Vdc *v)
{
    return v->pending_read || v->pending_write;
}

static void update_busy(Vdc *v)
{
    if (vdc_has_pending_access(v))
        v->status |= ST_BUSY;
    else
        v->status &= (uint16_t)~ST_BUSY;
}

/* ---- Interrupts ---- */

static void vblank_irq(Vdc *v)
{
    if (v->reg[R_CR] & CR_VBLANK) {
        v->status |= ST_VBLANK;
        assert_irq1(v, 1);
    }
    /* The sprite attribute table (SATB) copy happens at the start of VBlank. */
    if (v->trigger_sat_transfer || (v->reg[R_DCR] & 0x10)) {
        v->trigger_sat_transfer = 0;
        v->sat_transfer_pending = 1024;
        v->sat_copies++;
        v->sat_copy_base = v->reg[R_DVSSR];
    }
}

static void rcr_irq(Vdc *v)
{
    if ((v->reg[R_CR] & CR_SCANLINE) && ((int)v->reg[R_RCR] - 64) == v->raster_line) {
        v->status |= ST_SCANLINE;
        assert_irq1(v, 1);
    }
}

static void overflow_irq(Vdc *v)
{
    if (v->reg[R_CR] & CR_OVERFLOW) {
        v->status |= ST_OVERFLOW;
        assert_irq1(v, 1);
    }
}

static void collision_irq(Vdc *v)
{
    if (v->reg[R_CR] & CR_COLLISION) {
        v->status |= ST_COLLISION;
        assert_irq1(v, 1);
    }
}

/* ---- CPU access to VRAM ---- */

static void queue_read(Vdc *v)
{
    v->pending_read = 1;
    v->transfer_delay = k_vram_read_delay[vce_speed_index(v->vce)];
    update_busy(v);
}

static void queue_write(Vdc *v)
{
    v->pending_write = 1;
    v->transfer_delay = k_vram_write_delay[vce_speed_index(v->vce)];
    update_busy(v);
}

static void process_read(Vdc *v)
{
    v->read_buffer = read_vram(v, v->reg[R_MARR]);
    v->reg[R_MARR] = (uint16_t)(v->reg[R_MARR] + k_increment[(v->reg[R_CR] >> 11) & 3]);
    v->pending_read = 0;
    update_busy(v);
}

/* Smooth scrolling (Vdc.line_log): write to VRAM or to the sprites after the frame's 1st drawn scanline */
/* Where to record: the log, or the next frame's one if this one has already ended (VdcLineLog.closed; NULL if out of memory) */
static VdcLineLog *open_log(Vdc *v)
{
    VdcLineLog *log = v->line_log;
    if (log && log->closed) {
        if (!log->next) {
            log->next = (VdcLineLog *)calloc(1, sizeof(VdcLineLog));
            if (log->next)
                log->next->last_row = -1;
        }
        return log->next;
    }
    return log;
}

void vdc_log_close(Vdc *v)
{
    VdcLineLog *log = v->line_log;
    if (!log)
        return;
    log->closed = 1;
    if (log->next)
        log->next->last_row = -1;
}

void vdc_log_open(Vdc *v)
{
    VdcLineLog *log = v->line_log;
    if (!log || !log->closed)
        return;
    log->closed = 0;
    VdcLineLog *next = log->next;
    if (next && next->last_row >= 0) {
        memcpy(log, next, sizeof(*log));
        log->next = next;
        log->closed = 0;
        next->last_row = -1;
    }
}

static void log_write(Vdc *v, uint32_t addr, uint16_t value)
{
    VdcLineLog *log = open_log(v);
    if (!log || log->last_row < 0)
        return;
    if (log->write_count == VDC_LOG_WRITES) {
        log->overflow = 1;
        return;
    }
    VdcLogWrite *w = &log->writes[log->write_count++];
    w->addr = (uint16_t)addr;
    w->value = value;
    w->row = (int16_t)(log->last_row + 1);
}

static void process_write(Vdc *v)
{
    if (v->reg[R_MAWR] < VDC_VRAM_SIZE) {
        v->vram[v->reg[R_MAWR]] = v->reg[R_VWR];
        log_write(v, v->reg[R_MAWR], v->reg[R_VWR]);
        if (v->write_hook)
            v->write_hook(v->write_hook_ctx, v->reg[R_MAWR], v->reg[R_VWR], 0);
    }
    v->reg[R_MAWR] = (uint16_t)(v->reg[R_MAWR] + k_increment[(v->reg[R_CR] >> 11) & 3]);
    v->pending_write = 0;
    update_busy(v);
}

/* The CPU stalls (in steps of 3 master cycles) until the pending access finishes. */
static void wait_for_vram_access(Vdc *v)
{
    int guard = 0;
    while (vdc_has_pending_access(v)) {
        if (v->cpu->clock)
            huc6280_stall_fast_cycle(v->cpu);
        else
            vdc_process_cpu_access(v, 3);
        if (++guard > 1000000)
            break;
    }
}

static int32_t clocks_since_hsync_start(const Vdc *v, int32_t elapsed)
{
    int32_t h = hclock(v) + elapsed;
    if (h >= VCE_LINE_LENGTH)
        h -= VCE_LINE_LENGTH;
    int32_t e = h - v->hsync_start_clock;
    if (e < 0)
        e += VCE_LINE_LENGTH;
    return e;
}

static int in_bg_fetch_window(const Vdc *v, int32_t h)
{
    return !v->burst_mode && h >= v->load_bg_start_clock && h < v->load_bg_end_clock &&
           v->vpos >= ACTIVE_DISPLAY_START && v->vpos < BOTTOM_BLANKING_START;
}

static int bg_slot_allowed(const Vdc *v, int32_t h)
{
    int32_t bg_clock = h - v->load_bg_start_clock;
    if (bg_clock < 0)
        return 0;
    int32_t bg_dot = bg_clock / divider(v) - 1;
    if (bg_dot < 0)
        return 0;
    switch (v->latched_mwr & 3) {
    case 0: return (bg_dot & 1) == 0;
    case 1:
    case 2: return (bg_dot & 7) == 2 || (bg_dot & 7) == 3;
    default: return 0;
    }
}

static int slot_available(Vdc *v, int32_t elapsed)
{
    int32_t h = hclock(v);
    int32_t div = divider(v);
    int in_bg = in_bg_fetch_window(v, h);
    int sprites_on = (v->latched_cr & 0x40) != 0;
    int blocked;

    if (v->sat_transfer_pending > 0 || v->vram_transfer_pending > 0)
        return 0;

    if (v->v_state != V_VDW || v->burst_mode || ((!sprites_on || v->sprite_count == 0) && !in_bg)) {
        blocked = ((h / div) & 1) != 0;
    } else {
        int allow = in_bg && bg_slot_allowed(v, h);
        blocked = in_bg && !allow;
        if (!blocked && !in_bg && sprites_on) {
            int32_t clock_count = (h > v->load_bg_end_clock) ? (h - v->load_bg_end_clock)
                                                             : (VCE_LINE_LENGTH - v->load_bg_end_clock + h);
            int32_t dot_count = clock_count / div;
            int dots_per_sprite;
            switch ((v->latched_mwr >> 2) & 3) {
            case 2: dots_per_sprite = 8; break;
            case 3: dots_per_sprite = 16; break;
            default: dots_per_sprite = 4; break;
            }
            if (dot_count < v->sprite_count * dots_per_sprite)
                blocked = 1;
            else
                blocked = ((h / div) & 1) != 0;
        }
    }
    if (blocked)
        return 0;
    if (v->h_state == H_HSW && clocks_since_hsync_start(v, elapsed) < dots_to_clocks(v, 8))
        return 0;
    return 1;
}

void vdc_process_cpu_access(Vdc *v, uint32_t cycles)
{
    cycles += v->cpu_vram_cycles;
    while (vdc_has_pending_access(v) && cycles >= 3) {
        if (v->transfer_delay > 0) {
            v->transfer_delay -= 3;
            if (v->transfer_delay > 0) {
                cycles -= 3;
                continue;
            }
        }
        v->transfer_delay = 0;
        if (!slot_available(v, 3)) {
            cycles -= 3;
            continue;
        }
        if (v->pending_read)
            process_read(v);
        else if (v->pending_write)
            process_write(v);
        cycles -= 3;
    }
    v->cpu_vram_cycles = vdc_has_pending_access(v) ? cycles : 0;
}

/* ---- Scrolling ---- */

static void latch_scroll_y(Vdc *v)
{
    if (v->raster_line == 0) {
        v->bg_counter_y = v->reg[R_BYR];
    } else if (v->bg_scroll_y_update_pending) {
        v->bg_counter_y = v->reg[R_BYR];
        v->bg_scroll_y_update_pending = 0;
        v->bg_counter_y++;
    } else if (v->increment_bg_counter_y) {
        v->bg_counter_y++;
    }
    v->bg_offset_y = v->bg_counter_y;
    v->increment_bg_counter_y = 0;
}

static void refresh_scroll_y_current_line(Vdc *v)
{
    v->bg_offset_y = v->raster_line == 0 ? v->reg[R_BYR] : v->reg[R_BYR] + 1;
}

/* Did the write fall within the half-dot window in which the VDC is copying the value? */
static int check_latch_timing(const Vdc *v, int32_t clock)
{
    if (clock < 0)
        return 0;
    int32_t div = divider(v);
    int32_t start = (clock / div) * div;
    int32_t end = start + (div + 1) / 2;
    int32_t h = hclock(v);
    return h > start && h < end;
}

/* ---- Registers ---- */

uint8_t vdc_read(Vdc *v, uint16_t offset)
{
    switch (offset & 3) {
    case 0: {
        uint8_t r = (uint8_t)(v->status & 0x3F);
        if (vdc_has_pending_access(v))
            r |= ST_BUSY;
        assert_irq1(v, 0);
        v->status = 0;
        update_busy(v);
        return r;
    }
    case 2:
        if (v->pending_read)
            wait_for_vram_access(v);
        return (uint8_t)(v->read_buffer & 0xFF);
    case 3: {
        if (v->pending_read)
            wait_for_vram_access(v);
        uint8_t r = (uint8_t)(v->read_buffer >> 8);
        if (v->ar == R_VWR)
            queue_read(v);
        return r;
    }
    default:
        return 0;
    }
}

void vdc_write(Vdc *v, uint16_t offset, uint8_t value)
{
    switch (offset & 3) {
    case 0:
        v->ar = value & 0x1F;
        break;
    case 2:
    case 3: {
        int msb = offset & 1;
        uint16_t r = v->ar;
        if (r > 0x13)
            return;
        if (r == R_MAWR || r == R_MARR || r == R_VWR)
            wait_for_vram_access(v);

        if (msb)
            v->reg[r] = (uint16_t)((v->reg[r] & 0x00FF) | (value << 8));
        else
            v->reg[r] = (uint16_t)((v->reg[r] & 0xFF00) | value);
        v->reg[r] &= k_reg_mask[r];

        switch (r) {
        case R_MARR:
            if (msb)
                queue_read(v);
            break;
        case R_VWR:
            if (msb)
                queue_write(v);
            break;
        case R_BXR:
            if (v->latch_clock_x < 0)
                v->bxr_written_before_latch = 1;
            if (check_latch_timing(v, v->latch_clock_x))
                v->latched_bxr = v->reg[R_BXR];
            break;
        case R_BYR:
            v->bg_scroll_y_update_pending = 1;
            if (check_latch_timing(v, v->latch_clock_y)) {
                latch_scroll_y(v);
                v->byr_lsb_write_clock = -1;
            } else if (!msb && v->latch_clock_y >= 0) {
                int32_t elapsed = hclock(v) - v->latch_clock_y;
                if (elapsed < 0)
                    elapsed += VCE_LINE_LENGTH;
                v->byr_lsb_write_clock = (elapsed <= (divider(v) << 2)) ? hclock(v) : -1;
            } else if (msb && v->byr_lsb_write_clock >= 0) {
                if (!v->bxr_written_before_latch && v->h_state == H_HSW) {
                    int32_t elapsed = hclock(v) - v->byr_lsb_write_clock;
                    if (elapsed < 0)
                        elapsed += VCE_LINE_LENGTH;
                    if (elapsed <= divider(v) * 6)
                        refresh_scroll_y_current_line(v);
                }
                v->byr_lsb_write_clock = -1;
            }
            break;
        case R_LENR:
            if (msb) {
                v->vram_transfer_pending = 4u * ((uint32_t)v->reg[R_LENR] + 1);
                v->vram_transfer_src = v->reg[R_SOUR];
                v->vram_transfer_dest = v->reg[R_DESR];
            }
            break;
        case R_DVSSR:
            if (msb)
                v->trigger_sat_transfer = 1;
            break;
        default:
            break;
        }
        break;
    }
    default:
        break;
    }
}

/* ---- DMA ---- */

static void sat_transfer(Vdc *v)
{
    v->sat_transfer_pending--;
    if ((v->sat_transfer_pending & 3) == 0) {
        int i = 255 - (v->sat_transfer_pending >> 2);
        v->sat[i] = read_vram(v, (uint16_t)(v->reg[R_DVSSR] + i));
        log_write(v, VDC_VRAM_SIZE + (uint32_t)i, v->sat[i]);
        if (v->sat_transfer_pending == 0 && (v->reg[R_DCR] & 0x01)) {
            v->status |= ST_SAT_END;
            assert_irq1(v, 1);
        }
    }
}

static void vram_transfer(Vdc *v)
{
    v->vram_transfer_pending--;
    if ((v->vram_transfer_pending & 3) == 0) {
        if (v->vram_transfer_dest < VDC_VRAM_SIZE) {
            v->vram[v->vram_transfer_dest] = read_vram(v, v->vram_transfer_src);
            log_write(v, v->vram_transfer_dest, v->vram[v->vram_transfer_dest]);
            if (v->write_hook)
                v->write_hook(v->write_hook_ctx, v->vram_transfer_dest, v->vram[v->vram_transfer_dest], 1);
        }
        v->vram_transfer_src = (uint16_t)(v->vram_transfer_src + ((v->reg[R_DCR] & 0x04) ? -1 : 1));
        v->vram_transfer_dest = (uint16_t)(v->vram_transfer_dest + ((v->reg[R_DCR] & 0x08) ? -1 : 1));
        v->reg[R_SOUR] = v->vram_transfer_src;
        v->reg[R_DESR] = v->vram_transfer_dest;
        v->reg[R_LENR] = (uint16_t)((v->vram_transfer_pending >> 2) - 1);
        if (v->vram_transfer_pending == 0 && (v->reg[R_DCR] & 0x02)) {
            v->status |= ST_VRAM_END;
            assert_irq1(v, 1);
        }
    }
}

/* ---- Sprites ---- */

static void fetch_sprites(Vdc *v)
{
    v->sprite_count = 0;
    int mode1 = ((v->latched_mwr >> 2) & 3) == 1;

    for (int i = 0; i < 64; i++) {
        const uint16_t *s = &v->sat[i * 4];
        int sprite_y = (s[0] & 0x3FF) - 64;
        uint16_t flags = s[3];
        int cgy = (flags >> 12) & 3;
        int height = k_sprite_height[cgy];
        if (!(sprite_y <= v->raster_line && sprite_y + height > v->raster_line))
            continue;
        int y = v->raster_line - sprite_y;

        if (v->sprite_count >= 16) {
            v->sprite_overflow = 1;
            break;
        }

        int mode1_offset = mode1 ? (s[2] & 1) << 5 : 0;
        int cgx = (flags >> 8) & 1;
        int width = k_sprite_width[cgx];
        uint16_t sprite_x = s[1] & 0x3FF;
        uint16_t pattern = (uint16_t)((s[2] >> 1) & 0x3FF);
        pattern &= (uint16_t)k_sprite_mask_width[cgx];
        pattern &= (uint16_t)k_sprite_mask_height[cgy];
        uint16_t address = (uint16_t)(pattern << 6);
        uint8_t palette = (uint8_t)((flags & 0x0F) << 4);
        int x_flip = (flags & 0x0800) != 0;
        if (flags & 0x8000)
            y = height - 1 - y;
        uint16_t line_start = (uint16_t)(address + (y >> 4) * 128 + (y & 0xF) + mode1_offset);

        int halves = width == 16 ? 1 : 2;
        for (int half = 0; half < halves; half++) {
            if (half == 1) {
                v->sprite_count++;
                if (v->sprite_count >= 16) {
                    v->sprite_overflow = 1;
                    return;
                }
            }
            uint16_t line = line_start;
            if (halves == 2)
                line = (uint16_t)(line_start + ((half == 0) == x_flip ? 64 : 0));
            VdcSprite *sp = &v->sprites[v->sprite_count];
            sp->index = i;
            sp->x = (uint16_t)(sprite_x + half * 16);
            sp->flags = flags;
            sp->palette = palette;
            sp->data[0] = read_vram(v, (uint16_t)(line + 0));
            sp->data[1] = read_vram(v, (uint16_t)(line + 16));
            sp->data[2] = mode1 ? 0 : read_vram(v, (uint16_t)(line + 32));
            sp->data[3] = mode1 ? 0 : read_vram(v, (uint16_t)(line + 48));
        }
        v->sprite_count++;
    }
}

static void render_sprites(Vdc *v, int width)
{
    const uint16_t rendered_flag = 0x0200;
    if (v->sprite_count == 0)
        return;

    int min_x = width, max_x = -1;
    for (int i = 0; i < v->sprite_count; i++) {
        int left = v->sprites[i].x - 0x20;
        int right = left + 15;
        if (right < 0 || left >= width)
            continue;
        if ((left < 0 ? 0 : left) < min_x)
            min_x = left < 0 ? 0 : left;
        if ((right > width - 1 ? width - 1 : right) > max_x)
            max_x = right > width - 1 ? width - 1 : right;
    }
    if (max_x < min_x)
        return;
    for (int x = min_x; x <= max_x; x++)
        v->line_buffer_sprites[x] = 0;

    /* From last to first: the lower-numbered sprite ends up on top. */
    for (int i = v->sprite_count - 1; i >= 0; i--) {
        const VdcSprite *sp = &v->sprites[i];
        int pos = sp->x - 0x20;
        if (pos + 15 < 0 || pos >= width)
            continue;
        int priority = (sp->flags & 0x0080) != 0;
        uint16_t opaque = sp->data[0] | sp->data[1] | sp->data[2] | sp->data[3];
        if (opaque == 0)
            continue;
        int start_x = pos < 0 ? -pos : 0;
        int end_x = pos + 15 >= width ? width - pos - 1 : 15;
        for (int x = start_x; x <= end_x; x++) {
            int px = (sp->flags & 0x0800) ? (x & 0xF) : 15 - (x & 0xF);
            if (!((opaque >> px) & 1))
                continue;
            uint16_t pixel = (uint16_t)(((sp->data[0] >> px) & 1) | (((sp->data[1] >> px) & 1) << 1) |
                                        (((sp->data[2] >> px) & 1) << 2) | (((sp->data[3] >> px) & 1) << 3));
            int sx = pos + x;
            if (sp->index == 0 && (v->line_buffer_sprites[sx] & 0x0F))
                collision_irq(v);
            if (!priority && (v->line_buffer[sx] & 0x0F))
                pixel |= 0x100; /* behind the background: the background stays visible */
            else
                pixel |= (uint16_t)(sp->palette | 0x100 | rendered_flag);
            v->line_buffer_sprites[sx] = pixel;
        }
    }
    for (int x = min_x; x <= max_x; x++)
        if (v->line_buffer_sprites[x] & rendered_flag)
            v->line_buffer[x] = v->line_buffer_sprites[x] & (uint16_t)~rendered_flag;
}

/* ---- Background (tiles) ---- */

static void render_background(Vdc *v, int width)
{
    int screen_reg = (v->latched_mwr >> 4) & 7;
    int tiles_x = k_screen_tiles_x[screen_reg];
    int mask_x = tiles_x * 8 - 1;
    int bg_y = v->bg_offset_y & (k_screen_tiles_y[screen_reg] * 8 - 1);
    int tile_y = bg_y & 7;
    int bat_offset = (bg_y >> 3) * tiles_x;
    int bg_x = v->latched_bxr & mask_x;
    int i = 0;

    while (i < width) {
        uint16_t bat = v->vram[bat_offset + (bg_x >> 3)];
        int tile = (bat & 0x07FF) << 4;
        uint16_t word_a = v->vram[tile + tile_y];
        uint16_t word_b = v->vram[tile + tile_y + 8];
        v->vram_openbus = word_b;
        uint16_t palette = (uint16_t)((bat >> 12) << 4);
        uint16_t pixels[8];
        for (int x = 0; x < 8; x++) {
            int b = 7 - x;
            pixels[x] = (uint16_t)(palette | ((word_a >> b) & 1) | (((word_a >> (8 + b)) & 1) << 1) |
                                   (((word_b >> b) & 1) << 2) | (((word_b >> (8 + b)) & 1) << 3));
        }
        int offset = bg_x & 7;
        int count = 8 - offset;
        if (count > width - i)
            count = width - i;
        memcpy(v->line_buffer + i, pixels + offset, (size_t)count * sizeof(uint16_t));
        i += count;
        bg_x = (bg_x + count) & mask_x;
    }
}

/* Records the scanline for smooth scrolling (Vdc.line_log). The 1st dot of the visible area comes out in this same
 * vdc_clock and the VCE puts it in column pixel_x + 1. */
static void log_line(Vdc *v, int width)
{
    const Vce *c = v->vce;
    int w = c->screen_end_x - c->screen_start_x;
    if (!c->active_line || w <= 0 || v->burst_mode)
        return;
    int x = c->pixel_x + 1 - c->screen_start_x;
    int row = (c->pixel_index - (x > 0 ? x : 0)) / w;
    if (row < 0 || row >= VCE_ACTIVE_LINES || x >= w)
        return;
    VdcLineLog *log = open_log(v);
    if (!log)
        return;
    if (log->last_row < 0 || row <= log->last_row) {
        /* 1st scanline of the frame: VRAM and sprites as they are now; from here on, the writes */
        memset(log->row, 0, sizeof(log->row));
        memcpy(log->vram, v->vram, sizeof(log->vram));
        memcpy(log->sat, v->sat, sizeof(log->sat));
        log->write_count = 0;
        log->overflow = 0;
    }
    log->last_row = row;
    VdcLineInfo *l = &log->row[row];
    l->valid = 1;
    l->cr = (uint8_t)(v->latched_cr & 0xC0);
    l->x0 = (int16_t)x;
    l->width = (uint16_t)width;
    l->bxr = v->latched_bxr;
    l->bgy = (uint16_t)v->bg_offset_y;
    l->mwr = v->latched_mwr;
    l->raster = (int16_t)v->raster_line;
}

static void render_line(Vdc *v)
{
    int width = (v->latched_hdw + 1) << 3;
    if (width > VDC_MAX_WIDTH)
        width = VDC_MAX_WIDTH;
    if (v->line_log)
        log_line(v, width);
    if (!(v->latched_cr & 0x80))
        for (int i = 0; i < width; i++)
            v->line_buffer[i] = 0x100;
    if (!v->burst_mode) {
        if (v->latched_cr & 0x80)
            render_background(v, width);
        if (v->latched_cr & 0x40)
            render_sprites(v, width);
    }
}

/* ---- Scanline and frame state machine ---- */

static void increment_raster_line(Vdc *v)
{
    v->raster_line++;
    v->need_to_increment_raster_line = 0;
    v->increment_bg_counter_y = 1;
    v->lines_to_next_v_state--;
    while (v->lines_to_next_v_state <= 0)
        next_v_state(v);
    rcr_irq(v);
}

static void end_of_line(Vdc *v)
{
    v->hpos = 0;
    v->vpos++;
    v->latch_clock_y = -1;
    v->latch_clock_x = -1;
    v->bxr_written_before_latch = 0;
    v->byr_lsb_write_clock = -1;

    if (v->need_to_increment_raster_line)
        increment_raster_line(v);

    if (v->vpos == vce_total_lines(v->vce)) {
        v->vpos = 0;
        v->latched_cr = (uint16_t)((v->latched_cr & ~0x00C0) | (CRV(v) & 0x00C0));
        v->burst_mode = (CRV(v) & 0x00C0) == 0;
    }
    v->active_line = v->v_state == V_VDW && v->vpos >= ACTIVE_DISPLAY_START && v->vpos < BOTTOM_BLANKING_START;
}

static void hsync_start(Vdc *v)
{
    v->hsync_start_clock = hclock(v);
    v->load_bg_start_clock = VCE_LINE_LENGTH;
    v->load_bg_end_clock = 0;

    v->latched_hds = HDS(v);
    v->latched_hdw = HDW(v);
    v->latched_hde = HDE(v);
    v->latched_hsw = HSW(v);

    v->next_event = EV_NONE;
    v->clocks_to_next_event = -1;

    int32_t display_start = v->hpos + v->clocks_to_next_h_state + ((v->latched_hds + 1) << 3);
    if (dots_to_clocks(v, display_start - 24) >= VCE_LINE_LENGTH)
        return;

    if (v->v_state == V_VDW) {
        int32_t display_width = (v->latched_hdw + 1) << 3;
        v->load_bg_start_clock = dots_to_clocks(v, display_start - 16);
        v->load_bg_end_clock = v->load_bg_start_clock + dots_to_clocks(v, display_width + 16);
        if (v->load_bg_end_clock > VCE_LINE_LENGTH)
            v->load_bg_end_clock = VCE_LINE_LENGTH;
    }

    int32_t event_clocks;
    if (v->v_state == V_VDW) {
        v->next_event = EV_BYR;
        event_clocks = BYR_LATCH_CLOCKS;
    } else {
        v->next_event = EV_HDS;
        event_clocks = 26;
    }

    if (display_start - event_clocks <= v->hpos) {
        v->clocks_to_next_event = 1;
        line_events(v);
    } else {
        v->clocks_to_next_event = display_start - event_clocks - v->hpos;
    }
}

static void line_events(Vdc *v)
{
    v->clocks_to_next_event--;
    if (v->clocks_to_next_event != 0)
        return;

    switch (v->next_event) {
    case EV_BYR:
        v->next_event = EV_BXR;
        v->clocks_to_next_event = 2;
        v->latch_clock_y = hclock(v);
        latch_scroll_y(v);
        break;
    case EV_BXR:
        v->next_event = EV_HDS;
        v->clocks_to_next_event = 6;
        v->latched_bxr = v->reg[R_BXR];
        v->latch_clock_x = hclock(v);
        break;
    case EV_HDS:
        v->next_event = EV_NONE;
        v->clocks_to_next_event = -1;
        if (v->v_state != V_VDW && !v->vblank_triggered) {
            v->vblank_triggered = 1;
            vblank_irq(v);
        }
        if (v->sprite_overflow) {
            v->sprite_overflow = 0;
            overflow_irq(v);
        }
        break;
    case EV_RCR:
        v->next_event = EV_NONE;
        v->clocks_to_next_event = -1;
        increment_raster_line(v);
        if (v->v_state == V_VDW && !v->burst_mode)
            fetch_sprites(v);
        break;
    default:
        break;
    }
}

static void next_v_state(Vdc *v)
{
    v->v_state = (v->v_state + 1) % 4;
    switch (v->v_state) {
    case V_VDS:
        v->lines_to_next_v_state = v->latched_vds + 2;
        break;
    case V_VDW:
        v->lines_to_next_v_state = v->latched_vdw + 1;
        v->raster_line = 0;
        v->vblank_triggered = 0;
        break;
    case V_VCR:
        v->lines_to_next_v_state = v->latched_vcr;
        break;
    case V_VSW:
        v->lines_to_next_v_state = v->latched_vsw + 1;
        v->latched_mwr = v->reg[R_MWR];
        v->latched_vds = VDS(v);
        v->latched_vdw = VDW(v);
        v->latched_vcr = VCRV(v);
        v->latched_vsw = VSW(v);
        break;
    }
}

static void next_h_state(Vdc *v)
{
    v->h_state = (v->h_state + 1) % 4;
    switch (v->h_state) {
    case H_HDS:
        v->clocks_to_next_h_state = (v->latched_hds + 1) << 3;
        v->line_buffer_index = 0;
        if (!v->burst_mode)
            v->latched_cr = CRV(v);
        break;
    case H_HDW:
        v->clocks_to_next_h_state = (v->latched_hdw + 1) << 3;
        v->next_event = EV_RCR;
        v->clocks_to_next_event = ((int32_t)v->latched_hdw - 1) * 8 + 2;
        v->need_to_increment_raster_line = 1;
        if (v->clocks_to_next_event <= 0) {
            v->clocks_to_next_event = 1;
            line_events(v);
        }
        if (v->active_line)
            render_line(v);
        break;
    case H_HDE:
        v->clocks_to_next_h_state = (v->latched_hde + 1) << 3;
        break;
    case H_HSW:
        v->clocks_to_next_h_state = (v->latched_hsw + 1) << 3;
        hsync_start(v);
        break;
    }
}

uint16_t vdc_clock(Vdc *v)
{
    if (v->sat_transfer_pending > 0)
        sat_transfer(v);
    else if (v->vram_transfer_pending > 0)
        vram_transfer(v);

    v->hpos++;
    v->clocks_to_next_h_state--;
    while (v->clocks_to_next_h_state == 0)
        next_h_state(v);

    if (v->clocks_to_next_event > 0)
        line_events(v);

    uint16_t pixel = v->burst_mode ? VCE_PIXEL_BLACK : 0x100;
    if (v->active_line && v->h_state == H_HDW) {
        if (v->burst_mode) {
            pixel = VCE_PIXEL_BLACK;
        } else if (v->line_buffer_index < VDC_MAX_WIDTH) {
            pixel = v->line_buffer[v->line_buffer_index];
            if ((pixel & 0x0F) == 0)
                pixel = 0;
        }
        v->line_buffer_index++;
    }
    return pixel;
}

int vdc_quiet_pixels(const Vdc *v)
{
    if (v->sat_transfer_pending > 0 || v->vram_transfer_pending > 0)
        return 0;
    /* The dot that zeroes one of these counters changes the phase or fires the event: it is left to vdc_clock. */
    int32_t limit = v->clocks_to_next_h_state - 1;
    if (v->clocks_to_next_event > 0 && v->clocks_to_next_event - 1 < limit)
        limit = v->clocks_to_next_event - 1;
    return limit > 0 ? limit : 0;
}

int vdc_clock_span(Vdc *v, int n, uint16_t *out)
{
    int limit = vdc_quiet_pixels(v);
    if (n > limit)
        n = limit;
    if (n <= 0)
        return 0;
    v->hpos += n;
    v->clocks_to_next_h_state -= n;
    if (v->clocks_to_next_event > 0)
        v->clocks_to_next_event -= n;
    if (v->active_line && v->h_state == H_HDW && !v->burst_mode) {
        int index = v->line_buffer_index;
        for (int i = 0; i < n; i++, index++) {
            uint16_t pixel = 0x100;
            if (index < VDC_MAX_WIDTH) {
                pixel = v->line_buffer[index];
                if ((pixel & 0x0F) == 0)
                    pixel = 0;
            }
            out[i] = pixel;
        }
    } else {
        uint16_t pixel = v->burst_mode ? VCE_PIXEL_BLACK : 0x100;
        for (int i = 0; i < n; i++)
            out[i] = pixel;
    }
    if (v->active_line && v->h_state == H_HDW)
        v->line_buffer_index += n;
    return n;
}

void vdc_hsync(Vdc *v)
{
    end_of_line(v);
    v->h_state = H_HSW;
    v->clocks_to_next_h_state = divider(v) == 3 ? 32 : 24;
    hsync_start(v);
}

void vdc_vsync(Vdc *v)
{
    v->latched_mwr = v->reg[R_MWR];
    v->latched_vds = VDS(v);
    v->latched_vdw = VDW(v);
    v->latched_vcr = VCRV(v);
    v->latched_vsw = VSW(v);
    v->v_state = V_VSW;
    v->lines_to_next_v_state = v->latched_vsw + 1;
    v->increment_bg_counter_y = 0;
}

void vdc_reset(Vdc *v, Vce *vce, HuC6280 *cpu)
{
    memset(v, 0, sizeof(*v));
    v->vce = vce;
    v->cpu = cpu;
    v->reg[R_HDR] = 0x1F;
    v->reg[R_VDR] = 239;
    v->read_buffer = 0xFFFF;
    v->load_bg_start_clock = VCE_LINE_LENGTH;
    v->byr_lsb_write_clock = -1;
    v->latch_clock_y = -1;
    v->latch_clock_x = -1;
    v->latched_hds = HDS(v);
    v->latched_hdw = HDW(v);
    v->latched_hde = HDE(v);
    v->latched_hsw = HSW(v);
    v->latched_vds = VDS(v);
    v->latched_vdw = VDW(v);
    v->latched_vcr = VCRV(v);
    v->latched_vsw = VSW(v);
    v->latched_cr = CRV(v);
    v->v_state = V_VDS;
    v->h_state = H_HDS;
    v->next_event = EV_NONE;
    v->lines_to_next_v_state = v->latched_vds + 2;
    v->clocks_to_next_h_state = 1;
    v->clocks_to_next_event = -1;
}
