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

add_host_test(test_tcp
    "${BROOK_ROOT}/src/kernel/src/tcp.cpp"
)
target_compile_options(test_tcp PRIVATE
    "-iquote${BROOK_ROOT}/src/kernel/src"
    "-iquote${BROOK_ROOT}/src/shared"
)

add_host_test(test_kmutex)
add_host_test(test_krwlock)
add_host_test(test_ksemaphore)
add_host_test(test_sched_policy
    "${BROOK_ROOT}/src/kernel/src/sched_policy.cpp"
)
target_compile_options(test_sched_policy PRIVATE
    "-iquote${BROOK_ROOT}/src/kernel/src"
)

# ---------------------------------------------------------------------------
# Scheduler module shared libraries (.so) for dlopen-based testing
# ---------------------------------------------------------------------------

add_library(sched_rr SHARED "${BROOK_ROOT}/src/modules/sched_rr.cpp")
target_include_directories(sched_rr PRIVATE "${BROOK_ROOT}/src/kernel/src")
set_target_properties(sched_rr PROPERTIES PREFIX "lib" SUFFIX ".so")

add_library(sched_mlfq SHARED "${BROOK_ROOT}/src/modules/sched_mlfq.cpp")
target_include_directories(sched_mlfq PRIVATE "${BROOK_ROOT}/src/kernel/src")
set_target_properties(sched_mlfq PROPERTIES PREFIX "lib" SUFFIX ".so")

# test_sched_modules — dlopen-based test that loads the .so modules
add_executable(test_sched_modules
    "${BROOK_ROOT}/src/tests/test_sched_modules/main.cpp"
)
target_compile_options(test_sched_modules PRIVATE
    -Wall -Wextra -Werror -O2
    "-iquote${BROOK_ROOT}/src/kernel/src"
)
target_link_libraries(test_sched_modules PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(test_sched_modules sched_rr sched_mlfq)

# Pass .so paths to the test binary.
add_test(NAME test_sched_modules
    COMMAND test_sched_modules
        "$<TARGET_FILE:sched_rr>"
        "$<TARGET_FILE:sched_mlfq>"
)
