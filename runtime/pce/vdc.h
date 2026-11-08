/*
 * VDC (HuC6270): video controller. 64 KB of VRAM (32K 16-bit words),
 * 20 registers, a background made of 8x8 tiles and up to 64 sprites.
 *
 * It works dot by dot, driven by the VCE (vdc_clock at each dot, vdc_hsync at the
 * start of each scanline, vdc_vsync at the end of the frame). Each scanline goes through the states
 * HDS (left margin), HDW (visible area), HDE (right margin) and HSW (sync);
 * each frame through VDS, VDW (visible scanlines), VCR and VSW.
 *
 * The CPU does not access VRAM immediately: the read/write stays pending until a free
 * "window" comes up (the VDC also uses VRAM to draw). If the CPU attempts another
 * access before that, it waits (extra cycles), as on the console.
 *
 * Behavior reference: reference/Geargrafx/src/huc6270*.
 */
#ifndef PCE_VDC_H
#define PCE_VDC_H

#include <stdint.h>

#include "cpu/huc6280.h"
#include "pce/vce.h"

#define VDC_VRAM_SIZE 0x8000
#define VDC_SAT_SIZE 0x100
#define VDC_MAX_WIDTH 1024

typedef struct VdcSprite {
    int index;
    uint16_t x;
    uint16_t flags;
    uint8_t palette;
    uint16_t data[4];
} VdcSprite;

/* What the VDC used to draw each scanline of the VCE image (only recorded when Vdc.line_log is not NULL:
 * the PC version of a game can redraw the image from this, e.g. smooth scrolling; the console is unchanged) */
typedef struct VdcLineInfo {
    uint8_t valid;   /* the scanline came from the VDC (otherwise: border or scanline outside the drawing area) */
    uint8_t cr;      /* bits 0x80 (background) and 0x40 (sprites) of the CR */
    int16_t x0;      /* column of the VCE image where the 1st dot of the VDC's visible area falls */
    uint16_t width;  /* dots of the visible area */
    uint16_t bxr;    /* horizontal scroll of the scanline */
    uint16_t bgy;    /* background map row (before the map-size mask) */
    uint16_t mwr;    /* map size and sprite mode */
    int16_t raster;  /* VDC scanline (the sprites' one) */
} VdcLineInfo;

/* Write to VRAM (addr < VDC_VRAM_SIZE) or to the sprite attribute table (VDC_VRAM_SIZE + i) during drawing:
 * takes effect from image row row onward */
typedef struct VdcLogWrite {
    uint16_t addr, value;
    int16_t row;
} VdcLogWrite;

#define VDC_LOG_WRITES 16384

/* One frame: VRAM and sprites as they were at the 1st drawn scanline, and what changed afterwards, scanline by scanline */
typedef struct VdcLineLog {
    VdcLineInfo row[VCE_ACTIVE_LINES];
    int last_row;      /* last scanline recorded; -1 = none yet */
    uint16_t vram[VDC_VRAM_SIZE];
    uint16_t sat[VDC_SAT_SIZE];
    VdcLogWrite writes[VDC_LOG_WRITES];
    int write_count;
    int overflow;      /* too many writes: the frame cannot be redrawn */
    /* The frame has ended (vdc_log_close) and has not been read yet (vdc_log_open). A C BIOS routine can
     * spend tens of thousands of cycles at once, and the VDC may already draw scanlines of the next frame before
     * whoever reads the log sees the end of the frame: they go to next (created the 1st time), they do not erase this one */
    int closed;
    struct VdcLineLog *next;
} VdcLineLog;

typedef struct Vdc {
    uint16_t vram[VDC_VRAM_SIZE];
    uint16_t sat[VDC_SAT_SIZE];
    uint16_t reg[20];
    uint16_t ar;     /* selected register */
    uint16_t status;

    uint16_t read_buffer;
    uint16_t vram_openbus;
    int pending_read, pending_write;
    int32_t transfer_delay;
    uint32_t cpu_vram_cycles;

    int32_t load_bg_start_clock, load_bg_end_clock, hsync_start_clock;
    int trigger_sat_transfer;
    uint16_t sat_transfer_pending;
    /* For observers only (the game project): sprite attribute table copies started and the address (R13) of the last one. A
     * long instruction (TIA) can contain a whole copy, without sat_transfer_pending being seen between instructions */
    uint32_t sat_copies;
    uint16_t sat_copy_base;
    uint32_t vram_transfer_pending;
    uint16_t vram_transfer_src, vram_transfer_dest;

    int32_t hpos, vpos;
    int32_t bg_offset_y, bg_counter_y;
    int bg_scroll_y_update_pending;
    int bxr_written_before_latch;
    int32_t byr_lsb_write_clock;
    int increment_bg_counter_y;
    int need_to_increment_raster_line;
    int32_t latch_clock_y, latch_clock_x;
    int32_t raster_line;

    uint16_t latched_bxr, latched_hds, latched_hdw, latched_hde, latched_hsw;
    uint16_t latched_vds, latched_vdw, latched_vcr, latched_vsw, latched_mwr, latched_cr;

    int v_state, h_state, next_event;
    int32_t lines_to_next_v_state, clocks_to_next_h_state, clocks_to_next_event;
    int vblank_triggered;
    int active_line;
    int burst_mode;

    uint16_t line_buffer[VDC_MAX_WIDTH];
    uint16_t line_buffer_sprites[VDC_MAX_WIDTH];
    int line_buffer_index;
    int sprite_count;
    int sprite_overflow;
    VdcSprite sprites[128];

    Vce *vce;
    HuC6280 *cpu;
    VdcLineLog *line_log; /* NULL = does not record (traces, tests) */
    /* Research (pce_trace --bat-writers): called for each word written to VRAM (by the CPU or by the
     * VRAM-VRAM copy); NULL = nobody listens. Observes only: does not change the console */
    void (*write_hook)(void *ctx, uint16_t addr, uint16_t value, int dma);
    void *write_hook_ctx;
} Vdc;

void vdc_reset(Vdc *vdc, Vce *vce, HuC6280 *cpu);
uint8_t vdc_read(Vdc *vdc, uint16_t offset);
void vdc_write(Vdc *vdc, uint16_t offset, uint8_t value);

/* Called by the VCE */
uint16_t vdc_clock(Vdc *vdc); /* one dot; returns the color index (0-511) or VCE_PIXEL_BLACK */
/* Up to n consecutive dots in which the VDC only advances (no transfer, no scanline phase change, no event):
 * the same as n calls to vdc_clock, with the dots in out. Returns how many it did; 0 = the next dot
 * needs vdc_clock. */
int vdc_clock_span(Vdc *vdc, int n, uint16_t *out);
int vdc_quiet_pixels(const Vdc *vdc); /* how many dots vdc_clock_span would do now */
void vdc_hsync(Vdc *vdc);
void vdc_vsync(Vdc *vdc);

/* Pending CPU accesses to VRAM (the system clock advances in steps of 3 while there are any) */
int vdc_has_pending_access(const Vdc *vdc);
void vdc_process_cpu_access(Vdc *vdc, uint32_t cycles);

/* Scanline log (Vdc.line_log): end of the image (the VCE finished the frame) and start of reading the next one
 * (pce_run_frame), when what has already been recorded of the next frame moves into the log */
void vdc_log_close(Vdc *vdc);
void vdc_log_open(Vdc *vdc);

#endif
