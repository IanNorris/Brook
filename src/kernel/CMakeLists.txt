cmake_minimum_required(VERSION 3.25)
project(brook-kernel C CXX ASM)
option(BROOK_MAPPING_TEST_HOOKS "Enable guest-controlled mapping fault barriers (test kernels only)" OFF)
option(BROOK_REGISTER_TEST_HOOKS "Enable controlled kernel register probes (test kernels only)" OFF)
option(BROOK_SCHED_START_TEST_HOOKS "Force delayed-AP first-dispatch regression cases (test kernels only)" OFF)

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

# Resolve against this build environment on every configure. An older cached
# Python environment can lack Pillow after branding becomes a build input.
find_program(BROOK_PYTHON3 python3 REQUIRED NO_CACHE)

# Compile the Linux userspace ABI separately from the kernel target flags.
find_program(BROOK_VDSO_CLANG clang REQUIRED NO_CACHE)
set(VDSO_ELF "${CMAKE_CURRENT_BINARY_DIR}/brook-vdso.so")
set(VDSO_CPP "${CMAKE_CURRENT_BINARY_DIR}/vdso_code_page.cpp")
add_custom_command(
    OUTPUT "${VDSO_ELF}" "${VDSO_CPP}"
    COMMAND ${BROOK_VDSO_CLANG} --target=x86_64-linux-gnu -O2 -Wall -Wextra -Werror
            -ffreestanding -fPIC -fno-builtin -fno-stack-protector
            -fno-asynchronous-unwind-tables -fno-unwind-tables -nostdlib
            -fuse-ld=lld -shared -Wl,-Bsymbolic -Wl,--hash-style=both
            -Wl,-soname=linux-vdso.so.1
            "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/vdso/layout.ld"
            "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/vdso/versions.map"
            "${CMAKE_CURRENT_SOURCE_DIR}/vdso/clock.c" -o "${VDSO_ELF}"
    COMMAND ${BROOK_PYTHON3} "${BROOK_ROOT}/scripts/bake_vdso.py"
            "${VDSO_ELF}" --cpp "${VDSO_CPP}"
    DEPENDS vdso/clock.c vdso/layout.ld vdso/versions.map
            "${BROOK_ROOT}/scripts/bake_vdso.py"
    COMMENT "Building and validating Linux clock vDSO"
    VERBATIM
)

# Bake branding from the tracked source rather than a stale checked-in array.
set(BOOT_LOGO_HEADER "${CMAKE_CURRENT_BINARY_DIR}/brook_boot_logo_data.h")
add_custom_command(
    OUTPUT "${BOOT_LOGO_HEADER}"
    COMMAND ${BROOK_PYTHON3} "${BROOK_ROOT}/scripts/bake_logo.py"
            "${BROOK_ROOT}/data/branding/brook-mark.png"
            "${BOOT_LOGO_HEADER}" 300
    DEPENDS "${BROOK_ROOT}/scripts/bake_logo.py"
            "${BROOK_ROOT}/data/branding/brook-mark.png"
    COMMENT "Baking Brook boot logo"
    VERBATIM
)

# Bake the font atlas at configure time so it's available immediately.
message(STATUS "Baking font atlas: ${BROOK_FONT_TTF} @ ${BROOK_FONT_SIZE}px")
execute_process(
    COMMAND ${BROOK_PYTHON3}
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
    COMMAND ${BROOK_PYTHON3}
            "${BROOK_ROOT}/scripts/bake_font.py"
            "${BROOK_FONT_TTF}"
            "${BROOK_FONT_SIZE}"
            "${FONT_ATLAS_CPP}"
    DEPENDS "${BROOK_ROOT}/scripts/bake_font.py" "${BROOK_FONT_TTF}"
    COMMENT "Baking font atlas (${BROOK_FONT_SIZE}px Hack)"
    VERBATIM
)

