/*
 * VCE (HuC6260): 512-color table and the video clock.
 *
 * A scanline lasts 1365 master clock cycles; a frame has 262 scanlines (263 with
 * the "blur" bit of the control register). At each dot (every 4, 3 or 2 cycles,
 * depending on the speed) the VCE asks the VDC for a pixel and converts it to a color.
 * At the start of each scanline it sends the HSYNC signal to the VDC; near the end of the frame, the VSYNC.
 *
 * Behavior reference: reference/Geargrafx/src/huc6260*.
 */
#ifndef PCE_VCE_H
#define PCE_VCE_H

#include <stdint.h>

#define VCE_LINE_LENGTH 1365
#define VCE_HSYNC_START_HPOS (VCE_LINE_LENGTH - 237)
#define VCE_PIXEL_BLACK 0x0800   /* "black" pixel (VDC in burst mode) */
#define VCE_ACTIVE_FIRST_LINE 14 /* first visible scanline */
#define VCE_ACTIVE_LINES 242
#define VCE_MAX_WIDTH 512

struct Vdc;

typedef struct Vce {
    uint8_t control;
    uint16_t color_address;
    uint16_t color[512];

    int speed;         /* 0 = 5.36 MHz, 1 = 7.16 MHz, 2/3 = 10.8 MHz */
    int clock_divider; /* 4, 3 or 2 master cycles per dot */
    int blur;          /* 1 = 263 scanlines */

    int hpos;          /* master cycle within the scanline (0..1364) */
    int vpos;          /* scanline (0..261/262) */
    int pixel_x;       /* dot within the scanline */
    int active_line;
    int screen_start_x, screen_end_x;
    int multiple_speeds; /* speed changed during the frame */

    /* Frame under construction: 9-bit color values (or VCE_PIXEL_BLACK) */
    uint16_t buffer[VCE_MAX_WIDTH * VCE_ACTIVE_LINES];
    int pixel_index;

    /* The color number (0-511, or VCE_PIXEL_BLACK) of each dot of the frame under construction and of the last
     * completed one (the smooth fade knows which color each dot is; changes nothing in the console) */
    uint16_t index_buffer[VCE_MAX_WIDTH * VCE_ACTIVE_LINES];
    uint16_t frame_index[VCE_MAX_WIDTH * VCE_ACTIVE_LINES];

    /* Last completed frame, in ARGB8888 */
    uint32_t frame[VCE_MAX_WIDTH * VCE_ACTIVE_LINES];
    int frame_width, frame_height;
    uint64_t frame_count;
} Vce;

void vce_reset(Vce *vce);
uint8_t vce_read(Vce *vce, uint16_t offset);
void vce_write(Vce *vce, uint16_t offset, uint8_t value);
int vce_total_lines(const Vce *vce);
/* Speed for tables that only have 3 entries (5.36 / 7.16 / 10.8 MHz). */
int vce_speed_index(const Vce *vce);

/* Advances N master cycles, taking the VDC along. Returns 1 if a frame became ready. */
int vce_clock(Vce *vce, struct Vdc *vdc, uint32_t cycles);
/* Master cycles from now during which the VCE/VDC only advance counters and ordinary dots: anything shorter
 * than this can be deferred to a single call, with the same result (nothing visible from outside happens before). */
uint32_t vce_quiet_cycles(const Vce *vce, const struct Vdc *vdc);

/* Converts a 9-bit color (GGGRRRBBB) to ARGB8888. */
uint32_t vce_rgb(uint16_t color);

#endif
