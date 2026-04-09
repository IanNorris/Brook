cmake_minimum_required(VERSION 3.25)
project(brook-kernel CXX)

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
# Kernel executable
# ---------------------------------------------------------------------------

add_executable(kernel
    src/kernel.cpp
    src/gdt.cpp
    src/idt.cpp
    src/pmm.cpp
    src/vmm.cpp
    src/heap.cpp
    src/acpi.cpp
    src/apic.cpp
    src/tty.cpp
    "${FONT_ATLAS_CPP}"
    "${BROOK_ROOT}/src/shared/src_km/serial.cpp"
)

target_include_directories(kernel PRIVATE
    ${SHARED_INCLUDE_DIRS}
    "${CMAKE_CURRENT_SOURCE_DIR}/src"      # so font_atlas.cpp can find font_atlas.h
    "${CMAKE_CURRENT_BINARY_DIR}"          # for the generated font_atlas.cpp
)

target_compile_options(kernel PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wshadow
    -Wno-unused-parameter
    -mgeneral-regs-only
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

