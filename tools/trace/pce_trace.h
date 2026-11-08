/*
 * pce_trace: instruction-by-instruction trace and frames of the emulated console (pce_trace.c), with a hook for a
 * game project's extensions.
 *
 * PCERecomp builds the generic pce_trace (pce_trace_main.c: no extension). A game project can build the same
 * pce_trace.c with its own extension (command-line options, what to do at each frame and the summary at the end):
 * it writes a TraceExt and a main that calls pce_trace_main(argc, argv, &ext). In CMake, PCERecomp's
 * pce_trace_executable(<name> <extension sources>) function builds the program (see CMakeLists.txt).
 */
#ifndef PCE_TRACE_H
#define PCE_TRACE_H

#include "input_script.h"
#include "pce/pce.h"

/* What the extension sees of the run. */
typedef struct TraceRun {
    Pce *pce;            /* the console */
    Pce *ref;            /* with --lockstep, the reference (interpreter); otherwise NULL */
    InputScript *input;  /* the button script (--input; empty without it): game events (input_script_event) */
    VdcLineLog *line_log; /* log of the image lines; whoever needs it enables it with pce->vdc.line_log = line_log */
    int frame_mode;      /* --frame */
    int lockstep;        /* --lockstep */
    int volumes;         /* --volumes: the mix (pce->mix) was already enabled before setup */
} TraceRun;

typedef struct TraceExt {
    const char *name;    /* program name in messages (e.g. the game's) */
    const char *help;    /* text of the extension's options, shown in --help after the generic ones */
    /* Option argv[*i]: 1 = it belonged to the extension (advance *i past the arguments it consumed), 0 = it did not,
     * -1 = error (the message was already shown; the program exits with 1). */
    int (*arg)(int argc, char **argv, int *i);
    /* After loading the game, the reset and the generic console options (--volumes, --bios...), before
     * --bram, --log-calls and the lockstep. Nonzero = error (exits with 1). May set pce->on_instruction
     * (--log-calls, if given, takes its place). */
    int (*setup)(TraceRun *r);
    /* --frame: at the start (frame = 0, before the 1st frame) and after each frame's buttons (frame = frames
     * run): events of the recorded session that change the console. */
    void (*input)(TraceRun *r, long frame);
    /* --frame: end of each frame, before --state-log, --hashes, the lockstep, --audio and --every. */
    void (*frame)(TraceRun *r, long frame);
    /* --frame: after the last frame, before saving the final image. Nonzero = failed (the image is saved
     * anyway and the program exits with 1). */
    int (*done)(TraceRun *r, long frames, const char *ppm_path);
} TraceExt;

/* Runs pce_trace with the extension (NULL = generic only). Returns the program's exit code. */
int pce_trace_main(int argc, char **argv, const TraceExt *ext);

/* Helpers for the extensions. */
int trace_save_ppm(const Pce *pce, const char *path);                      /* the current frame (RGB) */
int trace_save_ppm_argb(const uint32_t *px, int w, int h, const char *path); /* a 0xAARRGGBB image */

#endif
