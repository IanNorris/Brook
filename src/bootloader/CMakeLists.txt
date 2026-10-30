cmake_minimum_required(VERSION 3.25)
project(brook-bootloader CXX)

if(NOT DEFINED BROOK_ROOT)
    set(BROOK_ROOT "${CMAKE_SOURCE_DIR}/../..")
endif()

set(UEFI_INCLUDE_DIRS
    "${BROOK_ROOT}/vendor/uefi-headers/Include"
    "${BROOK_ROOT}/vendor/uefi-headers/Include/X64"
)

set(SHARED_INCLUDE_DIRS
    "${BROOK_ROOT}/src/shared"
    "${BROOK_ROOT}/src/shared/inc_km"
)

find_program(BOOT_PYTHON python3 REQUIRED)
set(BOOT_LOGO_HEADER "${CMAKE_CURRENT_BINARY_DIR}/brook_boot_logo_data.h")
add_custom_command(OUTPUT "${BOOT_LOGO_HEADER}"
    COMMAND ${BOOT_PYTHON} "${BROOK_ROOT}/scripts/bake_logo.py"
            "${BROOK_ROOT}/data/branding/brook-mark.png" "${BOOT_LOGO_HEADER}" 300
    DEPENDS "${BROOK_ROOT}/scripts/bake_logo.py" "${BROOK_ROOT}/data/branding/brook-mark.png"
    VERBATIM)
add_executable(bootloader
    "${BOOT_LOGO_HEADER}"
    src/main.cpp
    src/config.cpp
    src/console.cpp
    src/graphics.cpp
    src/memory.cpp
    src/acpi.cpp
    src/update_slots.cpp
    src/fs.cpp
    src/elf_loader.cpp
    src/paging.cpp
    src/runtime.cpp
)

target_include_directories(bootloader PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
    ${UEFI_INCLUDE_DIRS}
    ${SHARED_INCLUDE_DIRS}
    src   # so files can include each other without path prefix
)

target_compile_options(bootloader PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wshadow
    -Wno-unused-parameter
)

set_target_properties(bootloader PROPERTIES
    OUTPUT_NAME "BOOTX64"
    SUFFIX      ".efi"
)

target_link_options(bootloader PRIVATE
    "/entry:EfiMain"
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)
