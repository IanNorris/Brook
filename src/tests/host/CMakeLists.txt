cmake_minimum_required(VERSION 3.25)
project(brook-host-tests CXX)

if(NOT DEFINED BROOK_ROOT)
    set(BROOK_ROOT "${CMAKE_SOURCE_DIR}/../../..")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Host-native tests — compiled with the system compiler, linked with pthreads.
# These test kernel data structures in isolation without booting a kernel.

find_package(Threads REQUIRED)

function(add_host_test TEST_NAME)
    add_executable(${TEST_NAME}
        "${BROOK_ROOT}/src/tests/${TEST_NAME}/main.cpp"
        ${ARGN}
    )
    target_compile_options(${TEST_NAME} PRIVATE
        -Wall -Wextra -Werror -O2
    )
    target_link_libraries(${TEST_NAME} PRIVATE Threads::Threads)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endfunction()

enable_testing()

add_host_test(test_kmutex)
