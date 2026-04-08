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
)

add_executable(bootloader
    src/main.cpp
    src/console.cpp
    src/graphics.cpp
    src/memory.cpp
    src/acpi.cpp
    src/runtime.cpp
)

target_include_directories(bootloader PRIVATE
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