# Native desktop fonts use separate final-size glyphs rather than stretching
# terminal pixels. Keeping the same source face preserves a consistent family.
set(DESKTOP_FONT_SOURCES "")
foreach(DESKTOP_SCALE IN ITEMS 150 200)
    math(EXPR DESKTOP_FONT_SIZE "16 * ${DESKTOP_SCALE} / 100")
    set(DESKTOP_FONT_CPP "${CMAKE_CURRENT_BINARY_DIR}/desktop_font_${DESKTOP_SCALE}.cpp")
    add_custom_command(
        OUTPUT "${DESKTOP_FONT_CPP}"
        COMMAND ${BROOK_PYTHON3} "${BROOK_ROOT}/scripts/bake_font.py"
                "${BROOK_FONT_TTF}" "${DESKTOP_FONT_SIZE}" "${DESKTOP_FONT_CPP}"
                "g_desktopFont${DESKTOP_SCALE}"
        DEPENDS "${BROOK_ROOT}/scripts/bake_font.py" "${BROOK_FONT_TTF}"
        COMMENT "Baking native desktop font (${DESKTOP_SCALE}%)"
        VERBATIM
    )
    list(APPEND DESKTOP_FONT_SOURCES "${DESKTOP_FONT_CPP}")
endforeach()

# ---------------------------------------------------------------------------
# FAT test image baking — generates a small FAT16 ramdisk embedded in the kernel.
# ---------------------------------------------------------------------------

