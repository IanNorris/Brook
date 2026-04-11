cmake_minimum_required(VERSION 3.25)
project(brook-kernel C CXX ASM)

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
# FatFS build-directory copy
# ---------------------------------------------------------------------------
# FatFS ff.c uses #include "ffconf.h" which always finds the co-located file.
# To override settings without modifying the submodule, we copy the FatFS
# sources into the build tree and overlay our custom ffconf.h.

set(FATFS_SRC_DIR "${BROOK_ROOT}/src/third_party/fatfs/source")
set(FATFS_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/fatfs")

file(MAKE_DIRECTORY "${FATFS_BUILD_DIR}")
file(COPY
    "${FATFS_SRC_DIR}/ff.c"
    "${FATFS_SRC_DIR}/ff.h"
    "${FATFS_SRC_DIR}/ffsystem.c"
    "${FATFS_SRC_DIR}/ffunicode.c"
    "${FATFS_SRC_DIR}/diskio.h"
    DESTINATION "${FATFS_BUILD_DIR}"
)
# Overlay our custom config (LFN=1, VOLUMES=4, etc.)
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/src/ffconf.h" DESTINATION "${FATFS_BUILD_DIR}")

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
    src/input.cpp
    src/keyboard.cpp
    src/kprintf.cpp
    src/ksymtab.cpp
    src/module.cpp
    src/panic.cpp
    src/panic_qr.cpp
    src/base45.cpp
    src/pci.cpp
    src/ramdisk.cpp
    src/runtime.cpp
    src/syscall.cpp
    src/process.cpp
    src/scheduler.cpp
    src/compositor.cpp
    src/shell.cpp
    src/boot_logo.cpp
    src/klog.cpp
    src/smp.cpp
    src/ap_trampoline.S
    src/context_switch.S
    src/elf_loader.cpp
    src/vfs.cpp
    src/virtio_blk.cpp
    src/acpi.cpp
    src/apic.cpp
    src/tty.cpp
    src/sync/kmutex.cpp
    # Memory subsystem
    src/memory/virtual_memory.cpp
    src/memory/physical_memory.cpp
    src/memory/heap.cpp
    "${FONT_ATLAS_CPP}"
    "${FAT_IMAGE_CPP}"
    "${BROOK_ROOT}/src/shared/src_km/serial.cpp"
    # FatFS (C source — compiled from a build-dir copy so our ffconf.h overlay
    # is found naturally by #include "ffconf.h" in ff.c / ff.h).
    "${FATFS_BUILD_DIR}/ff.c"
    "${FATFS_BUILD_DIR}/ffsystem.c"
    "${FATFS_BUILD_DIR}/ffunicode.c"
    # Nayuki QR Code Generator (C source)
    "${BROOK_ROOT}/src/third_party/qrcodegen/qrcodegen.c"
)

target_include_directories(kernel PRIVATE
    ${SHARED_INCLUDE_DIRS}
    "${CMAKE_CURRENT_SOURCE_DIR}/src"      # kernel src
    "${CMAKE_CURRENT_SOURCE_DIR}/src/memory" # memory subsystem headers
    "${CMAKE_CURRENT_BINARY_DIR}"          # for the generated font_atlas.cpp
    "${FATFS_BUILD_DIR}"                  # ff.h, diskio.h, our ffconf.h overlay
    "${BROOK_ROOT}/src/third_party/qrcodegen"    # qrcodegen.h
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
    src/scheduler.cpp
    src/compositor.cpp
    src/keyboard.cpp
    src/panic.cpp
    src/smp.cpp
    PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only;-mno-sse;-mno-sse2;-mfpmath="
)

# FatFS is third-party C code — suppress extra warnings that would fail -Werror.
set_source_files_properties(
    "${FATFS_BUILD_DIR}/ff.c"
    "${FATFS_BUILD_DIR}/ffsystem.c"
    "${FATFS_BUILD_DIR}/ffunicode.c"
    "${BROOK_ROOT}/src/third_party/qrcodegen/qrcodegen.c"
    PROPERTIES COMPILE_OPTIONS
        "-Wno-unused-parameter;-Wno-unused-function;-Wno-sign-compare;-Wno-implicit-fallthrough;-Wno-unused-variable"
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

# ---------------------------------------------------------------------------
# Driver module build system
# ---------------------------------------------------------------------------
# Modules are relocatable ELF64 objects (ET_REL) that the kernel loads at
# runtime via ModuleLoad().  They are compiled with the same freestanding
# kernel flags but linked with -r (partial link) instead of producing an
# executable.
#
# Usage:
#   add_driver_module(virtio_blk
#       src/virtio_blk_mod.cpp
#       src/pci.cpp
#   )
#
# Output: build/drivers/<name>.mod
# The run-qemu.sh script copies *.mod into build/esp/drivers/ so they
# appear at /boot/drivers/ in the QEMU virtual FAT filesystem.

set(DRIVER_MODULE_FLAGS
    "--target=x86_64-elf"
    "-ffreestanding"
    "-fno-stack-protector"
    "-mno-red-zone"
    "-fno-asynchronous-unwind-tables"
    "-mcmodel=kernel"
    "-fno-exceptions"
    "-fno-rtti"
    "-g"
    "-Wall" "-Wextra" "-Werror"
    "-Wshadow"
    "-Wno-unused-parameter"
    "-msse2" "-mfpmath=sse"
)

set(DRIVER_MODULE_INCLUDE_DIRS
    ${SHARED_INCLUDE_DIRS}
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/memory"
    "${BROOK_ROOT}/src/third_party/fatfs/source"
)

# Output directory for .mod files.
set(DRIVER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/drivers")
file(MAKE_DIRECTORY "${DRIVER_OUTPUT_DIR}")

function(add_driver_module mod_name)
    set(MOD_SOURCES ${ARGN})
    set(MOD_OBJ  "${DRIVER_OUTPUT_DIR}/${mod_name}.mod")

    # Build -I flags from the list.
    set(INCLUDE_FLAGS)
    foreach(inc IN LISTS DRIVER_MODULE_INCLUDE_DIRS)
        list(APPEND INCLUDE_FLAGS "-I${inc}")
    endforeach()

    # Compile each source to an object, then partial-link with -r.
    set(OBJ_FILES)
    foreach(src IN LISTS MOD_SOURCES)
        get_filename_component(src_name "${src}" NAME_WE)
        set(obj "${DRIVER_OUTPUT_DIR}/${mod_name}_${src_name}.o")

        add_custom_command(
            OUTPUT  "${obj}"
            COMMAND "${CMAKE_CXX_COMPILER}"
                    ${DRIVER_MODULE_FLAGS}
                    ${INCLUDE_FLAGS}
                    -c "${src}" -o "${obj}"
            DEPENDS "${src}"
            COMMENT "Compiling module source ${src}"
            VERBATIM
        )
        list(APPEND OBJ_FILES "${obj}")
    endforeach()

    # Partial link → relocatable ELF (.mod).
    add_custom_command(
        OUTPUT  "${MOD_OBJ}"
        COMMAND "${CMAKE_CXX_COMPILER}"
                "--target=x86_64-elf"
                "-nostdlib" "-r"
                ${OBJ_FILES}
                -o "${MOD_OBJ}"
        DEPENDS ${OBJ_FILES}
        COMMENT "Linking driver module ${mod_name}.mod"
        VERBATIM
    )

    add_custom_target("module_${mod_name}" ALL DEPENDS "${MOD_OBJ}")
endfunction()

# Add driver module subdirectory (will be populated when drivers are extracted).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/drivers/CMakeLists.txt")
    add_subdirectory(drivers)
endif()


