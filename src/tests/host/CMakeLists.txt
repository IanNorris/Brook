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

# ---------------------------------------------------------------------------
# ThreadSanitizer layer (opt-in via -DBROOK_SANITIZE_THREAD=ON).
# Several of these tests drive the kernel lock primitives (kmutex/krwlock/
# ksemaphore) and lock-free paths under real pthreads, so TSan can flag data
# races and lock-order inversions that the assertions alone won't. TSan
# understands C++11 std::atomic (unlike valgrind/helgrind, which false-positives
# on it), making it the correct race detector for this codebase.
#
# Use scripts/run-tsan-tests.sh to build+run this variant. NOTE: TSan must
# disable ASLR via the personality() syscall at startup; environments that
# block that syscall (some sandboxes/containers) cannot run TSan binaries.
option(BROOK_SANITIZE_THREAD "Build host tests with ThreadSanitizer" OFF)
if(BROOK_SANITIZE_THREAD)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=thread)
endif()

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
    "-iquote${BROOK_ROOT}/src/shared/inc_km"
    -DBROOK_HOST_TEST
)

add_host_test(test_kmutex)
add_host_test(test_krwlock)
add_host_test(test_ksemaphore)

# Real-code harness driving the actual kernel krwlock.cpp (BRO-162). Compiles
# the real lock implementation against shim process.h/scheduler.h, with cli/sti
# neutralised via BROOK_HOST_TEST so the privileged guard runs in user space.
add_host_test(test_krwlock_cleanup
    "${BROOK_ROOT}/src/kernel/src/sync/krwlock.cpp"
)
target_compile_options(test_krwlock_cleanup PRIVATE
    -DBROOK_HOST_TEST
    "-iquote${BROOK_ROOT}/src/tests/test_krwlock_cleanup/shim"
    "-iquote${BROOK_ROOT}/src/kernel/src"
)
add_host_test(test_futex_pool)
target_compile_options(test_futex_pool PRIVATE
    "-iquote${BROOK_ROOT}/src/kernel/src"
)
add_host_test(test_fd_table)
target_compile_options(test_fd_table PRIVATE
    "-iquote${BROOK_ROOT}/src/kernel/src"
)
add_host_test(test_loopback)
target_compile_options(test_loopback PRIVATE
    "-iquote${BROOK_ROOT}/src/kernel/src"
)
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

# Ext2 filesystem structure tests (run against a real ext2 disk image)
add_executable(test_ext2
    "${BROOK_ROOT}/src/tests/test_ext2/main.cpp"
)
target_compile_options(test_ext2 PRIVATE -Wall -Wextra -Werror -O2)
add_test(NAME test_ext2
    COMMAND test_ext2 "${BROOK_ROOT}/brook_ext2_disk.img"
)
