# Reference tool: runs Geargrafx (reference/Geargrafx, GPL-3) instruction by instruction
# and writes a trace in the same format as our runtime (see tools/trace/README.md).
#
# CMake project SEPARATE from the main one: Geargrafx code is never linked into our runtime.
# Build: .\tools\reference_trace\build.ps1
cmake_minimum_required(VERSION 3.24)
project(gg_trace C CXX)

set(GG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../reference/Geargrafx")
set(GG_SRC "${GG_DIR}/src")
set(GG_DEPS "${GG_DIR}/platforms/shared/dependencies")
if(NOT EXISTS "${GG_SRC}/geargrafx_core.cpp")
    message(FATAL_ERROR "Geargrafx not found in ${GG_DIR}. See docs/hardware/INDEX.md (how to restore).")
endif()

set(CMAKE_CXX_STANDARD 11)

set(GG_CORE_SOURCES
    geargrafx_core adpcm audio arcade_card_mapper cdrom_audio cdrom_chd_file_adapter cdrom_chd_image
    cdrom_cuebin_image cdrom_mmi_image media_file media_file_native media_file_slice mmi_archive
    ogg_vorbis_decoder cdrom_image cdrom_media cdrom huc6202 huc6260 huc6270 huc6280 huc6280_functors
    huc6280_opcodes huc6280_psg input laseractive mapper mb128 media memory scsi_controller sf2_mapper
    trace_logger profiler turbolink vgm_recorder)
list(TRANSFORM GG_CORE_SOURCES PREPEND "${GG_SRC}/")
list(TRANSFORM GG_CORE_SOURCES APPEND ".cpp")

set(GG_DEP_SOURCES
    ${GG_DEPS}/libchdr/src/libchdr_bitstream.c
    ${GG_DEPS}/libchdr/src/libchdr_cdrom.c
    ${GG_DEPS}/libchdr/src/libchdr_chd.c
    ${GG_DEPS}/libchdr/src/libchdr_flac.c
    ${GG_DEPS}/libchdr/src/libchdr_huffman.c
    ${GG_DEPS}/lzma/src/LzFind.c
    ${GG_DEPS}/lzma/src/LzmaEnc.c
    ${GG_DEPS}/lzma/src/LzmaDec.c
    ${GG_DEPS}/lzma/src/CpuArch.c
    ${GG_DEPS}/minivorbis/minivorbis.c
    ${GG_DEPS}/miniz/miniz.c
    ${GG_DEPS}/qon/qon.cpp)

# Geargrafx core, built once and shared by both tools.
add_library(gg_core STATIC ${GG_CORE_SOURCES} ${GG_DEP_SOURCES})
target_include_directories(gg_core PUBLIC
    ${GG_SRC}
    ${GG_DEPS}/libchdr/include
    ${GG_DEPS}/lzma/include
    ${GG_DEPS}/minivorbis
    ${GG_DEPS}/miniz
    ${GG_DEPS}/json
    ${GG_DEPS}/qon
    ${GG_DEPS}/zstd)
target_compile_definitions(gg_core PUBLIC
    EMULATOR_BUILD="pce_recomp-ref" Z7_ST ZSTD_DISABLE_ASM _CRT_SECURE_NO_WARNINGS NOMINMAX)
if(MSVC)
    target_compile_options(gg_core PUBLIC /W0 /utf-8 /EHsc)
endif()

add_executable(gg_trace gg_trace.cpp)
target_link_libraries(gg_trace PRIVATE gg_core)

# Summary and track CRCs of a CUE/BIN image read by Geargrafx (compared against tools/cd/cd_info.c).
add_executable(gg_cdinfo gg_cdinfo.cpp)
target_link_libraries(gg_cdinfo PRIVATE gg_core)
