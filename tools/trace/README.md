# Traces: comparison with the reference emulator

A *trace* is the instruction-by-instruction record of the CPU state **before** each instruction:

```
E000 A:00 X:00 Y:00 S:00 P:04 V:0 M:00,00,00,00,00,00,00,00
```

`V` = speed (0 = CSL, 1 = CSH); `M` = MPR0..MPR7.

All tools respond to `--help`.

| Tool | What it does |
|---|---|
| `build\Release\pce_trace.exe <rom> <n> <output>` | trace of our runtime (built by `build.ps1`); `--frame` runs frames (below) |
| `tools\reference_trace\build\Release\gg_trace.exe <rom> <n> <output>` | trace of Geargrafx, used unmodified as a library. Separate CMake project (the GPL code does not go into the runtime). Build once with `.\tools\reference_trace\build.ps1`. Options: `--input`, `--from-frame`, `--frame`, `--audio`, `--hashes`, `--bios`, `--bram` (same meaning as in `pce_trace`) |
| `python tools\trace\compare_trace.py <ref> <ours>` | shows the first differing line, the previous instructions and the fields that differ |
| `tools\trace\run_trace_test.py` | Geargrafx trace against ours, for the game project's tests; skipped if the ROM or `gg_trace` is missing |
| `tools\trace\run_tools_test.py` | `tools` test: `pce_trace` on a synthetic ROM and the `--help` of each tool |

Both sides set to 0 the values that real hardware leaves random at reset
(registers, MPR, RAM), so that the traces are comparable.

## Why Geargrafx and not Mednafen
The Geargrafx code is in `reference/` and can be run instruction by instruction from a program
of ours, without a GUI. Mednafen is still used for visual comparison.

## Frames (images)
`gg_trace --frame <rom> <n> <output.ppm>` and `pce_trace --frame <rom> <n> <output.ppm>` save frame n.
`python tools\trace\compare_frames.py <ref.ppm> <ours.ppm> --png <dir>` compares pixel by pixel and writes
`reference.png`, `ours.png` and `diff.png` (differing pixels in magenta).
## Sound
`--audio <output.raw>` (together with `--frame`, in both tools) writes the sound of all frames run:
signed 16-bit, interleaved stereo, 44100 Hz. `python tools\trace\compare_audio.py <ref.raw> <ours.raw> --wav <dir>`
compares value by value and writes `reference.wav` and `ours.wav` to listen to. `pce_trace --volumes G,CD,CHIP,EF,VZ[,F]`
uses the Mednafen mix with these volumes (the one of the PC version) instead of the Geargrafx one.
## Button script
`--input <file>` (in both tools) applies buttons at fixed frames. Format, one change per line:
`<frame> <buttons>`, with buttons `I`, `II`, `SELECT`, `RUN`, `UP`, `RIGHT`, `DOWN`, `LEFT` joined by `+`,
or `-` to release everything. The buttons change when the frame ends (`input_script.h`).
`--from-frame Q` (trace mode) only starts writing after frame Q, to compare ranges in the middle of the game.

**Game events**: lines `# <name> <frame> <numbers>` (name with lowercase letters and `_` only). For
`gg_trace` and the generic `pce_trace` they are comments; the game project records with `input_script_record_event`
what, besides the buttons, changed the console during the session (e.g. a PC option that changes the game) and, on
replay, asks for those of each name with `input_script_event`. A session with events diverges in Geargrafx from the
1st of them on.

## Recorded sessions
The game program records the buttons frame by frame in the script format (with `# game:`, `# bios:`,
`# frames:` header lines). If the session started from a save, the save goes along (a `.brm` with the session's name, header
`# save:`) and the check passes `--bram` to both tools. `pce_trace --bram-out <file>` writes the save memory at the
end. `--hashes <file>` (in both tools) writes the CRC of the image and the sound of each frame, to compare a whole
session at once. Known limitations: frames in which the game changes the resolution in the middle of the screen
(Geargrafx draws them 1024 wide) are not comparable; the sound is compared as a continuous stream (the split between
frames sometimes differs by a few values).

## CD-ROM²
`--bios <system card>` (in both tools and in the `run_trace_test.py`/`run_hle_timing_test.py` scripts) enables a
console with CD-ROM²; then `<rom>` is the CD image (`.cue`). Without `--bios`, `pce_trace` opens the `.cue` with the
synthetic BIOS (routines in C, `runtime/bios`; test: `run_nobios_test.py`). In `gg_trace`, the console is forced to
plain CD-ROM² and the random delay of the first disc read is fixed at 0. `python tools\cd\run_cd_test.py` (test
`cd_image`) checks the table of contents and the track contents read by `cd_info` against `gg_cdinfo`.

