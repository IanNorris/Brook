cmake_minimum_required(VERSION 3.25)
project(brook-host-tests CXX)
set(CMAKE_ASM_COMPILER "${CMAKE_CXX_COMPILER}")
enable_language(ASM)

if(NOT DEFINED BROOK_ROOT)
    set(BROOK_ROOT "${CMAKE_SOURCE_DIR}/../../..")
endif()

include("${BROOK_ROOT}/cmake/EevdfExperiment.cmake")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# These executables use assert for both checks and fixture operations. Release
# builds must execute them too, rather than removing checks or failing -Werror
# because variables used only by assertions become unused.
add_compile_options(-UNDEBUG)

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
add_host_test(test_signal_fp_state)
target_compile_options(test_signal_fp_state PRIVATE -UNDEBUG)
add_host_test(test_process_socket_metadata)
target_compile_options(test_process_socket_metadata PRIVATE -UNDEBUG)
set_tests_properties(test_process_socket_metadata PROPERTIES TIMEOUT 10)
add_host_test(test_socket_identity)
target_compile_options(test_socket_identity PRIVATE -UNDEBUG)
add_host_test(test_scheduler_fd_snapshot)
target_compile_options(test_scheduler_fd_snapshot PRIVATE -UNDEBUG)
set_tests_properties(test_scheduler_fd_snapshot PROPERTIES TIMEOUT 10)
add_host_test(test_proc_inode)
target_compile_options(test_proc_inode PRIVATE -UNDEBUG)
add_host_test(test_memfd_seals)
target_compile_options(test_memfd_seals PRIVATE -UNDEBUG)
add_host_test(test_shared_irq_registry)
add_host_test(test_ioapic_route)
add_host_test(test_gpu_completion_wait)
add_host_test(test_gpu_completion_event)
add_host_test(test_gpu_control_submit)
add_host_test(test_gpu_compose_packet)
target_compile_options(test_gpu_completion_wait PRIVATE -UNDEBUG)
add_host_test(test_eevdf)
add_host_test(test_mapping_access_timing "${BROOK_ROOT}/src/kernel/src/memory/mapping_access_timing.cpp")
add_host_test(test_audio_service_timing)
add_host_test(test_audio_delivery_timing)
add_host_test(test_audio_mix_contributions)
add_host_test(test_sysv_sem_ops)
add_host_test(test_sysv_sem_service)
set_tests_properties(test_sysv_sem_service PROPERTIES TIMEOUT 20)
add_host_test(test_dsp_write_plan)
add_host_test(test_pipe_write)
add_host_test(test_pipe_poll)
set_tests_properties(test_pipe_poll PROPERTIES TIMEOUT 20)
add_host_test(test_sched_stall)
add_host_test(test_frame_timing)
add_host_test(test_compositor_frame_wait)
add_host_test(test_frame_timing_runtime)
add_host_test(test_timer_wakeup_timing)
add_host_test(test_wake_scan_handoff)
set_tests_properties(test_wake_scan_handoff PROPERTIES TIMEOUT 20)
target_compile_options(test_mapping_access_timing PRIVATE -DBROOK_HOST_TEST "-iquote${BROOK_ROOT}/src/kernel/src")
set_tests_properties(test_eevdf PROPERTIES TIMEOUT 20)
add_host_test(test_runtime_accounting)
add_host_test(test_runtime_debt)
add_host_test(test_sched_lock_trace)
add_host_test(test_sched_lock_owner)
add_host_test(test_signal_fd)
set_tests_properties(test_signal_fd PROPERTIES TIMEOUT 20)
add_host_test(test_signal_wait)
set_tests_properties(test_sched_lock_owner PROPERTIES TIMEOUT 20)
add_host_test(test_vmalloc_arena)
set_tests_properties(test_vmalloc_arena PROPERTIES TIMEOUT 20)
set_tests_properties(test_sched_lock_trace PROPERTIES TIMEOUT 20)
set_tests_properties(test_runtime_accounting PROPERTIES TIMEOUT 15)
add_host_test(test_smp_boot_policy)
add_host_test(test_gpu_retirement)
add_host_test(test_gpu_upload_backing)
add_host_test(test_gpu_capture)
add_host_test(test_gpu_resource_ids)
target_compile_options(test_gpu_resource_ids PRIVATE -UNDEBUG)
set_tests_properties(test_gpu_resource_ids PROPERTIES TIMEOUT 15)
add_host_test(test_drm_map_tokens)
set_tests_properties(test_drm_map_tokens PROPERTIES TIMEOUT 15)
add_host_test(test_window_surfaces)
target_compile_options(test_window_surfaces PRIVATE
    -Wno-unused-parameter -Wno-attributes -Wno-misleading-indentation -Wno-error=extra
    -DBROOK_HOST_TEST -ffunction-sections -fdata-sections
    "-iquote${BROOK_ROOT}/src/kernel/src"
    "-iquote${BROOK_ROOT}/src/shared"
    "-iquote${BROOK_ROOT}/src/shared/inc_km"
)
target_link_options(test_window_surfaces PRIVATE -Wl,--gc-sections)
add_host_test(test_gpu_capture_queue)
target_compile_options(test_gpu_capture_queue PRIVATE
    -Wno-unused-parameter -Wno-attributes -Wno-misleading-indentation -Wno-error=extra
    -DBROOK_HOST_TEST -ffunction-sections -fdata-sections
    "-iquote${BROOK_ROOT}/src/kernel/src"
    "-iquote${BROOK_ROOT}/src/shared"
    "-iquote${BROOK_ROOT}/src/shared/inc_km"
)
target_link_options(test_gpu_capture_queue PRIVATE -Wl,--gc-sections)
set_tests_properties(test_gpu_capture_queue PROPERTIES TIMEOUT 20)
add_host_test(test_ready_latency)
target_compile_options(test_ready_latency PRIVATE "-iquote${BROOK_ROOT}/src/kernel/src")
set_tests_properties(test_ready_latency PROPERTIES TIMEOUT 15)
add_host_test(test_mapping_ranges "${BROOK_ROOT}/src/kernel/src/memory/mapping_ranges.cpp")
target_compile_options(test_mapping_ranges PRIVATE
    "-iquote${BROOK_ROOT}/src/kernel/src"
    "-iquote${BROOK_ROOT}/src/shared"
    "-iquote${BROOK_ROOT}/src/shared/inc_km"
)
set_tests_properties(test_mapping_ranges PROPERTIES TIMEOUT 15)
add_host_test(test_heap_index "${BROOK_ROOT}/src/kernel/src/memory/heap.cpp")
target_compile_options(test_heap_index PRIVATE
    -DBROOK_HOST_TEST -DBROOK_HEAP_HOST_REGION=0x300000000000ULL
    "-iquote${BROOK_ROOT}/src/kernel/src"
    "-iquote${BROOK_ROOT}/src/shared"
    "-iquote${BROOK_ROOT}/src/shared/inc_km"
)
set_tests_properties(test_heap_index PROPERTIES TIMEOUT 15)
add_host_test(test_object_cache)
set_tests_properties(test_object_cache PROPERTIES TIMEOUT 15)
add_host_test(test_module_sections)
target_compile_options(test_module_sections PRIVATE
    -Wno-unused-parameter -DBROOK_HOST_TEST -ffunction-sections -fdata-sections
    "-iquote${BROOK_ROOT}/src/kernel/src"
    "-iquote${BROOK_ROOT}/src/shared"
    "-iquote${BROOK_ROOT}/src/shared/inc_km"
)
target_link_options(test_module_sections PRIVATE -Wl,--gc-sections)
set_tests_properties(test_module_sections PROPERTIES TIMEOUT 15)
add_host_test(test_virtio_net_tx)
target_compile_options(test_virtio_net_tx PRIVATE
    -Wno-unused-parameter -DBROOK_HOST_TEST -ffunction-sections -fdata-sections
    "-iquote${BROOK_ROOT}/src/kernel/src"
    "-iquote${BROOK_ROOT}/src/shared"
    "-iquote${BROOK_ROOT}/src/shared/inc_km"
)
target_link_options(test_virtio_net_tx PRIVATE -Wl,--gc-sections)
set_tests_properties(test_virtio_net_tx PROPERTIES TIMEOUT 15)
add_host_test(test_kvmclock_stable)
target_compile_options(test_kvmclock_stable PRIVATE
    -DBROOK_HOST_TEST -ffunction-sections -fdata-sections
    "-iquote${BROOK_ROOT}/src/kernel/src"
    "-iquote${BROOK_ROOT}/src/shared"
    "-iquote${BROOK_ROOT}/src/shared/inc_km"
)
target_link_options(test_kvmclock_stable PRIVATE -Wl,--gc-sections)
set_tests_properties(test_kvmclock_stable PROPERTIES TIMEOUT 15)
add_host_test(test_posix_timer_clock)
target_compile_options(test_posix_timer_clock PRIVATE
    -DBROOK_HOST_TEST -ffunction-sections -fdata-sections
    "-iquote${BROOK_ROOT}/src/kernel/src"
    "-iquote${BROOK_ROOT}/src/shared"
    "-iquote${BROOK_ROOT}/src/shared/inc_km"
)
target_link_options(test_posix_timer_clock PRIVATE -Wl,--gc-sections)
set_tests_properties(test_posix_timer_clock PROPERTIES TIMEOUT 15)
add_host_test(test_audio_delivery "${BROOK_ROOT}/src/kernel/src/audio.cpp"
    "${BROOK_ROOT}/src/tests/test_audio_delivery/hda_clock.cpp")
