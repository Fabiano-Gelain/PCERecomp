# PCERecomp

A **static recompiler** for PC Engine / TurboGrafx-16 games (HuCard and CD-ROM²), plus the emulated console the
recompiled code runs on and the tools used to prove the result is correct.

PCERecomp reads a game's HuC6280 machine code, translates it ahead of time into C, and compiles that C together with
a runtime that simulates the rest of the console (video, sound, CD drive, System Card). The generated C does not try
to be readable: it reproduces the CPU instruction by instruction, so the goal is that it behaves **exactly** like the
original, and that is checked automatically.

No ROM, disc image, BIOS or code generated from a game is distributed. A game project brings its own configuration
and uses PCERecomp as a library and a set of tools. It is used by
[YsRecomp](https://github.com/Fabiano-Gelain/YsRecomp), an enhanced PC version of a TurboGrafx-CD game.

## What is in it

| Part | Where | What it does |
|---|---|---|
| CPU | `runtime/cpu/` | HuC6280 interpreter (all 256 opcodes, including TAM/TMA, ST0-2, block transfers, CSL/CSH) and the support code for recompiled functions (`huc6280_recomp.h`) |
| Console | `runtime/pce/` | Banked memory map (MPR), VDC (background, sprites, interrupts, DMA), VCE (palette), timer, joypad, PSG sound, sound mixing, lockstep checker, coverage recorder, BIOS profiler |
| CD-ROM² | `runtime/cd/` | BIN/CUE reader, CD controller (SCSI commands with seek timing, interrupts), CD audio (play, pause, fade), ADPCM, and tracking of the disc origin of every RAM byte |
| System Card in C | `runtime/bios/` | The System Card BIOS routines a game calls, rewritten in C (CD, ADPCM, save RAM, sound driver, math, font, interrupts, boot), plus a synthetic BIOS image with vectors and tables only, so games can run **without a BIOS file** |
| Recompiler | `tools/recomp/` | Python: disassembler, control-flow analysis, disc maps, C generator |
| Verification | `tools/trace/`, `tools/cd/`, `tools/reference_trace/` | Instruction traces, frame and audio comparison, BIOS-in-C checker, CD image info, and a bridge to the Geargrafx emulator for reference traces |
| Hardware docs | `docs/hardware/` | Collected hardware documentation with sources and known gaps ([INDEX.md](docs/hardware/INDEX.md)) |
| Tests | `tests/` | CPU and console tests |

## How it works

### 1. Find the code

`tools/recomp/recomp.py` disassembles recursively from the interrupt vectors and from known entry points, tracking
which bank is mapped in each of the eight 8 KB windows of the HuC6280 address space (the MPR registers). It detects
functions, follows `JMP (abs,X)` jump tables read from the ROM, and follows bank switches (`TAM`) along the way.

- **HuCard games**: the code is in the ROM; the analysis starts from the vectors and a small per-game config.
- **CD-ROM² games** load their code from the disc into RAM, often over and over at the same addresses. A **disc map**
  (`games/<game>/disc.toml` in the game project) describes the disc's file table, which stretches of the disc are
  loaded where, which window each bank runs in, the entry points, routine tables and far-call stubs
  (format in [`tools/recomp/pcerecomp/discmap.py`](tools/recomp/pcerecomp/discmap.py)). With it, the recompiler finds
  **all** the game's code straight from the disc, without depending on recorded play sessions. Each load becomes a
  "virtual bank" analysed like a ROM bank, and the runtime tracks the disc origin of every RAM byte so it knows which
  one is in memory.
- Coverage files recorded by the runtime (`--coverage`) are then only a cross-check: `recomp.py disc ... --check`
  lists what a play session executed that the static analysis did not find, and how the game got there.

### 2. Generate C

`recomp.py generate` writes one C function per game function. Every instruction calls the **same opcode routine the
interpreter uses**, so the generated code inherits the validated CPU (flags, cycle counts, interrupts); branches
become `goto`. A dispatcher looks up (bank, address) to enter recompiled code. Code that runs from RAM first checks
its own bytes (a signature) and drops back to the dispatcher if the game rewrote it; jump targets that a game patches
at run time are read from RAM.

### 3. Fall back to the interpreter

Anything the analysis did not find (code the game builds in RAM, a path no map describes) still runs: the dispatcher
hands it to the interpreter. A game project can log what falls back to see whether its map is missing something.

### 4. Check everything

- **Interpreter vs. Geargrafx.** `pce_trace` writes an instruction-by-instruction trace (`PC A X Y S P` and the MPRs),
  frame images, audio and hashes; `gg_trace` produces the same from the
  [Geargrafx](https://github.com/drhelius/Geargrafx) emulator (used unmodified as a library). The comparison scripts
  show the first differing instruction, frame or sample. Geargrafx output is cached while nothing changes
  (`gg_cache.py`).
- **Recompiled vs. interpreter (lockstep).** `runtime/pce/lockstep.c` runs both side by side and compares the CPU
  state after every instruction, and RAM, VRAM, sprites, palette, image and sound after every frame, stopping at
  the first divergence. The chain is: recompiled = interpreter = Geargrafx.
- **CPU tests.** The interpreter passes the [SingleStepTests/huc6280](https://github.com/SingleStepTests/huc6280)
  suite (generated by Mesen; downloaded separately).
- **BIOS in C vs. real BIOS.** `bios_check` replays recorded sessions and, at every call of a BIOS routine, runs the C
  version from the same state as the real System Card: registers, RAMs, VRAM, VDC, palette and the order of hardware
  writes must all match. The real BIOS can still be loaded (`--bios`) as a reference.

## Status

- Used by **YsRecomp**: a complete CD-ROM² game recompiled from a disc map, playable from start to finish without a
  BIOS file, with recorded sessions replayed in lockstep as regression tests.
- The HuCard path was validated earlier on a simpler game (about 1.4 billion instructions in lockstep with no
  divergence) before the work moved to CD-ROM².
- Windows only for now (MSVC). The console runtime is plain C with no platform code; the window, input and audio
  belong to the game project.
- Not a general-purpose emulator: only what the games in use needed is implemented and verified. Known hardware gaps
  are listed in [docs/hardware/INDEX.md](docs/hardware/INDEX.md#known-gaps).

## Requirements

- Windows 10 or 11, 64-bit
- Visual Studio 2022 Build Tools (MSVC); the CMake that comes with them does not need to be in `PATH`, `build.ps1`
  finds it
- Python 3.14 (the recompiler and the test scripts)
- Optional, for reference traces: the Geargrafx sources in `reference/` and `.\tools\reference_trace\build.ps1`
  (see [docs/hardware/INDEX.md](docs/hardware/INDEX.md#how-to-restore-reference))

## Build and test

```powershell
.\build.ps1          # Release build in build\ (quiet: only warnings, errors and a summary)
.\build.ps1 -Test    # build and run the tests
.\build.ps1 -Verbose # show everything
```

Tests that need files you do not have (the CPU test data, a game disc) are skipped. Every tool answers `--help`; the
trace tools are described in [tools/trace/README.md](tools/trace/README.md).

## Repository layout

```
runtime/
  cpu/              HuC6280 interpreter and recompiled-code support
  pce/              memory map, VDC, VCE, PSG, mixing, lockstep, coverage
  cd/               CUE/BIN, CD controller, SCSI, CD audio, ADPCM, RAM origin tracking
  bios/             System Card routines in C, synthetic BIOS, boot
  util/             UTF-8 file paths
tools/
  recomp/           recompiler (recomp.py + the pcerecomp package, with unit tests)
  trace/            pce_trace, bios_check, input scripts, trace/frame/audio comparison, contact sheets
  cd/               cd_info and the CD image test
  reference_trace/  gg_trace / gg_cdinfo: Geargrafx bridge (GPL-3, separate CMake project)
tests/              CPU and console tests
docs/hardware/      hardware documentation and its sources
reference/          local reference sources (not in Git)
```

## Using it from a game project

1. **Add it as a submodule** at `external/PCERecomp` (or keep it in a sibling folder) and include it from CMake:

   ```cmake
   set(PCERECOMP_DIR "${CMAKE_SOURCE_DIR}/external/PCERecomp" CACHE PATH "PCERecomp folder")
   add_subdirectory("${PCERECOMP_DIR}" pcerecomp)
   ```

   This gives the `pce_core` library (CPU, console, CD, BIOS in C), `pce_input` (recorded input scripts) and the
   generic tools.
2. **Describe the game**: for a CD game, a disc map `games/<game>/disc.toml` (file table, families of loads, entry
   points, routine records, far calls). Generate the C with
   `python tools/recomp/recomp.py generate <bios> --map games/<game>/disc.toml --cd <image.cue> --out <folder>` and
   compile it as a library linked to `pce_core`. The generated C is derived from the game: keep it out of Git.
3. **Add game-specific behavior through the generic hooks**, never inside PCERecomp:
   - `pce_trace` extensions: write a `TraceExt` ([tools/trace/pce_trace.h](tools/trace/pce_trace.h)) and build your
     own trace tool with `pce_trace_executable(<name> <sources>)` (set `PCE_GAME_TRACE` before `add_subdirectory`);
   - session events in recorded input (`input_script_record_event` / `input_script_event`);
   - configurable disc-map rules (`script`, `routine_records`, `loose_pointers`);
   - `Pce.game_patch` for an intentional change applied on both sides of the lockstep;
   - `Vdc.line_log` and `Vdc.write_hook` to observe what the video chip draws and who writes it.
4. **Check it**: record play sessions, replay them in lockstep, compare them with Geargrafx, and run `bios_check` on
   them.

## License

PCERecomp is released under the [MIT License](LICENSE), Copyright (c) 2026 Fabiano Gelain.

Exception: `tools/reference_trace/` links the Geargrafx emulator and is GPL-3.0 on its own; nothing else links or
contains Geargrafx code. Geargrafx (GPL-3.0) and Mednafen (GPL-2.0) were read as references for hardware behavior; no
code was copied from them. Third-party documentation and test data are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

PC Engine and TurboGrafx-16 are trademarks of their respective owners. This project is not affiliated with them.
