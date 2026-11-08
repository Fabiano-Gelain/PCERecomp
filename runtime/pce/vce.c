/*
 * VCE (HuC6260). Ver vce.h.
 */
#include "pce/vce.h"

#include <string.h>

#include "pce/vdc.h"

/* Dots per full scanline, for each speed. */
static const int k_full_line_width[4] = { 342, 455, 683, 683 };
/* Visible dot range (without overscan), for each speed. */
static const int k_line_start[4] = { 48, 72, 120, 120 };
static const int k_line_end[4] = { 304, 413, 632, 632 };

static void set_speed(Vce *v, int speed)
{
    v->speed = speed;
    v->clock_divider = speed == 0 ? 4 : (speed == 1 ? 3 : 2);
    v->screen_start_x = k_line_start[speed];
    v->screen_end_x = k_line_end[speed];
}

void vce_reset(Vce *v)
{
    v->control = 0;
    v->color_address = 0;
    set_speed(v, 0);
    v->blur = 0;
    v->hpos = 0;
    v->vpos = 0;
    v->pixel_x = 0;
    v->pixel_index = 0;
    v->active_line = 0;
    v->multiple_speeds = 0;
    /* The hardware powers on with random colors; we use 0, like gg_trace. */
    memset(v->color, 0, sizeof(v->color));
    v->frame_width = k_line_end[0] - k_line_start[0];
    v->frame_height = VCE_ACTIVE_LINES;
    v->frame_count = 0;
    memset(v->frame, 0, sizeof(v->frame));
    memset(v->frame_index, 0, sizeof(v->frame_index));
}

int vce_total_lines(const Vce *v)
{
    return v->blur ? 263 : 262;
}

int vce_speed_index(const Vce *v)
{
    return v->speed > 2 ? 2 : v->speed;
}

uint8_t vce_read(Vce *v, uint16_t offset)
{
    switch (offset & 7) {
    case 4:
        return (uint8_t)(v->color[v->color_address] & 0xFF);
    case 5: {
        uint8_t r = (uint8_t)(0xFE | ((v->color[v->color_address] >> 8) & 1));
        v->color_address = (v->color_address + 1) & 0x1FF;
        return r;
    }
    default:
        return 0xFF;
    }
}

void vce_write(Vce *v, uint16_t offset, uint8_t value)
{
    switch (offset & 7) {
    case 0: {
        v->control = value;
        v->blur = (value >> 2) & 1;
        int speed = value & 3;
        if (speed != v->speed)
            v->multiple_speeds = 1;
        set_speed(v, speed);
        break;
    }
    case 2:
        v->color_address = (uint16_t)((v->color_address & 0x100) | value);
        break;
    case 3:
        v->color_address = (uint16_t)((v->color_address & 0x0FF) | ((value & 1) << 8));
        break;
    case 4:
        v->color[v->color_address] = (uint16_t)((v->color[v->color_address] & 0x100) | value);
        break;
    case 5:
        v->color[v->color_address] = (uint16_t)((v->color[v->color_address] & 0x0FF) | ((value & 1) << 8));
        v->color_address = (v->color_address + 1) & 0x1FF;
        break;
    default:
        break;
    }
}

