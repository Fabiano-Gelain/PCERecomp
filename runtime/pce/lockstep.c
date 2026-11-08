/*
 * Lockstep execution. See lockstep.h.
 */
#include "pce/lockstep.h"

#include "cd/cdrom.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void capture(LockstepCpu *s, const Pce *pce)
{
    const HuC6280 *c = &pce->cpu;
    s->pc = c->pc;
    s->a = c->a;
    s->x = c->x;
    s->y = c->y;
    s->s = c->s;
    s->p = c->p;
    s->speed = c->speed;
    memcpy(s->mpr, c->mpr, 8);
    s->irq_request = c->irq_request;
    s->cycles = c->cycles;
    s->master_cycles = pce->master_cycles;
}

static int same_cpu(const LockstepCpu *a, const LockstepCpu *b)
{
    return a->pc == b->pc && a->a == b->a && a->x == b->x && a->y == b->y && a->s == b->s && a->p == b->p &&
           a->speed == b->speed && memcmp(a->mpr, b->mpr, 8) == 0 && a->irq_request == b->irq_request &&
           a->cycles == b->cycles && a->master_cycles == b->master_cycles;
}

static void append(Lockstep *ls, const char *fmt, ...)
{
    size_t len = strlen(ls->report);
    if (len + 1 >= sizeof(ls->report))
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ls->report + len, sizeof(ls->report) - len, fmt, ap);
    va_end(ap);
}

static void append_cpu(Lockstep *ls, const char *label, const LockstepCpu *s)
{
    append(ls, "  %-11s PC=%04X A=%02X X=%02X Y=%02X S=%02X P=%02X V=%d IRQ=%X M=%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X"
               " cycles=%llu master=%llu\n",
           label, s->pc, s->a, s->x, s->y, s->s, s->p, s->speed, s->irq_request, s->mpr[0], s->mpr[1], s->mpr[2],
           s->mpr[3], s->mpr[4], s->mpr[5], s->mpr[6], s->mpr[7], (unsigned long long)s->cycles,
           (unsigned long long)s->master_cycles);
}

static void diverge(Lockstep *ls, const char *what)
{
    ls->diverged = 1;
    ls->test->cpu.yield = 1;
    snprintf(ls->report, sizeof(ls->report),
             "DIVERGENCE between the recompiled code and the interpreter: %s\n"
             "  after %llu compared instructions, at frame %llu\n",
             what, (unsigned long long)ls->instructions, (unsigned long long)ls->frames);
    append(ls, "Last instructions of the recompiled code (state BEFORE each one, the last is the one that diverged):\n");
    for (int i = 0; i < LOCKSTEP_HISTORY; i++) {
        const LockstepCpu *h = &ls->history[(ls->history_pos + i) % LOCKSTEP_HISTORY];
        if (h->cycles || h->pc)
            append_cpu(ls, "", h);
    }
}

/* Hook of the test console: before, saves the state; after, steps the reference one instruction and compares. */
static void on_instruction(Pce *test, int after)
{
    Lockstep *ls = (Lockstep *)test->user;
    if (ls->diverged)
        return;
    if (!after) {
        capture(&ls->before, test);
        ls->history[ls->history_pos] = ls->before;
        ls->history_pos = (ls->history_pos + 1) % LOCKSTEP_HISTORY;
        return;
    }
    LockstepCpu ref_before;
    capture(&ref_before, ls->ref);
    if (!same_cpu(&ref_before, &ls->before)) {
        diverge(ls, "state BEFORE the instruction");
        append_cpu(ls, "recompiled", &ls->before);
        append_cpu(ls, "reference", &ref_before);
        return;
    }
    ls->ref->pad_buttons = test->pad_buttons;
    ls->ref->game_patch = test->game_patch; /* the same option that changes the game */
    pce_step(ls->ref); /* interpreter (the reference has no recomp_lookup) or C BIOS routine */
    ls->instructions++;

    LockstepCpu t, r;
    capture(&t, test);
    capture(&r, ls->ref);
    if (!same_cpu(&t, &r)) {
        diverge(ls, "CPU state after the instruction");
        append(ls, "After the instruction at %04X:\n", ls->before.pc);
        append_cpu(ls, "recompiled", &t);
        append_cpu(ls, "reference", &r);
    }
}

void lockstep_init(Lockstep *ls, Pce *test, Pce *ref)
{
    memset(ls, 0, sizeof(*ls));
    ls->test = test;
    ls->ref = ref;
    test->on_instruction = on_instruction;
    test->user = ls;
    ref->recomp_lookup = NULL; /* the reference is always the interpreter */
}

static int compare_block(Lockstep *ls, const char *name, const void *a, const void *b, size_t size, size_t unit)
{
    if (memcmp(a, b, size) == 0)
        return 0;
    const uint8_t *pa = (const uint8_t *)a, *pb = (const uint8_t *)b;
    size_t i = 0;
    while (pa[i] == pb[i])
        i++;
    char what[128];
    snprintf(what, sizeof(what), "%s differs at the end of the frame (position %zu)", name, i / unit);
    diverge(ls, what);
    return 1;
}

int lockstep_end_frame(Lockstep *ls)
{
    if (ls->diverged)
        return 1;
    Pce *t = ls->test, *r = ls->ref;
    /* The reference does not go through pce_run_frame: close its frame the same way. */
    r->frame_ready = 0;
    pce_end_frame_audio(r);
    ls->frames++;

    /* CD games: CD RAM (where the code loaded from the disc runs), save RAM and ADPCM RAM. */
    if (t->cd && r->cd &&
        (compare_block(ls, "CD RAM", t->cd->ram, r->cd->ram, sizeof(t->cd->ram), 1) ||
         compare_block(ls, "save RAM", t->cd->bram, r->cd->bram, sizeof(t->cd->bram), 1) ||
         compare_block(ls, "ADPCM RAM", t->cd->adpcm.ram, r->cd->adpcm.ram, sizeof(t->cd->adpcm.ram), 1)))
        return 1;

    if (compare_block(ls, "RAM", t->ram, r->ram, sizeof(t->ram), 1) ||
        compare_block(ls, "VRAM", t->vdc.vram, r->vdc.vram, sizeof(t->vdc.vram), 2) ||
        compare_block(ls, "sprite attribute table", t->vdc.sat, r->vdc.sat, sizeof(t->vdc.sat), 2) ||
        compare_block(ls, "VDC registers", t->vdc.reg, r->vdc.reg, sizeof(t->vdc.reg), 2) ||
        compare_block(ls, "palette", t->vce.color, r->vce.color, sizeof(t->vce.color), 2) ||
        compare_block(ls, "image", t->vce.frame, r->vce.frame, sizeof(t->vce.frame), 4) ||
        compare_block(ls, "sound", t->audio, r->audio, sizeof(int16_t) * (size_t)t->audio_count, 2))
        return 1;
    if (t->audio_count != r->audio_count) {
        diverge(ls, "sound sample count differs");
        return 1;
    }
    return 0;
}

int lockstep_write_report(const Lockstep *ls, const char *path)
{
    if (!ls->diverged)
        return 0;
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fputs(ls->report, f);
    fclose(f);
    return 0;
}
