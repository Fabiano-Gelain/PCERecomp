cmake_minimum_required(VERSION 3.24)
project(PCERecomp C)

# PCERecomp: the emulated PC-Engine / TurboGrafx-16 console (HuC6280 CPU, video, sound, CD-ROM², BIOS routines
# rewritten in C) and the recompilation and verification tools. Builds standalone (its own tests) or inside a
# game project (add_subdirectory), which adds the tests that need the disc.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    # .pdb in Release too, so the profiler (--profile) can name functions.
    add_compile_options($<$<CONFIG:Release>:/Zi>)
    add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
    # C runtime linked into the .exe (/MT): runs without the "Visual C++ Redistributable".
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Cross-file optimization (LTCG) in Release: the hardware clock calls small functions from several files on
# every instruction.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# Emulated hardware core (no SDL).
add_library(pce_core STATIC
    runtime/cpu/huc6280.c
    runtime/pce/pce.c
    runtime/pce/vce.c
    runtime/pce/vdc.c
    runtime/pce/psg.c
    runtime/pce/mix.c
    runtime/pce/coverage.c
    runtime/pce/lockstep.c
    runtime/pce/bios_profile.c
    runtime/bios/hle.c
    runtime/bios/routines.c
    runtime/bios/hle_ex.c
    runtime/bios/hle_math.c
    runtime/bios/hle_irq.c
    runtime/bios/hle_cd.c
    runtime/bios/hle_adpcm.c
    runtime/bios/hle_bram.c
    runtime/bios/hle_font.c
    runtime/bios/hle_psg.c
    runtime/bios/hle_boot.c
    runtime/bios/synth.c
    runtime/cd/cdimage.c
    runtime/cd/adpcm.c
    runtime/cd/cdaudio.c
    runtime/cd/cdrom.c
    runtime/cd/origin.c
    runtime/cd/scsi.c
    runtime/util/u8file.c)
target_include_directories(pce_core PUBLIC runtime)

# Input script of recorded sessions (--input/--record), used by the game programs and by the tools.
add_library(pce_input STATIC tools/trace/input_script.c)
target_include_directories(pce_input PUBLIC tools/trace)
target_link_libraries(pce_input PUBLIC pce_core)

# Sampling profiler (--profile).
add_library(trace_profiler STATIC tools/trace/profiler.c)
target_include_directories(trace_profiler PUBLIC tools/trace)
if(WIN32)
    target_link_libraries(trace_profiler PUBLIC dbghelp winmm)
endif()

# pce_trace: instruction-by-instruction trace and console frames (tools/trace/pce_trace.c; pce_trace --help). A game
# project builds its own, with an extension (tools/trace/pce_trace.h: TraceExt), via
# pce_trace_executable(<name> <extension sources>), and sets PCE_GAME_TRACE before add_subdirectory so that the
# pce_trace name goes to its own.
set(PCE_TRACE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/tools/trace/pce_trace.c" CACHE INTERNAL "")
function(pce_trace_executable name)
    add_executable(${name} "${PCE_TRACE_SOURCE}" ${ARGN})
    target_link_libraries(${name} PRIVATE pce_core pce_input trace_profiler)
endfunction()
if(NOT PCE_GAME_TRACE)
    pce_trace_executable(pce_trace tools/trace/pce_trace_main.c)
endif()

# Checks the BIOS routines rewritten in C against the real BIOS (runtime/bios/hle.h).
add_executable(bios_check tools/trace/bios_check.c)
target_link_libraries(bios_check PRIVATE pce_core pce_input)

# Summary and track contents of a CD image (CUE/BIN).
add_executable(cd_info tools/cd/cd_info.c)
target_link_libraries(cd_info PRIVATE pce_core)

# ---- Tests ----
enable_testing()
find_program(PYTHON_EXE NAMES python python3)

add_executable(huc6280_tests tests/cpu/huc6280_tests.c)
target_link_libraries(huc6280_tests PRIVATE pce_core)
add_test(NAME cpu_huc6280 COMMAND huc6280_tests "${CMAKE_CURRENT_SOURCE_DIR}/reference/huc6280-tests/v1")

add_executable(pce_tests tests/pce/pce_tests.c)
target_link_libraries(pce_tests PRIVATE pce_core pce_input)
add_test(NAME pce_memory_io COMMAND pce_tests)

if(PYTHON_EXE)
    # Recompiler (Python): ROM reader, opcode table, disassembler, C generator, disc map.
    add_test(NAME recomp_python COMMAND ${PYTHON_EXE} -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tools/recomp/tests" -v)
    # CD image (CUE/BIN): track summary and contents match what Geargrafx reads, on synthetic images
    # (INDEX 00 and PREGAP pauses, several tracks per file, MODE1/2048) and, if the game project provides a disc
    # (PCE_TEST_CUE), on it too.
    set(PCE_CD_TEST_ARGS)
    if(PCE_TEST_CUE)
        set(PCE_CD_TEST_ARGS --cue "${PCE_TEST_CUE}")
    endif()
    add_test(NAME cd_image COMMAND ${PYTHON_EXE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/cd/run_cd_test.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/reference_trace/build/Release/gg_cdinfo.exe"
        $<TARGET_FILE:cd_info> "${CMAKE_BINARY_DIR}/cd_output" ${PCE_CD_TEST_ARGS})
    set_tests_properties(cd_image PROPERTIES SKIP_RETURN_CODE 77 LABELS console)
    # Tools without any game: pce_trace with a synthetic ROM (trace, frames, --hashes) and each tool's --help.
    # Inside a game project, pce_trace is the game's own (with its extension).
    add_test(NAME tools COMMAND ${PYTHON_EXE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/trace/run_tools_test.py"
        $<TARGET_FILE:pce_trace> $<TARGET_FILE:bios_check> $<TARGET_FILE:cd_info> "${CMAKE_BINARY_DIR}/tools_test")
    set_tests_properties(tools PROPERTIES LABELS quick)
endif()
set_tests_properties(cpu_huc6280 pce_memory_io PROPERTIES LABELS quick)
if(TEST recomp_python)
    set_tests_properties(recomp_python PROPERTIES LABELS quick)
endif()
