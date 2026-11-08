/*
 * Sampling performance profiler (Windows only; development tool).
 * A separate thread pauses the thread that called profiler_start ~1000 times per second,
 * records which function of the .exe it was in (and who called it) and resumes it. At the end, profiler_stop
 * writes the list of functions with the most samples: "self" = the CPU was in it; "total" = in it or in something
 * it called. Needs the .pdb next to the .exe to name the functions.
 */
#ifndef PROFILER_H
#define PROFILER_H

int profiler_start(void);
void profiler_stop(const char *path); /* path NULL = stderr */

#endif