The tests against Geargrafx keep its output (traces, `--hashes`, sound, CD table of contents) and reuse it
while `gg_trace`/`gg_cdinfo`, the game, the BIOS, the script and the save do not change (`gg_cache.py`: a
`<output>.key` file next to it). `PCE_GG_CACHE=0` forces Geargrafx to run again. Equal traces are checked byte by
byte (fast); the line-by-line comparison only runs to describe a difference.

## BIOS routines in C: result and timing
- `bios_check.exe --bios S <cue> <frames> [--input P --bram B]`: each call to the C routines equal to the
  real BIOS (registers, memory, hardware writes) and, per routine, how many calls without an interrupt in the middle
  took the same time; `--routines E069,E07B`: only these routines (default `all`); `--pieces`: the first piece with
  a different duration; `--timings N`: lists the first N calls with a different time (frame, registers,
  $20F8-$20FF); `--detail E0D8:11`: everything about the 11th call of $E0D8.
- Finding the first wrong timing: `pce_trace --cycles [--input P --bram B] [--from-frame Q] --bios S <cue> N a.txt`
  and the same with `--hle` into `b.txt`; `python tools\trace\compare_hle_trace.py a.txt b.txt` stops at the first
  instruction that happens at a different cycle and shows the previous C routine (the suspect). Use `--no-p`: it
  ignores the flags and, when a C routine leaves A/X/Y different at an internal point, matches by PC/stack at the
  same cycle (with a warning). If the timing matches but the image does not, the suspect is a hardware write off the
  BIOS cycle inside a C routine.
- `run_hle_timing_test.py`: a session recorded with the BIOS, replayed with `--hle`, with the image equal to the
  real BIOS one in all frames (except the given ranges).
- `pce_trace --log-calls E000` (or addresses): the BIOS calls with frame, line and $20F8-$20FF; `BANK:ADDRESS`
  (e.g. `81:61F2`) = game routine in that bank.
- `pce_trace --bios-profile FILE` writes the BIOS routines used; `python tools\recomp\bios_report.py` merges several
  profiles into a Markdown report.
- Instruction timings to check a routine: the `k_cycles` table in `runtime/cpu/huc6280_exec.h` (branch
  taken +2) and the listing from `tools\recomp\recomp.py disasm <bios> <address> <n> --mpr FF,F8,00,00,00,00,00,00`.

## Studying the image and the state (without opening many images)
`pce_trace --frame` also accepts (full list: `pce_trace --help`): `--every N DIR` (one frame every N;
`--every-from FROM`: only from frame FROM on), `--dump-state FILE` (VRAM, colors, VDC registers, RAM and CD RAM at
the end), `--watch-vram ADDRESS` (who writes to this VRAM word, with the last 64 instructions; `--watch-ram
ADDRESS`: the same for a RAM byte $2000-$3FFF), `--watch-vce FROM TO` (in frames FROM..TO, who writes to the VCE
colors and who asks the BIOS for colors at `$221F`, with the 64 instructions before the 1st write to each address),
`--bat-writers FILE` (per frame, who wrote to the background map and the sprite table in VRAM: 3 routines of the
call stack, instruction, no. of writes, rectangle of cells; `PCE_BW_DETAIL=FROM-TO` lists each write;
`--bat-routine BB:EEEE` counts the frames in which that routine wrote to the map), `--state-log FILE` (at each frame:
RAM, sprite table and the scroll of each line, in binary, to search for variables with scripts; format in
`--help`), `--bios-reads FILE` (the BIOS bytes the game read as data). Tools:

| Tool | What it does |
|---|---|
| `ppm2png.py FILE.ppm...` | converts PPM to PNG |
| `contact_sheet.py OUTPUT.png COLUMNS REDUCTION FILE.ppm...` | contact sheet: several reduced images in one PNG |
| `lit_pixels.py [--changes] DIR` | lit pixels per image (finds screen changes from the numbers) |
| `vdc_state.py STATE` | reads the `--dump-state` file (library + summary: registers, palette 0, sprites) |
| `show_tiles.py STATE tiles "4B6,4B8"` / `map` | draws tiles as text / shows the background map |

## Performance
`pce_trace ... --profile FILE`: sampling profiler (`profiler.c`, Windows only; uses the `.pdb`, also generated in
Release). Writes the functions with the most self and total time and the hottest source lines (shows what the
compiler inlined); at the end shows the time and frames/s. `--interp-log FILE`: the ranges that went through the
interpreter the most (in the build with the game's recompiled code: what the recompiled code does not cover).

## pce_trace with a game's parts
A game project builds its own `pce_trace` with an extension (`pce_trace.h`: `TraceExt`): extra options
(shown in `--help`), what to do after each frame's buttons (session events), at the end of each frame and at the
end (summaries). In CMake: `set(PCE_GAME_TRACE ON)` before PCERecomp's `add_subdirectory` and
`pce_trace_executable(pce_trace <extension sources>)`; the build with the game's recompiled code is another
`pce_trace_executable` with `PCE_RECOMP` defined and linked to the generated C library (`--lockstep`, `--interpret`).
