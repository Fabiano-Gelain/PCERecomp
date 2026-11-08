/*
 * Button script. See input_script.h.
 */
#include "input_script.h"
#include "util/u8file.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Same bits as PCE_PAD_* (pce.h). */
static const char *const k_names[8] = { "I", "II", "SELECT", "RUN", "UP", "RIGHT", "DOWN", "LEFT" };

static int append(InputScript *s, long frame, uint8_t buttons)
{
    if (s->count == s->capacity) {
        int cap = s->capacity ? s->capacity * 2 : 1024;
        long *f = (long *)realloc(s->frame, (size_t)cap * sizeof(long));
        if (!f)
            return -1;
        s->frame = f;
        uint8_t *b = (uint8_t *)realloc(s->buttons, (size_t)cap);
        if (!b)
            return -1;
        s->buttons = b;
        s->capacity = cap;
    }
    s->frame[s->count] = frame;
    s->buttons[s->count] = buttons;
    s->count++;
    return 0;
}

/* Puts the event in the list, in frame order (same frame: in arrival order). */
static int append_event(InputScript *s, const char *name, long frame, const long *v, int n)
{
    if (s->ev_count == s->ev_capacity) {
        int cap = s->ev_capacity ? s->ev_capacity * 2 : 16;
        InputEvent *e = (InputEvent *)realloc(s->ev, (size_t)cap * sizeof(InputEvent));
        if (!e)
            return -1;
        s->ev = e;
        s->ev_capacity = cap;
    }
    int k = s->ev_count;
    while (k > 0 && s->ev[k - 1].frame > frame) {
        s->ev[k] = s->ev[k - 1];
        k--;
    }
    InputEvent *e = &s->ev[k];
    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->frame = frame;
    e->n = n < 0 ? 0 : n > INPUT_EVENT_VALUES ? INPUT_EVENT_VALUES : n;
    for (int j = 0; j < e->n; j++)
        e->v[j] = v[j];
    s->ev_count++;
    return 0;
}

/* "# <name> <frame> <n1> [n2 ...]": name with lowercase letters and '_' only, then integers only.
 * 1 = it is an event (recorded), 0 = it is not (just a comment), -1 = out of memory. */
static int parse_event(InputScript *s, const char *p)
{
    if (*p++ != '#')
        return 0;
    while (*p == ' ' || *p == '\t')
        p++;
    char name[INPUT_EVENT_NAME];
    int len = 0;
    while ((islower((unsigned char)*p) || *p == '_') && len < INPUT_EVENT_NAME - 1)
        name[len++] = *p++;
    name[len] = 0;
    if (len == 0 || (*p != ' ' && *p != '\t'))
        return 0;
    long nums[1 + INPUT_EVENT_VALUES];
    int n = 0;
    for (;;) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == 0 || *p == '\n' || *p == '\r')
            break;
        char *end;
        long x = strtol(p, &end, 10);
        if (end == p || (*end != 0 && *end != ' ' && *end != '\t' && *end != '\n' && *end != '\r'))
            return 0;
        if (n == 1 + INPUT_EVENT_VALUES)
            return 0;
        nums[n++] = x;
        p = end;
    }
    if (n < 2)
        return 0;
    return append_event(s, name, nums[0], nums + 1, n - 1) == 0 ? 1 : -1;
}

int input_script_load(InputScript *s, const char *path)
{
    memset(s, 0, sizeof(*s));
    FILE *f = u8_fopen(path, "r");
    if (!f)
        return -1;
    char line[256];
    int r = 0;
    while (r == 0 && fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        int ev = parse_event(s, p);
        if (ev != 0) {
            r = ev < 0 ? -1 : 0;
            continue;
        }
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == 0)
            continue;
        long frame;
        char names[200] = "";
        if (sscanf(p, "%ld %199s", &frame, names) < 2)
            continue;
        uint8_t b = 0;
        for (char *tok = strtok(names, "+"); tok; tok = strtok(NULL, "+"))
            for (int i = 0; i < 8; i++)
                if (strcmp(tok, k_names[i]) == 0)
                    b |= (uint8_t)(1 << i);
        r = append(s, frame, b);
    }
    fclose(f);
    return r;
}

uint8_t input_script_update(InputScript *s, long frames_done, uint8_t current)
{
    while (s->next < s->count && s->frame[s->next] <= frames_done)
        current = s->buttons[s->next++];
    return current;
}

void input_script_record(InputScript *s, long frames_done, uint8_t buttons)
{
    uint8_t last = s->count ? s->buttons[s->count - 1] : 0;
    if (buttons != last)
        append(s, frames_done, buttons);
}

void input_script_record_event(InputScript *s, const char *name, long frames_done, const long *v, int n)
{
    append_event(s, name, frames_done, v, n);
}

int input_script_event(InputScript *s, const char *name, long frames_done, InputEvent *out)
{
    for (int k = 0; k < s->ev_count && s->ev[k].frame <= frames_done; k++) {
        InputEvent *e = &s->ev[k];
        if (!e->taken && strcmp(e->name, name) == 0) {
            e->taken = 1;
            *out = *e;
            return 1;
        }
    }
    return 0;
}

int input_script_save(const InputScript *s, const char *path, const char *header)
{
    FILE *f = u8_fopen(path, "w");
    if (!f)
        return -1;
    if (header)
        fputs(header, f);
    int e = 0;
    for (int i = 0; i <= s->count; i++) {
        /* events in frame order: before the 1st button change of a frame after theirs */
        for (; e < s->ev_count && (i == s->count || s->ev[e].frame < s->frame[i]); e++) {
            fprintf(f, "# %s %ld", s->ev[e].name, s->ev[e].frame);
            for (int j = 0; j < s->ev[e].n; j++)
                fprintf(f, " %ld", s->ev[e].v[j]);
            fputc('\n', f);
        }
        if (i == s->count)
            break;
        fprintf(f, "%ld ", s->frame[i]);
        if (s->buttons[i] == 0) {
            fputs("-", f);
        } else {
            int first = 1;
            for (int b = 0; b < 8; b++)
                if (s->buttons[i] & (1 << b)) {
                    fprintf(f, "%s%s", first ? "" : "+", k_names[b]);
                    first = 0;
                }
        }
        fputc('\n', f);
    }
    return fclose(f) == 0 ? 0 : -1;
}

void input_script_free(InputScript *s)
{
    free(s->frame);
    free(s->buttons);
    free(s->ev);
    memset(s, 0, sizeof(*s));
}
