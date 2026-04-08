cmake_minimum_required(VERSION 3.25)
project(Brook NONE)

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