uint32_t vce_rgb(uint16_t c)
{
    uint32_t g = ((c >> 6) & 7) * 255 / 7;
    uint32_t r = ((c >> 3) & 7) * 255 / 7;
    uint32_t b = (c & 7) * 255 / 7;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void render_frame(Vce *v)
{
    static uint32_t rgb[512]; /* vce_rgb of each color, computed once */
    if (!rgb[0])
        for (int c = 0; c < 512; c++)
            rgb[c] = vce_rgb((uint16_t)c);
    for (int i = 0; i < v->pixel_index; i++) {
        uint16_t p = v->buffer[i];
        v->frame[i] = (p & VCE_PIXEL_BLACK) ? 0xFF000000u : rgb[p & 0x1FF];
    }
    memcpy(v->frame_index, v->index_buffer, (size_t)v->pixel_index * sizeof(uint16_t));
    v->frame_width = v->screen_end_x - v->screen_start_x;
    v->frame_height = v->frame_width ? v->pixel_index / v->frame_width : 0;
    v->frame_count++;
    v->multiple_speeds = 0;
}

uint32_t vce_quiet_cycles(const Vce *v, const struct Vdc *vdc)
{
    /* The next dot outside a span (vdc_clock_span) or the end of the scanline (HSYNC), whichever comes first */
    uint32_t div = (uint32_t)v->clock_divider;
    uint32_t to_special = div - (uint32_t)v->hpos % div + (uint32_t)vdc_quiet_pixels(vdc) * div;
    uint32_t to_line_end = (uint32_t)(VCE_LINE_LENGTH - v->hpos);
    return to_special < to_line_end ? to_special : to_line_end;
}

int vce_clock(Vce *v, struct Vdc *vdc, uint32_t cycles)
{
    int frame_ready = 0;
    /* Only the 1st step can start in the middle of a dot: after a dot or the end of the scanline, hpos is
     * a multiple of the divider (the speed only changes between calls, in vce_write). */
    uint32_t to_pixel = (uint32_t)(v->clock_divider - (v->hpos % v->clock_divider));

    const uint32_t div = (uint32_t)v->clock_divider;
    const int full_width = k_full_line_width[v->speed];

    while (cycles > 0) {
        uint32_t to_line_end = (uint32_t)(VCE_LINE_LENGTH - v->hpos);
        if (to_pixel <= cycles && to_pixel <= to_line_end) {
            /* All the dots that fit in the cycles and in the scanline (only the last can fall at the end of the
             * scanline), in spans where the VDC only advances (vdc_clock_span); hpos always stays at the dot in
             * progress, because vdc_clock reads it on scanline events. */
            uint32_t room = cycles < to_line_end ? cycles : to_line_end;
            int left = 1 + (int)((room - to_pixel) / div);
            v->hpos += (int)to_pixel;
            cycles -= to_pixel;
            for (;;) {
                uint16_t out[512];
                int n = vdc_clock_span(vdc, left < 512 ? left : 512, out);
                if (n == 0) {
                    out[0] = vdc_clock(vdc);
                    n = 1;
                }
                if (v->pixel_x + n < full_width) {
                    /* No wrap to 0 within the span: dots pixel_x+1 .. pixel_x+n; only the part in the visible range */
                    int first = v->pixel_x + 1;
                    int from = v->screen_start_x > first ? v->screen_start_x - first : 0;
                    int to = v->screen_end_x - first < n ? v->screen_end_x - first : n;
                    if (v->active_line && to > from) {
                        int room_left = VCE_MAX_WIDTH * VCE_ACTIVE_LINES - v->pixel_index;
                        if (to - from > room_left)
                            to = from + (room_left > 0 ? room_left : 0);
                        uint16_t *dst = &v->buffer[v->pixel_index], *idx = &v->index_buffer[v->pixel_index];
                        for (int i = from; i < to; i++) {
                            uint16_t pixel = out[i];
                            *idx++ = (pixel & VCE_PIXEL_BLACK) ? VCE_PIXEL_BLACK : (uint16_t)(pixel & 0x1FF);
                            *dst++ = (pixel & VCE_PIXEL_BLACK) ? VCE_PIXEL_BLACK : v->color[pixel & 0x1FF];
                        }
                        v->pixel_index += to - from;
                    }
                    v->pixel_x += n;
                } else {
                    for (int i = 0; i < n; i++) {
                        v->pixel_x++;
                        if (v->pixel_x == full_width)
                            v->pixel_x = 0;
                        if (v->active_line && v->pixel_x >= v->screen_start_x && v->pixel_x < v->screen_end_x &&
                            v->pixel_index < VCE_MAX_WIDTH * VCE_ACTIVE_LINES) {
                            uint16_t pixel = out[i];
                            v->index_buffer[v->pixel_index] =
                                (pixel & VCE_PIXEL_BLACK) ? VCE_PIXEL_BLACK : (uint16_t)(pixel & 0x1FF);
                            v->buffer[v->pixel_index++] = (pixel & VCE_PIXEL_BLACK) ? VCE_PIXEL_BLACK : v->color[pixel & 0x1FF];
                        }
                    }
                }
                left -= n;
                uint32_t advance = (uint32_t)(n - 1) * div;
                v->hpos += (int)advance;
                cycles -= advance;
                if (left == 0)
                    break;
                v->hpos += (int)div;
                cycles -= div;
            }
        } else {
            uint32_t step = cycles < to_line_end ? cycles : to_line_end;
            v->hpos += (int)step;
            cycles -= step;
        }

        to_pixel = div;
        if (v->hpos >= VCE_LINE_LENGTH) {
            v->hpos = 0;
            v->pixel_x = 0;
        }

        if (v->hpos == 0) {
            /* HSYNC: start of a new scanline */
            vdc_hsync(vdc);

            int total = vce_total_lines(v);
            if (v->vpos == total - 4)
                vdc_vsync(vdc);
            else if (v->vpos == total - 1)
                v->pixel_index = 0;

            v->vpos++;
            if (v->vpos == total)
                v->vpos = 0;

            if (v->vpos == VCE_ACTIVE_FIRST_LINE + VCE_ACTIVE_LINES) {
                render_frame(v);
                v->pixel_index = 0;
                frame_ready = 1;
                vdc_log_close(vdc);
            }

            v->active_line = v->vpos >= VCE_ACTIVE_FIRST_LINE && v->vpos < VCE_ACTIVE_FIRST_LINE + VCE_ACTIVE_LINES;
        }
    }
    return frame_ready;
}