set(FAT_IMAGE_SIZE_KB 360)
message(STATUS "Baking FAT test image (${FAT_IMAGE_SIZE_KB} KB)")
execute_process(
    COMMAND ${BROOK_PYTHON3}
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
    COMMAND ${BROOK_PYTHON3}
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
# Kernel symbol address table (addr → name reverse lookup for crash dumps)
# ---------------------------------------------------------------------------
# This is a two-pass build: the first link uses an empty stub table, then a
# POST_BUILD step runs gen_ksym_table.py on the ELF to produce the real table.
# Run 'cmake --build' a second time for the final symbol table to be linked in.

set(KSYM_TABLE_CPP "${CMAKE_CURRENT_BINARY_DIR}/ksym_addr_table.cpp")

if(NOT EXISTS "${KSYM_TABLE_CPP}")
    file(WRITE "${KSYM_TABLE_CPP}"
"// Stub — will be replaced by gen_ksym_table.py after first link\n\
#include \"ksym_addrs.h\"\n\
namespace brook {\n\
extern const KsymAddrEntry g_ksymAddrTable[] = {{0,\"(none)\"}};\n\
extern const uint32_t g_ksymAddrCount = 0;\n\
}\n")
endif()

# ---------------------------------------------------------------------------
# Kernel executable
# ---------------------------------------------------------------------------

add_executable(kernel
    "${BOOT_LOGO_HEADER}"
    src/kernel.cpp
    src/cpu.cpp
    src/device.cpp
    src/fatfs_glue.cpp
    src/fw_cfg.cpp
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
    src/user_copy.cpp
    src/user_copy_fault.cpp
    src/user_copy.S
    src/posix_timer.cpp
    src/process.cpp
    src/memory/address_space.cpp
    src/memory/mapping_transaction.cpp
    src/memory/file_page_cache.cpp
    src/memory/exec_page_cache.cpp
    src/memory/file_object.cpp
    src/record_locks.cpp
    src/file_locks.cpp
    src/memory/file_mapping_registry.cpp
    src/memory/mapping_pin_registry.cpp
    src/memory/mapping_ranges.cpp
    src/memory/user_mapping.cpp
    src/scheduler.cpp
    src/compositor.cpp
    src/window.cpp
    src/terminal.cpp
    src/rtc.cpp
    src/kvmclock.cpp
    src/vdso.cpp
    src/display.cpp
    src/gpu_compositor.cpp
    src/gpu_app.cpp
    src/mouse.cpp
    src/shell.cpp
    src/audio.cpp
    src/boot_logo.cpp
    src/klog.cpp
    src/smp.cpp
    src/ap_trampoline.S
    src/context_switch.S
    src/exception_stubs.S
    src/elf_loader.cpp
    src/exec_image.cpp
    src/vfs.cpp
    src/fatfs_vfs.cpp
    src/ext2_vfs.cpp
    src/ext4_vfs.cpp
    src/procfs.cpp
    src/procfs_vfs.cpp
    src/net.cpp
    src/shutdown.cpp
    src/lifecycle_channel.cpp
    src/debug_epoch.cpp
    src/debug_jobs.cpp
    src/debug_ui.cpp
    src/tcp.cpp
    src/virtio_blk.cpp
    src/gpt.cpp
    src/acpi.cpp
    src/apic.cpp
    src/tlb_test.cpp
    src/tty.cpp
    src/sync/kmutex.cpp
    src/sync/krwlock.cpp
    src/sync/ksemaphore.cpp
    src/serial_writer.cpp
    src/profiler.cpp
    src/watchdog.cpp
    src/sched_policy.cpp
    src/exception_info.cpp
    src/debug_overlay.cpp
    src/panic_screen.cpp
    # Default scheduler policy (statically linked fallback)
    "${BROOK_ROOT}/src/modules/sched_rr.cpp"
    # Memory subsystem
    src/memory/virtual_memory.cpp
    src/memory/physical_memory.cpp
    src/memory/heap.cpp
    src/ksym_addrs.cpp
    "${KSYM_TABLE_CPP}"
    "${FONT_ATLAS_CPP}"
    "${VDSO_CPP}"
    ${DESKTOP_FONT_SOURCES}
    "${FAT_IMAGE_CPP}"
    "${BROOK_ROOT}/src/shared/src_km/serial.cpp"
    # FatFS (C source — compiled from a build-dir copy so our ffconf.h overlay
    # is found naturally by #include "ffconf.h" in ff.c / ff.h).
    "${FATFS_BUILD_DIR}/ff.c"
    "${FATFS_BUILD_DIR}/ffsystem.c"
    "${FATFS_BUILD_DIR}/ffunicode.c"
    # Nayuki QR Code Generator (C source)
    "${BROOK_ROOT}/src/third_party/qrcodegen/qrcodegen.c"
    # LZ4 compression (C source) — used in panic QR for data compression
    "${BROOK_ROOT}/src/third_party/lz4/lz4.c"
)

target_include_directories(kernel PRIVATE
    ${SHARED_INCLUDE_DIRS}
    "${CMAKE_CURRENT_SOURCE_DIR}/src"      # kernel src
    "${CMAKE_CURRENT_SOURCE_DIR}/src/memory" # memory subsystem headers
    "${CMAKE_CURRENT_BINARY_DIR}"          # for the generated font_atlas.cpp
    "${FATFS_BUILD_DIR}"                  # ff.h, diskio.h, our ffconf.h overlay
    "${BROOK_ROOT}/src/third_party/qrcodegen"    # qrcodegen.h
    "${BROOK_ROOT}/src/third_party/lz4"          # lz4.h
)

target_compile_options(kernel PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wshadow
    -Wno-unused-parameter
    -Wno-unused-command-line-argument
    -msse2
    -mfpmath=sse   # use SSE for scalar float, not x87 (cleaner ABI)
    -fno-omit-frame-pointer  # enable RBP frame chain for profiler stack walking
)

# ---------------------------------------------------------------------------
# Build info defines (git hash + timestamp) for crash dumps
# ---------------------------------------------------------------------------
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${BROOK_ROOT}"
    OUTPUT_VARIABLE BROOK_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT BROOK_GIT_HASH)
    set(BROOK_GIT_HASH "unknown")
endif()

