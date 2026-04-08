cmake_minimum_required(VERSION 3.25)
project(brook-bootloader CXX)

# BROOK_ROOT is passed from the superbuild
if(NOT DEFINED BROOK_ROOT)
    # Allow standalone builds by defaulting to relative path
    set(BROOK_ROOT "${CMAKE_SOURCE_DIR}/../..")
endif()

set(UEFI_INCLUDE_DIRS
    "${BROOK_ROOT}/vendor/uefi-headers/Include"
    "${BROOK_ROOT}/vendor/uefi-headers/Include/X64"
)

add_executable(bootloader
    src/main.cpp
)

target_include_directories(bootloader PRIVATE ${UEFI_INCLUDE_DIRS})

target_compile_options(bootloader PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wshadow
    -Wno-unused-parameter   # UEFI callbacks have fixed signatures
)

set_target_properties(bootloader PROPERTIES
    OUTPUT_NAME "BOOTX64"
    SUFFIX      ".efi"
)

target_link_options(bootloader PRIVATE
    "/entry:EfiMain"
)

# Generate compile_commands.json for clangd/IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)
