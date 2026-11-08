/* Sampling profiler (profiler.h). */
#include "profiler.h"

#ifdef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <dbghelp.h>

#define MAX_SAMPLES 200000
#define MAX_DEPTH 24

static HANDLE g_target;
static HANDLE g_thread;
static volatile LONG g_stop;
static DWORD64 *g_stacks; /* MAX_DEPTH addresses per sample (0 = end) */
static long g_count;
static LARGE_INTEGER g_t0;

static DWORD WINAPI sampler(LPVOID arg)
{
    (void)arg;
    timeBeginPeriod(1);
    while (!g_stop && g_count < MAX_SAMPLES) {
        Sleep(1);
        if (SuspendThread(g_target) == (DWORD)-1)
            break;
        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_FULL;
        DWORD64 *out = &g_stacks[(size_t)g_count * MAX_DEPTH];
        if (GetThreadContext(g_target, &ctx)) {
            for (int d = 0; d < MAX_DEPTH && ctx.Rip; d++) {
                out[d] = ctx.Rip;
                DWORD64 base;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
                if (!fn) { /* function without an unwind table (leaf): the return address is on top of the stack */
                    ctx.Rip = *(DWORD64 *)ctx.Rsp;
                    ctx.Rsp += 8;
                    continue;
                }
                PVOID handler;
                DWORD64 frame;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &handler, &frame, NULL);
            }
            g_count++;
        }
        ResumeThread(g_target);
    }
    timeEndPeriod(1);
    return 0;
}

int profiler_start(void)
{
    g_stacks = (DWORD64 *)calloc((size_t)MAX_SAMPLES * MAX_DEPTH, sizeof(DWORD64));
    if (!g_stacks)
        return -1;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_target,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    QueryPerformanceCounter(&g_t0);
    g_thread = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    return g_thread ? 0 : -1;
}

typedef struct {
    char name[96];
    long self, total, last; /* last: last sample counted in total (so recursion is not counted twice) */
} Entry;

static Entry *g_entries;
static int g_nentries;

/* Address -> function index (per-address cache, open addressing) */
#define ADDR_HASH (1 << 18)
static DWORD64 g_addr_key[ADDR_HASH];
static int g_addr_val[ADDR_HASH];

static int entry_for(HANDLE proc, DWORD64 addr)
{
    unsigned h = (unsigned)((addr * 0x9E3779B97F4A7C15ull) >> 46) & (ADDR_HASH - 1);
    while (g_addr_key[h] && g_addr_key[h] != addr)
        h = (h + 1) & (ADDR_HASH - 1);
    if (g_addr_key[h])
        return g_addr_val[h];
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    char name[96];
    if (SymFromAddr(proc, addr, NULL, sym))
        snprintf(name, sizeof(name), "%s", sym->Name);
    else
        snprintf(name, sizeof(name), "?");
    int idx = -1;
    for (int i = 0; i < g_nentries; i++)
        if (strcmp(g_entries[i].name, name) == 0) {
            idx = i;
            break;
        }
    if (idx < 0) {
        g_entries = (Entry *)realloc(g_entries, sizeof(Entry) * (g_nentries + 1));
        idx = g_nentries++;
        memset(&g_entries[idx], 0, sizeof(Entry));
        g_entries[idx].last = -1;
        snprintf(g_entries[idx].name, sizeof(g_entries[idx].name), "%s", name);
    }
    g_addr_key[h] = addr;
    g_addr_val[h] = idx;
    return idx;
}

static int by_self(const void *a, const void *b)
{
    return ((const Entry *)b)->self > ((const Entry *)a)->self ? 1 : ((const Entry *)b)->self < ((const Entry *)a)->self ? -1 : 0;
}

static int by_total(const void *a, const void *b)
{
    return ((const Entry *)b)->total > ((const Entry *)a)->total ? 1 : ((const Entry *)b)->total < ((const Entry *)a)->total ? -1 : 0;
}

void profiler_stop(const char *path)
{
    if (!g_thread)
        return;
    LARGE_INTEGER t1, freq;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    InterlockedExchange(&g_stop, 1);
    WaitForSingleObject(g_thread, INFINITE);
    CloseHandle(g_thread);
    g_thread = NULL;

    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(proc, NULL, TRUE);
    for (long s = 0; s < g_count; s++) {
        const DWORD64 *st = &g_stacks[(size_t)s * MAX_DEPTH];
        for (int d = 0; d < MAX_DEPTH && st[d]; d++) {
            int e = entry_for(proc, d ? st[d] - 1 : st[d]); /* return - 1 = inside the call */
            if (d == 0)
                g_entries[e].self++;
            if (g_entries[e].last != s) {
                g_entries[e].total++;
                g_entries[e].last = s;
            }
        }
    }
    /* Self time per source line (also shows what the compiler inlined into another function) */
    typedef struct { char where[160]; long n; } LineCount;
    static LineCount lines[4096];
    int nlines = 0;
    for (long s = 0; s < g_count; s++) {
        IMAGEHLP_LINE64 li;
        DWORD disp;
        memset(&li, 0, sizeof(li));
        li.SizeOfStruct = sizeof(li);
        char where[160];
        if (SymGetLineFromAddr64(proc, g_stacks[(size_t)s * MAX_DEPTH], &disp, &li)) {
            const char *file = strrchr(li.FileName, '\\');
            snprintf(where, sizeof(where), "%s:%lu", file ? file + 1 : li.FileName, (unsigned long)li.LineNumber);
        } else {
            snprintf(where, sizeof(where), "?");
        }
        int i = 0;
        while (i < nlines && strcmp(lines[i].where, where) != 0)
            i++;
        if (i == nlines && nlines < 4096) {
            snprintf(lines[nlines].where, sizeof(lines[nlines].where), "%s", where);
            lines[nlines++].n = 0;
        }
        if (i < nlines)
            lines[i].n++;
    }
    SymCleanup(proc);

    FILE *f = path ? fopen(path, "w") : stderr;
    if (!f)
        f = stderr;
    double secs = (double)(t1.QuadPart - g_t0.QuadPart) / (double)freq.QuadPart;
    fprintf(f, "Profile: %ld samples in %.2f s\n", g_count, secs);
    double n = g_count ? (double)g_count : 1.0;
    qsort(g_entries, g_nentries, sizeof(Entry), by_self);
    fprintf(f, "\n-- Self time (the CPU was in the function) --\n");
    for (int i = 0; i < g_nentries && i < 40; i++)
        fprintf(f, "%6.2f%%  %s\n", 100.0 * g_entries[i].self / n, g_entries[i].name);
    qsort(g_entries, g_nentries, sizeof(Entry), by_total);
    fprintf(f, "\n-- Total time (the function and what it called) --\n");
    for (int i = 0; i < g_nentries && i < 40; i++)
        fprintf(f, "%6.2f%%  %s\n", 100.0 * g_entries[i].total / n, g_entries[i].name);
    fprintf(f, "\n-- Self time per source line --\n");
    for (int k = 0; k < 40; k++) {
        int best = -1;
        for (int i = 0; i < nlines; i++)
            if (lines[i].n > 0 && (best < 0 || lines[i].n > lines[best].n))
                best = i;
        if (best < 0)
            break;
        fprintf(f, "%6.2f%%  %s\n", 100.0 * lines[best].n / n, lines[best].where);
        lines[best].n = 0;
    }
    if (f != stderr)
        fclose(f);
}

#else
int profiler_start(void) { return -1; }
void profiler_stop(const char *path) { (void)path; }
#endif