# Mark dirty (uncommitted) trees so a panic's git hash distinguishes a working
# build from the clean commit. Without this every uncommitted build reports the
# same short hash and the crash decoder cannot tell which ELF to symbolize
# against (silently mis-symbolizing). The decoder treats a "-dirty" suffix as
# "no symbol archive — use the exact local build ELF".
execute_process(
    COMMAND git status --porcelain --untracked-files=no
    WORKING_DIRECTORY "${BROOK_ROOT}"
    OUTPUT_VARIABLE BROOK_GIT_DIRTY
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT BROOK_GIT_DIRTY STREQUAL "")
    # Disambiguate dirty builds by a CONTENT hash of the working tree, so two
    # different uncommitted states don't both report the bare "+". `git stash
    # create` snapshots the index+worktree as a commit object; its ^{tree} is a
    # content-addressed SHA that is identical for identical content and changes
    # the instant any tracked file changes — clock-independent (unlike a build
    # timestamp, which Nix freezes to 1980 under SOURCE_DATE_EPOCH). The 8-hex
    # tag rides in the (widened, 20-byte) panic git-hash field. Ian can verify a
    # booted kernel matches his tree with:
    #   git rev-parse "$(git stash create)^{tree}" | cut -c1-8
    execute_process(
        COMMAND git stash create
        WORKING_DIRECTORY "${BROOK_ROOT}"
        OUTPUT_VARIABLE BROOK_DIRTY_OBJ
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT BROOK_DIRTY_OBJ STREQUAL "")
        execute_process(
            COMMAND git rev-parse "${BROOK_DIRTY_OBJ}^{tree}"
            WORKING_DIRECTORY "${BROOK_ROOT}"
            OUTPUT_VARIABLE BROOK_DIRTY_TREE
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    endif()
    if(BROOK_DIRTY_TREE STREQUAL "")
        set(BROOK_GIT_HASH "${BROOK_GIT_HASH}+")
    else()
        string(SUBSTRING "${BROOK_DIRTY_TREE}" 0 8 BROOK_DIRTY_TAG)
        set(BROOK_GIT_HASH "${BROOK_GIT_HASH}+${BROOK_DIRTY_TAG}")
    endif()
endif()

execute_process(
    COMMAND git rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY "${BROOK_ROOT}"
    OUTPUT_VARIABLE BROOK_GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT BROOK_GIT_BRANCH)
    set(BROOK_GIT_BRANCH "unknown")
endif()

string(TIMESTAMP BROOK_BUILD_DATE "%Y-%m-%d" UTC)
# In Nix's hermetic build sandbox, SOURCE_DATE_EPOCH forces the above
# to "1980-01-01" for reproducibility. Use the git commit date instead:
# it's more informative and doesn't depend on the build clock.
execute_process(
    COMMAND git log -1 --format=%cs
    WORKING_DIRECTORY "${BROOK_ROOT}"
    OUTPUT_VARIABLE BROOK_GIT_DATE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(BROOK_GIT_DATE)
    set(BROOK_BUILD_DATE "${BROOK_GIT_DATE}")
endif()

target_compile_definitions(kernel PRIVATE
    BROOK_GIT_HASH="${BROOK_GIT_HASH}"
    BROOK_GIT_BRANCH="${BROOK_GIT_BRANCH}"
    BROOK_BUILD_DATE="${BROOK_BUILD_DATE}"
    __BROOK_KERNEL__=1
)
if(BROOK_MAPPING_TEST_HOOKS)
    target_sources(kernel PRIVATE src/memory/mapping_fault_test.cpp)
    target_compile_definitions(kernel PRIVATE BROOK_MAPPING_TEST_HOOKS=1)
endif()
if(BROOK_REGISTER_TEST_HOOKS)
    target_sources(kernel PRIVATE src/register_test.cpp src/register_test.S)
    target_compile_definitions(kernel PRIVATE BROOK_REGISTER_TEST_HOOKS=1)
endif()

if(BROOK_SCHED_START_TEST_HOOKS)
    target_compile_definitions(kernel PRIVATE BROOK_SCHED_START_TEST_HOOKS=1)
endif()

# Interrupt handler files and panic must NOT use SSE/FPU registers.
# -mgeneral-regs-only overrides the global -msse2/-mfpmath=sse for these files.
# Combined with __attribute__((interrupt)) this guarantees no XMM state is
# touched in ISRs, so no FXSAVE/FXRSTOR is needed in the interrupt path.
# panic.cpp is included because KernelPanic may be called from interrupt context.
set_source_files_properties(
    src/idt.cpp
    src/apic.cpp
    src/tlb_test.cpp
    src/scheduler.cpp
    src/compositor.cpp
    src/keyboard.cpp
    src/mouse.cpp
    src/panic.cpp
    src/panic_screen.cpp
    src/panic_qr.cpp
    src/exception_info.cpp
    src/profiler.cpp
    src/smp.cpp
    src/ksym_addrs.cpp
    PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only;-mno-sse;-mno-sse2;-mfpmath="
)

# FatFS is third-party C code — suppress extra warnings that would fail -Werror.
set_source_files_properties(
    "${FATFS_BUILD_DIR}/ff.c"
    "${FATFS_BUILD_DIR}/ffsystem.c"
    "${FATFS_BUILD_DIR}/ffunicode.c"
    PROPERTIES COMPILE_OPTIONS
        "-Wno-unused-parameter;-Wno-unused-function;-Wno-sign-compare;-Wno-implicit-fallthrough;-Wno-unused-variable"
)

# qrcodegen is called from panic context — needs -mgeneral-regs-only AND warning suppression.
set_source_files_properties(
    "${BROOK_ROOT}/src/third_party/qrcodegen/qrcodegen.c"
    PROPERTIES COMPILE_OPTIONS
        "-mgeneral-regs-only;-mno-sse;-mno-sse2;-mfpmath=;-Wno-unused-parameter;-Wno-unused-function;-Wno-sign-compare;-Wno-implicit-fallthrough;-Wno-unused-variable"
)

# LZ4 — freestanding mode, no heap, panic-safe (no SSE).
set_source_files_properties(
    "${BROOK_ROOT}/src/third_party/lz4/lz4.c"
    PROPERTIES
        COMPILE_OPTIONS "-mgeneral-regs-only;-mno-sse;-mno-sse2;-mfpmath=;-include;${BROOK_ROOT}/src/third_party/lz4/lz4_freestanding.h;-Wno-unused-parameter;-Wno-unused-function;-Wno-sign-compare;-Wno-implicit-fallthrough;-Wno-unused-variable"
        COMPILE_DEFINITIONS "LZ4_FREESTANDING=1;LZ4_memcpy=memcpy;LZ4_memset=memset;LZ4_memmove=memmove"
)

# Generated symbol table: suppress unused-const-variable (the extern refs are
# in ksym_addrs.cpp, so clang can't see them from this TU).
set_source_files_properties(
    "${KSYM_TABLE_CPP}"
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-const-variable"
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
# POST_BUILD: generate kernel symbol address table from the linked ELF.
# The generated .cpp replaces the stub so the NEXT build links the real table.
# Two builds are required for a complete symbol table.
# ---------------------------------------------------------------------------
add_custom_command(TARGET kernel POST_BUILD
    COMMAND ${BROOK_PYTHON3} "${BROOK_ROOT}/scripts/gen_ksym_table.py"
            "$<TARGET_FILE:kernel>" "${KSYM_TABLE_CPP}"
    COMMENT "Generating kernel symbol address table (rebuild to link it in)"
    VERBATIM
)

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
    "-Wno-unused-command-line-argument"
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
    # Parse optional EXTRA_FLAGS argument.
    cmake_parse_arguments(MOD "" "" "EXTRA_FLAGS" ${ARGN})
    set(MOD_SOURCES ${MOD_UNPARSED_ARGUMENTS})
    set(MOD_OBJ  "${DRIVER_OUTPUT_DIR}/${mod_name}.mod")

    # Build -I flags from the list.
    set(INCLUDE_FLAGS)
    foreach(inc IN LISTS DRIVER_MODULE_INCLUDE_DIRS)
        list(APPEND INCLUDE_FLAGS "-I${inc}")
    endforeach()

    # Compile each source to an object, then partial-link with -r.
    # C files use CMAKE_C_COMPILER and skip C++-only flags.
    set(OBJ_FILES)
    foreach(src IN LISTS MOD_SOURCES)
        get_filename_component(src_name "${src}" NAME_WE)
        get_filename_component(src_ext  "${src}" EXT)
        set(obj "${DRIVER_OUTPUT_DIR}/${mod_name}_${src_name}.o")
        set(depfile "${obj}.d")

        if("${src_ext}" STREQUAL ".c")
            set(_compiler "${CMAKE_C_COMPILER}")
            # C files: use gnu11 (not c11) so GNU extensions like asm() work
            set(_lang_flags "-std=gnu11")
        else()
            set(_compiler "${CMAKE_CXX_COMPILER}")
            set(_lang_flags "")
        endif()

        add_custom_command(
            OUTPUT  "${obj}"
            COMMAND "${_compiler}"
                    ${DRIVER_MODULE_FLAGS}
                    ${_lang_flags}
                    ${MOD_EXTRA_FLAGS}
                    ${INCLUDE_FLAGS}
                    -MMD -MF "${depfile}" -MT "${obj}"
                    -c "${src}" -o "${obj}"
            # Custom compiler commands need explicit depfile integration;
            # otherwise edits to included kernel/driver headers are ignored.
            DEPFILE "${depfile}"
            DEPENDS "${src}" "${BROOK_ROOT}/src/kernel/src/module_abi.h"
            COMMENT "Compiling module source ${src}"
            VERBATIM
        )
        list(APPEND OBJ_FILES "${obj}")
    endforeach()

    # Partial link → relocatable ELF (.mod).
    # Use ld.lld directly — clang driver with --target=x86_64-elf fails
    # if it can't find a GCC cross-toolchain for driver detection.
    find_program(LLD_LINKER ld.lld REQUIRED)
    add_custom_command(
        OUTPUT  "${MOD_OBJ}"
        COMMAND "${LLD_LINKER}"
                "-r"
                ${OBJ_FILES}
                -o "${MOD_OBJ}"
        DEPENDS ${OBJ_FILES}
        COMMENT "Linking driver module ${mod_name}.mod"
        VERBATIM
    )

    add_custom_target("module_${mod_name}" ALL DEPENDS "${MOD_OBJ}")
    # Track module targets so the symbol checker can depend on them (it needs
    # the compiled .mod binaries to exist before it inspects them).
    set_property(GLOBAL APPEND PROPERTY BROOK_MODULE_TARGETS "module_${mod_name}")
endfunction()

# Add driver module subdirectory (will be populated when drivers are extracted).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/drivers/CMakeLists.txt")
    add_subdirectory(drivers)
endif()

# ---------------------------------------------------------------------------
# Build-time check: every undefined symbol referenced by a compiled module
# must resolve against the kernel's .ksymtab exports, or the module fails to
# load at boot. This inspects the .mod binaries directly (see the script for
# why a source-level scan is insufficient — cf. BRO-154).
# ---------------------------------------------------------------------------
get_property(BROOK_MODULE_TARGETS GLOBAL PROPERTY BROOK_MODULE_TARGETS)
add_custom_target(check_module_symbols ALL
    COMMAND bash "${BROOK_ROOT}/scripts/check-module-symbols.sh" "${DRIVER_OUTPUT_DIR}"
    WORKING_DIRECTORY "${BROOK_ROOT}"
    COMMENT "Checking module import/export symbol consistency"
    VERBATIM
)
if(BROOK_MODULE_TARGETS)
    add_dependencies(check_module_symbols ${BROOK_MODULE_TARGETS})
endif()
