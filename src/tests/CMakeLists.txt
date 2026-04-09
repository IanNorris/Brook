cmake_minimum_required(VERSION 3.25)
project(brook-tests CXX)

if(NOT DEFINED BROOK_ROOT)
    set(BROOK_ROOT "${CMAKE_SOURCE_DIR}/../..")
endif()

set(SHARED_INCLUDE_DIRS
    "${BROOK_ROOT}/src/shared"
    "${BROOK_ROOT}/src/shared/inc_km"
    "${BROOK_ROOT}/src/tests/common"
)

# Helper function to add a test binary
function(add_brook_test TEST_NAME)
    add_executable(${TEST_NAME}
        ${TEST_NAME}/main.cpp
        "${BROOK_ROOT}/src/shared/src_km/serial.cpp"
    )
    target_include_directories(${TEST_NAME} PRIVATE ${SHARED_INCLUDE_DIRS})
    target_compile_options(${TEST_NAME} PRIVATE
        -Wall -Wextra -Werror -Wshadow -Wno-unused-parameter
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

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)
