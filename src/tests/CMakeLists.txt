cmake_minimum_required(VERSION 3.25)
project(brook-tests C CXX)

if(NOT DEFINED BROOK_ROOT)
    set(BROOK_ROOT "${CMAKE_SOURCE_DIR}/../..")
endif()

set(SHARED_INCLUDE_DIRS
    "${BROOK_ROOT}/src/shared"
    "${BROOK_ROOT}/src/shared/inc_km"
    "${BROOK_ROOT}/src/tests/common"
)

# Helper function to add a test binary.
# Usage: add_brook_test(TEST_NAME [extra_source1 extra_source2 ...])
function(add_brook_test TEST_NAME)
    add_executable(${TEST_NAME}
        ${TEST_NAME}/main.cpp
        "${BROOK_ROOT}/src/shared/src_km/serial.cpp"
        ${ARGN}
    )
    target_include_directories(${TEST_NAME} PRIVATE
        ${SHARED_INCLUDE_DIRS}
        "${BROOK_ROOT}/src/kernel/src"
    )
    target_compile_options(${TEST_NAME} PRIVATE
        -Wall -Wextra -Werror -Wshadow -Wno-unused-parameter
        -mno-sse -mno-sse2 -mgeneral-regs-only -fno-builtin
    )
    set_target_properties(${TEST_NAME} PROPERTIES
        OUTPUT_NAME "${TEST_NAME}"
        SUFFIX ".elf"
    )
    target_link_options(${TEST_NAME} PRIVATE
        -T "${BROOK_ROOT}/src/kernel/linker.ld"
        -nostdlib -static -z max-page-size=0x1000
    )
endfunction()

add_brook_test(test_memory_map)
add_brook_test(test_pmm
    "${BROOK_ROOT}/src/kernel/src/pmm.cpp"
    "${BROOK_ROOT}/src/kernel/src/vmm.cpp"
    "${BROOK_ROOT}/src/kernel/src/heap.cpp"
)
add_brook_test(test_vmm
    "${BROOK_ROOT}/src/kernel/src/pmm.cpp"
    "${BROOK_ROOT}/src/kernel/src/vmm.cpp"
    "${BROOK_ROOT}/src/kernel/src/heap.cpp"
)
add_brook_test(test_heap
    "${BROOK_ROOT}/src/kernel/src/pmm.cpp"
    "${BROOK_ROOT}/src/kernel/src/vmm.cpp"
    "${BROOK_ROOT}/src/kernel/src/heap.cpp"
)

add_brook_test(test_string)

add_brook_test(test_device_registry
    "${BROOK_ROOT}/src/kernel/src/device.cpp"
)

add_brook_test(test_vfs
    "${BROOK_ROOT}/src/kernel/src/vfs.cpp"
    "${BROOK_ROOT}/src/kernel/src/fatfs_glue.cpp"
    "${BROOK_ROOT}/src/kernel/src/device.cpp"
    "${BROOK_ROOT}/src/kernel/src/ramdisk.cpp"
    "${BROOK_ROOT}/src/kernel/src/heap.cpp"
    "${BROOK_ROOT}/src/kernel/src/pmm.cpp"
    "${BROOK_ROOT}/src/kernel/src/vmm.cpp"
)
target_include_directories(test_vfs PRIVATE
    "${BROOK_ROOT}/src/third_party/fatfs/source"
)
target_sources(test_vfs PRIVATE
    "${BROOK_ROOT}/src/third_party/fatfs/source/ff.c"
    "${BROOK_ROOT}/src/third_party/fatfs/source/ffunicode.c"
)
set_source_files_properties(
    "${BROOK_ROOT}/src/third_party/fatfs/source/ff.c"
    "${BROOK_ROOT}/src/third_party/fatfs/source/ffunicode.c"
    TARGET_DIRECTORY test_vfs
    PROPERTIES COMPILE_OPTIONS
        "-Wno-unused-parameter;-Wno-unused-function;-Wno-sign-compare;-Wno-implicit-fallthrough;-Wno-unused-variable"
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)
