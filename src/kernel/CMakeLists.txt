cmake_minimum_required(VERSION 3.25)
project(brook-kernel C CXX)

if(NOT DEFINED BROOK_ROOT)
    set(BROOK_ROOT "${CMAKE_SOURCE_DIR}/../..")
endif()

set(SHARED_INCLUDE_DIRS
    "${BROOK_ROOT}/src/shared"
    "${BROOK_ROOT}/src/shared/inc_km"
)

# ---------------------------------------------------------------------------
# Font atlas baking — run at configure time so the generated source is always
# available before the build step starts.
# ---------------------------------------------------------------------------

# Allow override via environment variable (set by shell.nix).
if(DEFINED ENV{HACK_FONT_TTF} AND EXISTS "$ENV{HACK_FONT_TTF}")
    set(BROOK_FONT_TTF "$ENV{HACK_FONT_TTF}")
else()
    # Fallback: search common nix store paths.
    file(GLOB FONT_CANDIDATES
        "/nix/store/*/hack-font-*/share/fonts/truetype/Hack-Regular.ttf"
    )
    if(FONT_CANDIDATES)
        list(GET FONT_CANDIDATES 0 BROOK_FONT_TTF)
    else()
        message(FATAL_ERROR
            "Hack font not found. Run from the Brook nix-shell:\n"
            "  nix-shell  (sets HACK_FONT_TTF automatically)")
    endif()
endif()

set(BROOK_FONT_SIZE 16)
set(FONT_ATLAS_CPP "${CMAKE_CURRENT_BINARY_DIR}/font_atlas.cpp")
set(FAT_IMAGE_CPP  "${CMAKE_CURRENT_BINARY_DIR}/fat_test_image.cpp")

find_program(PYTHON3 python3 REQUIRED)

# Bake the font atlas at configure time so it's available immediately.
message(STATUS "Baking font atlas: ${BROOK_FONT_TTF} @ ${BROOK_FONT_SIZE}px")
execute_process(
    COMMAND ${PYTHON3}
            "${BROOK_ROOT}/scripts/bake_font.py"
            "${BROOK_FONT_TTF}"
            "${BROOK_FONT_SIZE}"
            "${FONT_ATLAS_CPP}"
    RESULT_VARIABLE BAKE_RESULT
    OUTPUT_VARIABLE BAKE_OUTPUT
    ERROR_VARIABLE  BAKE_ERROR
)
if(NOT BAKE_RESULT EQUAL 0)
    message(FATAL_ERROR "Font baking failed:\n${BAKE_ERROR}")
endif()
message(STATUS "${BAKE_OUTPUT}")

# Also add a custom target so the atlas is re-baked if the font or script changes.
add_custom_command(
    OUTPUT  "${FONT_ATLAS_CPP}"
    COMMAND ${PYTHON3}
            "${BROOK_ROOT}/scripts/bake_font.py"
            "${BROOK_FONT_TTF}"
            "${BROOK_FONT_SIZE}"
            "${FONT_ATLAS_CPP}"
    DEPENDS "${BROOK_ROOT}/scripts/bake_font.py" "${BROOK_FONT_TTF}"
    COMMENT "Baking font atlas (${BROOK_FONT_SIZE}px Hack)"
    VERBATIM
)

# ---------------------------------------------------------------------------
# FAT test image baking — generates a small FAT16 ramdisk embedded in the kernel.
# ---------------------------------------------------------------------------

set(FAT_IMAGE_SIZE_KB 360)
message(STATUS "Baking FAT test image (${FAT_IMAGE_SIZE_KB} KB)")
execute_process(
    COMMAND ${PYTHON3}
            "${BROOK_ROOT}/scripts/make_fat_image.py"
            "${FAT_IMAGE_CPP}"
            "${FAT_IMAGE_SIZE_KB}"
    RESULT_VARIABLE FAT_RESULT
    OUTPUT_VARIABLE FAT_OUTPUT
    ERROR_VARIABLE  FAT_ERROR
)
if(NOT FAT_RESULT EQUAL 0)
    message(FATAL_ERROR "FAT image baking failed:\n${FAT_ERROR}")
endif()
message(STATUS "${FAT_OUTPUT}")

add_custom_command(
    OUTPUT  "${FAT_IMAGE_CPP}"
    COMMAND ${PYTHON3}
            "${BROOK_ROOT}/scripts/make_fat_image.py"
            "${FAT_IMAGE_CPP}"
            "${FAT_IMAGE_SIZE_KB}"
    DEPENDS "${BROOK_ROOT}/scripts/make_fat_image.py"
    COMMENT "Baking FAT test image (${FAT_IMAGE_SIZE_KB} KB)"
    VERBATIM
)

# ---------------------------------------------------------------------------
# Kernel executable
# ---------------------------------------------------------------------------

add_executable(kernel
    src/kernel.cpp
    src/cpu.cpp
    src/device.cpp
    src/fatfs_glue.cpp
    src/gdt.cpp
    src/idt.cpp
    src/keyboard.cpp
    src/kprintf.cpp
    src/panic.cpp
    src/pmm.cpp
    src/ramdisk.cpp
    src/vfs.cpp
    src/vmm.cpp
    src/heap.cpp
    src/acpi.cpp
    src/apic.cpp
    src/tty.cpp
    "${FONT_ATLAS_CPP}"
    "${FAT_IMAGE_CPP}"
    "${BROOK_ROOT}/src/shared/src_km/serial.cpp"
    # FatFS (C source — compiled as part of the kernel)
    "${BROOK_ROOT}/src/third_party/fatfs/source/ff.c"
    "${BROOK_ROOT}/src/third_party/fatfs/source/ffsystem.c"
)

target_include_directories(kernel PRIVATE
    ${SHARED_INCLUDE_DIRS}
    "${CMAKE_CURRENT_SOURCE_DIR}/src"      # kernel src (ffconf.h here shadows submodule's)
    "${CMAKE_CURRENT_BINARY_DIR}"          # for the generated font_atlas.cpp
    "${BROOK_ROOT}/src/third_party/fatfs/source"  # ff.h, diskio.h
)

target_compile_options(kernel PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wshadow
    -Wno-unused-parameter
    -msse2
    -mfpmath=sse   # use SSE for scalar float, not x87 (cleaner ABI)
)

# Interrupt handler files and panic must NOT use SSE/FPU registers.
# -mgeneral-regs-only overrides the global -msse2/-mfpmath=sse for these files.
# Combined with __attribute__((interrupt)) this guarantees no XMM state is
# touched in ISRs, so no FXSAVE/FXRSTOR is needed in the interrupt path.
# panic.cpp is included because KernelPanic may be called from interrupt context.
set_source_files_properties(
    src/idt.cpp
    src/apic.cpp
    src/keyboard.cpp
    src/panic.cpp
    PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only;-mno-sse;-mno-sse2;-mfpmath="
)

# FatFS is third-party C code — suppress extra warnings that would fail -Werror.
set_source_files_properties(
    "${BROOK_ROOT}/src/third_party/fatfs/source/ff.c"
    "${BROOK_ROOT}/src/third_party/fatfs/source/ffsystem.c"
    PROPERTIES COMPILE_OPTIONS
        "-Wno-unused-parameter;-Wno-unused-function;-Wno-sign-compare;-Wno-implicit-fallthrough"
)

set_target_properties(kernel PROPERTIES
    OUTPUT_NAME "BROOK"
    SUFFIX      ".elf"
)

target_link_options(kernel PRIVATE
    -T "${CMAKE_CURRENT_SOURCE_DIR}/linker.ld"
    -nostdlib
    -static
    -z max-page-size=0x1000
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)

