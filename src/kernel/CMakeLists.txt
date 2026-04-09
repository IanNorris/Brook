cmake_minimum_required(VERSION 3.25)
project(brook-kernel CXX)

if(NOT DEFINED BROOK_ROOT)
    set(BROOK_ROOT "${CMAKE_SOURCE_DIR}/../..")
endif()

set(SHARED_INCLUDE_DIRS
    "${BROOK_ROOT}/src/shared"
    "${BROOK_ROOT}/src/shared/inc_km"
)

add_executable(kernel
    src/kernel.cpp
    src/gdt.cpp
    src/idt.cpp
    src/pmm.cpp
    src/vmm.cpp
    src/heap.cpp
    src/acpi.cpp
    src/apic.cpp
    "${BROOK_ROOT}/src/shared/src_km/serial.cpp"
)

target_include_directories(kernel PRIVATE
    ${SHARED_INCLUDE_DIRS}
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
