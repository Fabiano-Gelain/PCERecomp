/*
 * Parts of the C BIOS used by more than one routine (e.g. the VBlank interrupt reads the joypads and
 * loads palettes like ex_joysns and ex_colorcmd). See hle.h.
 */
#ifndef PCE_BIOS_INTERNAL_H
#define PCE_BIOS_INTERNAL_H

#include "pce/pce.h"

/* ex_joysns without the return (RTS). Returns 1 if a game reset was requested (PC already at ($2284)). */
int bios_joysns(Pce *pce);
/* ex_colorcmd without the return (RTS). */
void bios_colorcmd(Pce *pce);
/* Call to a BIOS part that waits (also in C, registered at address `target`) or to game code:
 * pushes the return to `ret` (like a JSR made by the instruction ending at ret - 1) and sets the PC
 * to `target`. The return (RTS) lands on a point registered at `ret`. */
void bios_call(Pce *pce, uint16_t target, uint16_t ret);


/* ---- Shared parts of the CD and ADPCM routines (hle_cd.c) ---- */
/* TST #imm,abs (flags like the CPU); returns imm & memory */
uint8_t bios_tst(Pce *pce, uint8_t imm, uint16_t addr);
/* TSB/TRB abs with A = a */
void bios_tsb(Pce *pce, uint16_t addr, uint8_t a);
void bios_trb(Pce *pce, uint16_t addr, uint8_t a);
/* Window (0-7) of the bit set in `bit` (the BIOS builds TMA/TAM #bit at $228E) */
int bios_window_of_bit(uint8_t bit);
/* Clears the CD command ($224D-$2255) */
void bios_clear_command(Pce *pce);
/* Saves / restores $20F8-$20FF in $2260-$2267 */
void bios_save_args(Pce *pce);
void bios_restore_args(Pce *pce);
/* Time (CPU cycles, including JSR and RTS) of F107 (clears the command) and of F322/F331 (save/restore) */
#define T_CLEAR_COMMAND 120
#define T_SAVE_ARGS 148
/* Adds the base chosen by $2273 to the requested sector ($20FC-$20FE) */
void bios_add_base(Pce *pce);
/* 3 bytes from $20F8+x into the command at $224C+y */
void bios_put3(Pce *pce, uint8_t x, uint8_t y);
/* VRAM write address = ($20FA); $2272 = 1 */
void bios_vram_write_address(Pce *pce);
/* Selects the VDC register (saved in $20F7) */
void bios_cd_vdc_select(Pce *pce, uint8_t reg);

/* $2256-$2258 (minute, second, frame in BCD) -> sector in $20F8 (high)-$20FA */
void bios_msf_to_lba(Pce *pce);

/* Addresses of the internal parts of the CD drive conversation (see hle_cd.c) */
#define SCSI_SEND 0xE917    /* sends the command at $224C; returns with A = phase */
#define SCSI_SEND_NOW 0xE91C
#define SCSI_STATUS 0xE9DC  /* reads status and message; A = status (0 = ok), 6 = drive did not respond */
#define CD_ERROR 0xE224     /* handles the error: C = 1 gives up, C = 0 retries */

#endif
