/*
 * Synthetic BIOS (contains no BIOS code): see synth.c.
 */
#ifndef PCE_BIOS_SYNTH_H
#define PCE_BIOS_SYNTH_H

#include <stdint.h>

#define BIOS_SYNTH_SIZE 0x40000 /* 32 banks, like the System Card */

/* Builds the image (BIOS_SYNTH_SIZE bytes). */
void bios_synth_build(uint8_t *rom);
/* Places the disc's license text where the disc's boot program checks for it. */
void bios_synth_copyright(uint8_t *rom, const uint8_t *text, int len);

#endif
