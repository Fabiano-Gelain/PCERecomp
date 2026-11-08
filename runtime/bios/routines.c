/*
 * Table of the groups of BIOS routines rewritten in C. See routines.h.
 */
#include "bios/routines.h"

#include <stddef.h>

const HleRoutine *const hle_groups[] = {
    hle_ex_routines,
    hle_math_routines,
    hle_irq_routines,
    hle_cd_routines,
    hle_adpcm_routines,
    hle_bram_routines,
    hle_font_routines,
    hle_psg_routines,
    NULL,
};

const char *hle_routine_name(uint16_t entry)
{
    for (int g = 0; hle_groups[g]; g++)
        for (const HleRoutine *r = hle_groups[g]; r->entry; r++)
            if (r->entry == entry)
                return r->name;
    return NULL;
}