target_compile_options(test_audio_delivery PRIVATE
    -DBROOK_HOST_TEST -ffunction-sections -fdata-sections
    "-iquote${BROOK_ROOT}/src/kernel/src"
    "-iquote${BROOK_ROOT}/src/shared"
    "-iquote${BROOK_ROOT}/src/shared/inc_km"
)
target_link_options(test_audio_delivery PRIVATE -Wl,--gc-sections)
set_tests_properties(test_audio_delivery PROPERTIES TIMEOUT 15)
add_host_test(test_user_copy
    "${BROOK_ROOT}/src/kernel/src/user_copy_fault.cpp"
    "${BROOK_ROOT}/src/kernel/src/user_copy.S"
)
target_compile_options(test_user_copy PRIVATE "-iquote${BROOK_ROOT}/src/kernel/src")
set_tests_properties(test_user_copy PROPERTIES TIMEOUT 15)
add_host_test(test_virgl_submit)
add_host_test(test_busybox_lookup)
add_host_test(test_launcher_command)

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

# Real-code cooperative cancellation/lifetime harness for KMutex and KRwLock.
# Scheduler transitions are modeled; queueing, grants and references are actual
# kernel code. BROOK_HOST_TEST neutralises privileged guard instructions.
add_host_test(test_krwlock_cleanup
    "${BROOK_ROOT}/src/kernel/src/sync/kmutex.cpp"
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
add_host_test(test_record_locks
    "${BROOK_ROOT}/src/kernel/src/record_locks.cpp"
    "${BROOK_ROOT}/src/kernel/src/file_locks.cpp"
)
target_compile_options(test_record_locks PRIVATE
    -DBROOK_HOST_TEST
    "-iquote${BROOK_ROOT}/src/kernel/src"
    "-iquote${BROOK_ROOT}/src/shared"
    "-iquote${BROOK_ROOT}/src/shared/inc_km"
)
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

add_library(sched_eevdf SHARED "${BROOK_ROOT}/src/modules/sched_eevdf.cpp")
target_include_directories(sched_eevdf PRIVATE "${BROOK_ROOT}/src/kernel/src")
set_target_properties(sched_eevdf PROPERTIES PREFIX "lib" SUFFIX ".so")
target_compile_definitions(sched_eevdf PRIVATE BROOK_EEVDF_NORMAL_REQUEST_NS=${BROOK_EEVDF_NORMAL_REQUEST_NS})
target_compile_definitions(test_eevdf PRIVATE BROOK_EEVDF_NORMAL_REQUEST_NS=${BROOK_EEVDF_NORMAL_REQUEST_NS})
add_dependencies(test_eevdf sched_eevdf)
target_link_libraries(test_eevdf PRIVATE ${CMAKE_DL_LIBS})
add_test(NAME test_eevdf_module COMMAND test_eevdf "$<TARGET_FILE:sched_eevdf>")
set_tests_properties(test_eevdf_module PROPERTIES TIMEOUT 20)

# test_sched_modules — dlopen-based test that loads the .so modules
add_executable(test_sched_modules
    "${BROOK_ROOT}/src/tests/test_sched_modules/main.cpp"
)
target_compile_options(test_sched_modules PRIVATE
    -Wall -Wextra -Werror -O2
    "-iquote${BROOK_ROOT}/src/kernel/src"
)
target_link_libraries(test_sched_modules PRIVATE ${CMAKE_DL_LIBS})
set_target_properties(test_sched_modules PROPERTIES ENABLE_EXPORTS ON)
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

# Actual page-table walks and ownership transitions with aligned host frames.
# Platform/PMM, heap and filesystem/scheduler finalizers are instrumented.
# Address-space queues and page-table ownership transitions run actual code.
add_host_test(test_vmm_leaf
    "${BROOK_ROOT}/src/kernel/src/memory/virtual_memory.cpp"
    "${BROOK_ROOT}/src/kernel/src/memory/address_space.cpp"
    "${BROOK_ROOT}/src/kernel/src/memory/exec_page_cache.cpp"
    "${BROOK_ROOT}/src/kernel/src/memory/mapping_transaction.cpp"
    "${BROOK_ROOT}/src/kernel/src/memory/file_mapping_registry.cpp"
    "${BROOK_ROOT}/src/kernel/src/memory/file_object.cpp"
    "${BROOK_ROOT}/src/kernel/src/memory/mapping_ranges.cpp"
    "${BROOK_ROOT}/src/kernel/src/memory/user_mapping.cpp"
    "${BROOK_ROOT}/src/kernel/src/memory/mapping_access_timing.cpp"
)
target_compile_options(test_vmm_leaf PRIVATE
    -DBROOK_HOST_TEST -ffunction-sections -fdata-sections
    "-iquote${BROOK_ROOT}/src/kernel/src"
    "-iquote${BROOK_ROOT}/src/shared"
    "-iquote${BROOK_ROOT}/src/shared/inc_km"
)
target_link_options(test_vmm_leaf PRIVATE -Wl,--gc-sections)

# Actual canonical cache adoption under simultaneous independent-open misses.
add_host_test(test_file_page_cache
    "${BROOK_ROOT}/src/kernel/src/memory/file_page_cache.cpp"
)
target_compile_options(test_file_page_cache PRIVATE
    -DBROOK_HOST_TEST
    "-iquote${BROOK_ROOT}/src/kernel/src"
    "-iquote${BROOK_ROOT}/src/shared"
    "-iquote${BROOK_ROOT}/src/shared/inc_km"
)

# Real PMM counters/bitmap, with a bounded descriptor fixture and no real TLBs.
add_host_test(test_pmm_refs
    "${BROOK_ROOT}/src/kernel/src/memory/physical_memory.cpp"
)
target_compile_options(test_pmm_refs PRIVATE
    -DBROOK_HOST_TEST -ffunction-sections -fdata-sections
    "-iquote${BROOK_ROOT}/src/kernel/src"
    "-iquote${BROOK_ROOT}/src/shared"
    "-iquote${BROOK_ROOT}/src/shared/inc_km"
)
target_link_options(test_pmm_refs PRIVATE -Wl,--gc-sections)

add_host_test(test_file_object
    "${BROOK_ROOT}/src/kernel/src/memory/file_object.cpp"
)
set_tests_properties(test_file_object PROPERTIES TIMEOUT 30)
target_compile_options(test_file_object PRIVATE
    -DBROOK_HOST_TEST
    -DBROOK_HOST_USER_COPY_TEST
    "-iquote${BROOK_ROOT}/src/kernel/src"
    "-iquote${BROOK_ROOT}/src/shared"
    "-iquote${BROOK_ROOT}/src/shared/inc_km"
)

# Dynamic VFS pin bindings and identity aggregation; actual registry core.
add_host_test(test_mapping_pins
    "${BROOK_ROOT}/src/kernel/src/memory/mapping_pin_registry.cpp"
)
target_compile_options(test_mapping_pins PRIVATE
    -DBROOK_HOST_TEST
    "-iquote${BROOK_ROOT}/src/kernel/src"
    "-iquote${BROOK_ROOT}/src/shared"
    "-iquote${BROOK_ROOT}/src/shared/inc_km"
)

# Same bounded frame walker used by timer and context-switch profiling.
add_host_test(test_profiler_stack)
target_compile_options(test_profiler_stack PRIVATE
    -UNDEBUG
    "-iquote${BROOK_ROOT}/src/kernel/src"
)

# Saved-ID transitions and all-or-nothing unprivileged credential changes.
add_host_test(test_credential_ids)
add_host_test(test_user_namespace)
add_host_test(test_user_credentials)
target_compile_options(test_user_credentials PRIVATE -UNDEBUG -DBROOK_HOST_TEST
    "-iquote${BROOK_ROOT}/src/kernel/src" "-iquote${BROOK_ROOT}/src/shared" "-iquote${BROOK_ROOT}/src/shared/inc_km")
target_link_libraries(test_user_credentials PRIVATE Threads::Threads)

# Actual ELF loader/range metadata; host frames model the user page table.
add_host_test(test_elf_loader
    "${BROOK_ROOT}/src/kernel/src/elf_loader.cpp"
    "${BROOK_ROOT}/src/kernel/src/memory/exec_page_cache.cpp"
    "${BROOK_ROOT}/src/kernel/src/memory/mapping_ranges.cpp"
)
target_compile_options(test_elf_loader PRIVATE
    -DBROOK_HOST_TEST
    "-iquote${BROOK_ROOT}/src/kernel/src"
    "-iquote${BROOK_ROOT}/src/shared"
    "-iquote${BROOK_ROOT}/src/shared/inc_km"
)

add_host_test(test_exec_page_cache
    "${BROOK_ROOT}/src/kernel/src/memory/exec_page_cache.cpp"
)
target_compile_options(test_exec_page_cache PRIVATE
    -DBROOK_HOST_TEST
    "-iquote${BROOK_ROOT}/src/kernel/src"
    "-iquote${BROOK_ROOT}/src/shared"
    "-iquote${BROOK_ROOT}/src/shared/inc_km"
)
set_tests_properties(test_exec_page_cache PROPERTIES TIMEOUT 30)

add_host_test(test_launch_feedback)
target_compile_options(test_launch_feedback PRIVATE "-iquote${BROOK_ROOT}/src/kernel/src")

add_host_test(test_cursor_composition)
target_compile_options(test_cursor_composition PRIVATE "-iquote${BROOK_ROOT}/src/kernel/src")

add_host_test(test_desktop_metrics)
target_compile_options(test_desktop_metrics PRIVATE "-iquote${BROOK_ROOT}/src/kernel/src")

add_host_test(test_debug_job_wire)
target_compile_options(test_debug_job_wire PRIVATE -UNDEBUG "-iquote${BROOK_ROOT}/src/kernel/src")

add_host_test(test_debug_epoch "${BROOK_ROOT}/src/kernel/src/debug_epoch.cpp")
target_compile_options(test_debug_epoch PRIVATE -UNDEBUG -DBROOK_HOST_TEST
    "-iquote${BROOK_ROOT}/src/kernel/src"
    "-iquote${BROOK_ROOT}/src/shared"
    "-iquote${BROOK_ROOT}/src/shared/inc_km")

add_host_test(test_launch_history)
target_compile_options(test_launch_history PRIVATE "-iquote${BROOK_ROOT}/src/kernel/src")

# Shared component walker: symlink/dotdot ordering, search and typed failures.
# Production FatFs object adapter on a disposable FAT16 image.
if(NOT DEFINED CMAKE_C_COMPILER AND DEFINED ENV{HOST_CC})
    set(CMAKE_C_COMPILER "$ENV{HOST_CC}" CACHE FILEPATH "Wrapped host C compiler")
endif()
enable_language(C)
include("${BROOK_ROOT}/cmake/PrepareFatFs.cmake")
set(FATFS_OBJECT_HOST_DIR "${CMAKE_CURRENT_BINARY_DIR}/fatfs-object-adapter")
brook_prepare_fatfs("${FATFS_OBJECT_HOST_DIR}")
add_executable(test_fat_object_adapter
    "${BROOK_ROOT}/src/tests/test_fat_object_adapter/main.cpp"
    "${FATFS_OBJECT_HOST_DIR}/ff.c" "${FATFS_OBJECT_HOST_DIR}/ffunicode.c")
target_include_directories(test_fat_object_adapter PRIVATE "${FATFS_OBJECT_HOST_DIR}")
target_compile_options(test_fat_object_adapter PRIVATE -UNDEBUG)
target_compile_options(test_fat_object_adapter PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror>")
target_compile_options(test_fat_object_adapter PRIVATE "-iquote${BROOK_ROOT}/src/kernel/src")
add_test(NAME test_fat_object_adapter COMMAND python3 "${BROOK_ROOT}/src/tests/test_fat_object_adapter/run.py" "$<TARGET_FILE:test_fat_object_adapter>")
set_tests_properties(test_fat_object_adapter PROPERTIES TIMEOUT 60)
add_executable(test_fat_objects
    "${BROOK_ROOT}/src/tests/test_fat_objects/main.cpp"
    "${FATFS_OBJECT_HOST_DIR}/ff.c" "${FATFS_OBJECT_HOST_DIR}/ffunicode.c")
target_include_directories(test_fat_objects PRIVATE "${FATFS_OBJECT_HOST_DIR}")
target_compile_options(test_fat_objects PRIVATE -UNDEBUG -DBROOK_HOST_TEST -ffunction-sections -fdata-sections)
target_compile_options(test_fat_objects PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror;-Wno-unused-function;-Wno-unused-parameter;-Wno-attributes>")
target_compile_options(test_fat_objects PRIVATE "-iquote${BROOK_ROOT}/src/kernel/src"
    "-iquote${BROOK_ROOT}/src/shared" "-iquote${BROOK_ROOT}/src/shared/inc_km")
target_link_options(test_fat_objects PRIVATE -Wl,--gc-sections)
target_link_libraries(test_fat_objects PRIVATE Threads::Threads)
add_test(NAME test_fat_objects COMMAND python3 "${BROOK_ROOT}/src/tests/test_fat_objects/run.py" "$<TARGET_FILE:test_fat_objects>")
set_tests_properties(test_fat_objects PROPERTIES TIMEOUT 60)
add_executable(test_ext2_objects "${BROOK_ROOT}/src/tests/test_ext2_objects/main.cpp"
    "${BROOK_ROOT}/src/kernel/src/fs_context.cpp"
    "${BROOK_ROOT}/src/kernel/src/vfs.cpp" "${BROOK_ROOT}/src/kernel/src/memory/mapping_pin_registry.cpp")
target_link_libraries(test_ext2_objects PRIVATE Threads::Threads)
add_test(NAME test_ext2_objects COMMAND python3 "${BROOK_ROOT}/src/tests/test_ext2_objects/run.py" "$<TARGET_FILE:test_ext2_objects>")
target_compile_options(test_ext2_objects PRIVATE -Wall -Wextra -Werror -Wno-unused-function -UNDEBUG -DBROOK_HOST_TEST
    -ffunction-sections -fdata-sections -Wno-unused-parameter -Wno-attributes
    "-iquote${BROOK_ROOT}/src/kernel/src" "-iquote${BROOK_ROOT}/src/shared"
    "-iquote${BROOK_ROOT}/src/shared/inc_km")
target_link_options(test_ext2_objects PRIVATE -Wl,--gc-sections)
set_tests_properties(test_ext2_objects PROPERTIES TIMEOUT 60)
add_host_test(test_mount_namespace_state)
target_compile_options(test_mount_namespace_state PRIVATE -UNDEBUG "-iquote${BROOK_ROOT}/src/kernel/src")
set_tests_properties(test_mount_namespace_state PROPERTIES TIMEOUT 20)
add_host_test(test_vfs_object_refs)
target_compile_options(test_vfs_object_refs PRIVATE -UNDEBUG "-iquote${BROOK_ROOT}/src/kernel/src")
add_host_test(test_retained_path_walk)
target_compile_options(test_retained_path_walk PRIVATE -UNDEBUG "-iquote${BROOK_ROOT}/src/kernel/src")
add_host_test(test_path_walk)
target_compile_options(test_path_walk PRIVATE -UNDEBUG "-iquote${BROOK_ROOT}/src/kernel/src")

add_host_test(test_lifecycle_channel)
target_compile_options(test_lifecycle_channel PRIVATE -UNDEBUG "-iquote${BROOK_ROOT}/src/kernel/src")
set_tests_properties(test_lifecycle_channel PROPERTIES TIMEOUT 15)

add_host_test(test_process_destroy)
add_host_test(test_process_ids)
target_compile_options(test_process_ids PRIVATE -UNDEBUG "-iquote${BROOK_ROOT}/src/kernel/src")
set_tests_properties(test_process_ids PROPERTIES TIMEOUT 15)

add_host_test(test_write_admission "${BROOK_ROOT}/src/kernel/src/device.cpp")
target_compile_options(test_write_admission PRIVATE -UNDEBUG -DBROOK_HOST_TEST -ffunction-sections -fdata-sections
    "-iquote${BROOK_ROOT}/src/kernel/src" "-iquote${BROOK_ROOT}/src/shared" "-iquote${BROOK_ROOT}/src/shared/inc_km")
target_link_options(test_write_admission PRIVATE -Wl,--gc-sections)
set_tests_properties(test_write_admission PROPERTIES TIMEOUT 15)
add_host_test(test_shutdown)
target_compile_options(test_shutdown PRIVATE -UNDEBUG -DBROOK_HOST_TEST -ffunction-sections -fdata-sections
    "-iquote${BROOK_ROOT}/src/kernel/src" "-iquote${BROOK_ROOT}/src/shared" "-iquote${BROOK_ROOT}/src/shared/inc_km")
target_link_options(test_shutdown PRIVATE -Wl,--gc-sections)
set_tests_properties(test_shutdown PROPERTIES TIMEOUT 15)

add_host_test(test_gpu_scene)
target_sources(test_gpu_scene PRIVATE ../test_gpu_scene/versions.cpp)
target_sources(test_gpu_scene PRIVATE ../test_gpu_scene/service.cpp)
target_compile_options(test_gpu_scene PRIVATE -DBROOK_HOST_TEST -Wno-unused-parameter -Wno-attributes
    "-iquote${BROOK_ROOT}/src/kernel/src" "-iquote${BROOK_ROOT}/src/shared" "-iquote${BROOK_ROOT}/src/shared/inc_km")

add_host_test(test_gpu_context)

add_host_test(test_cpu_memory_type)

add_host_test(test_gpu_aperture)

add_host_test(test_dmabuf_layout)
target_compile_options(test_dmabuf_layout PRIVATE -UNDEBUG)

add_host_test(test_gpu_reservation)
target_compile_options(test_gpu_reservation PRIVATE -UNDEBUG)

# Production VFS mount topology, walk and object-to-vnode ownership transfer.
add_host_test(test_vfs_paths "${BROOK_ROOT}/src/kernel/src/memory/mapping_pin_registry.cpp"
    "${BROOK_ROOT}/src/kernel/src/fs_context.cpp" "${BROOK_ROOT}/src/kernel/src/process_fs.cpp")
target_compile_options(test_vfs_paths PRIVATE -UNDEBUG -DBROOK_HOST_TEST -ffunction-sections -fdata-sections
    -Wno-unused-function -Wno-unused-parameter -Wno-attributes
    "-iquote${BROOK_ROOT}/src/kernel/src" "-iquote${BROOK_ROOT}/src/shared" "-iquote${BROOK_ROOT}/src/shared/inc_km")
target_link_options(test_vfs_paths PRIVATE -Wl,--gc-sections)
set_tests_properties(test_vfs_paths PROPERTIES TIMEOUT 20)

add_executable(test_tmpfs "${BROOK_ROOT}/src/tests/test_tmpfs/main.cpp"
    "${BROOK_ROOT}/src/kernel/src/memory/file_object.cpp")
target_link_libraries(test_tmpfs PRIVATE Threads::Threads)
target_compile_options(test_tmpfs PRIVATE -Wall -Wextra -Werror -Wno-unused-function -UNDEBUG -DBROOK_HOST_TEST
    -ffunction-sections -fdata-sections -Wno-unused-parameter -Wno-attributes
    "-iquote${BROOK_ROOT}/src/kernel/src" "-iquote${BROOK_ROOT}/src/shared"
    "-iquote${BROOK_ROOT}/src/shared/inc_km")
target_link_options(test_tmpfs PRIVATE -Wl,--gc-sections)
add_test(NAME test_tmpfs COMMAND test_tmpfs)
set_tests_properties(test_tmpfs PROPERTIES TIMEOUT 60)

add_host_test(test_gpu_shared_contexts)
add_host_test(test_drm_resource_layout)
target_compile_options(test_drm_resource_layout PRIVATE -UNDEBUG)
