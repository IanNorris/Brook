cmake_minimum_required(VERSION 3.25)
project(Brook NONE)
option(BROOK_MAPPING_TEST_HOOKS "Enable mapping fault barriers in test kernels only" OFF)
option(BROOK_REGISTER_TEST_HOOKS "Enable controlled kernel register probes in test kernels only" OFF)
option(BROOK_SCHED_START_TEST_HOOKS "Force delayed-AP first-dispatch regression cases (test kernels only)" OFF)
set(BROOK_SMP_BOOT_TEST_STAGE "0" CACHE STRING "Test-only AP3 delay: 0 disabled, 1 trampoline, 2 private stack")
if(NOT BROOK_SMP_BOOT_TEST_STAGE MATCHES "^[012]$")
    message(FATAL_ERROR "BROOK_SMP_BOOT_TEST_STAGE must be 0, 1 or 2")
endif()

include(ExternalProject)

set(BROOK_ROOT ${CMAKE_SOURCE_DIR})
set(BROOK_BUILD_TYPE ${CMAKE_BUILD_TYPE})

if(NOT BROOK_BUILD_TYPE)
    set(BROOK_BUILD_TYPE "Debug")
endif()

# Bootloader - UEFI PE/COFF target
ExternalProject_Add(bootloader
    SOURCE_DIR  ${BROOK_ROOT}/src/bootloader
    BINARY_DIR  ${CMAKE_BINARY_DIR}/bootloader
    CMAKE_ARGS
        -DCMAKE_TOOLCHAIN_FILE=${BROOK_ROOT}/cmake/toolchains/uefi-x86_64-clang.cmake
        -DCMAKE_BUILD_TYPE=${BROOK_BUILD_TYPE}
        -DBROOK_ROOT=${BROOK_ROOT}
    BUILD_ALWAYS  TRUE
    INSTALL_COMMAND ""
)

# Kernel - bare metal ELF target
ExternalProject_Add(kernel
    SOURCE_DIR  ${BROOK_ROOT}/src/kernel
    BINARY_DIR  ${CMAKE_BINARY_DIR}/kernel
    CMAKE_ARGS
        -DCMAKE_TOOLCHAIN_FILE=${BROOK_ROOT}/cmake/toolchains/kernel-x86_64-clang.cmake
        -DCMAKE_BUILD_TYPE=${BROOK_BUILD_TYPE}
        -DBROOK_ROOT=${BROOK_ROOT}
        -DBROOK_MAPPING_TEST_HOOKS=${BROOK_MAPPING_TEST_HOOKS}
        -DBROOK_REGISTER_TEST_HOOKS=${BROOK_REGISTER_TEST_HOOKS}
        -DBROOK_SCHED_START_TEST_HOOKS=${BROOK_SCHED_START_TEST_HOOKS}
        -DBROOK_SMP_BOOT_TEST_STAGE=${BROOK_SMP_BOOT_TEST_STAGE}
    BUILD_ALWAYS  TRUE
    INSTALL_COMMAND ""
)

# Tests - use same kernel toolchain
ExternalProject_Add(tests
    SOURCE_DIR  ${BROOK_ROOT}/src/tests
    BINARY_DIR  ${CMAKE_BINARY_DIR}/tests
    CMAKE_ARGS
        -DCMAKE_TOOLCHAIN_FILE=${BROOK_ROOT}/cmake/toolchains/kernel-x86_64-clang.cmake
        -DCMAKE_BUILD_TYPE=${BROOK_BUILD_TYPE}
        -DBROOK_ROOT=${BROOK_ROOT}
    BUILD_ALWAYS  TRUE
    INSTALL_COMMAND ""
    DEPENDS kernel
)

# Host-native tests are built separately (see scripts/build.sh).
# They require a working host C++ compiler with pthreads support.
