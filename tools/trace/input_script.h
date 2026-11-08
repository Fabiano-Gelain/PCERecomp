/*
 * Button script: at which frame each button combination takes effect.
 *
 * Format (text, one change per line; '#' starts a comment):
 *   <frame> <buttons>
 * <buttons> = names separated by '+' (I, II, SELECT, RUN, UP, RIGHT, DOWN, LEFT) or '-' (none).
 * The buttons change as soon as frame <frame> ends (0 = from the reset).
 *
 * It is also used to record a session (the game program, --record): input_script_record at each
 * frame and input_script_save every minute of play and at the end.
 *
 * Game events: "# <name> <frame> <n1> [n2 n3 n4]" (name with lowercase letters and '_' only; with '#', for whoever
 * does not know the line, like gg_trace, it is just a comment). The game project records what, besides the buttons,
 * changed the console during the session (e.g. a PC option that changes the game) with input_script_record_event, and
 * whoever replays the session asks for the events of each name with input_script_event after each frame.
 */
#ifndef PCE_INPUT_SCRIPT_H
#define PCE_INPUT_SCRIPT_H

#include <stdint.h>

#define INPUT_EVENT_NAME 16
#define INPUT_EVENT_VALUES 4

typedef struct InputEvent {
    char name[INPUT_EVENT_NAME];
    long frame;                   /* after how many frames */
    long v[INPUT_EVENT_VALUES];   /* the numbers after the frame */
    int n;                        /* how many */
    int taken;                    /* replay: already returned by input_script_event */
} InputEvent;

typedef struct InputScript {
    int count, capacity;
    int next;
    long *frame;
    uint8_t *buttons;
    /* Game events, in frame order */
    int ev_count, ev_capacity;
    InputEvent *ev;
} InputScript;

/* Returns 0 if the file was read. */
int input_script_load(InputScript *s, const char *path);

/* Applies the changes whose frame has been reached; returns the current button state. */
uint8_t input_script_update(InputScript *s, long frames_done, uint8_t current);

/* Recording: records the buttons in effect after <frames_done> frames (only if they changed). */
void input_script_record(InputScript *s, long frames_done, uint8_t buttons);
/* Recording: records event <name> after <frames_done> frames, with the numbers v[0..n-1] (up to 4). */
void input_script_record_event(InputScript *s, const char *name, long frames_done, const long *v, int n);
/* Replay: 1 (and the event in *out) for each event <name> recorded up to <frames_done> not yet returned, in
 * frame order; 0 when there are no more. */
int input_script_event(InputScript *s, const char *name, long frames_done, InputEvent *out);
/* Writes the script (header: optional "# ..." comments, or NULL). Returns 0 on success. */
int input_script_save(const InputScript *s, const char *path, const char *header);
void input_script_free(InputScript *s);

#endif
