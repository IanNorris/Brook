#pragma once

#include <stdint.h>

enum {
    BROOK_SYS_TASK_MANAGER = 0xB26,
    BROOK_TASK_MANAGER_VERSION = 1,
    BROOK_TASK_MANAGER_SNAPSHOT = 0,
    BROOK_TASK_MANAGER_END_TASK = 1,
    BROOK_TASK_MANAGER_MAX_ROWS = 2048,
    BROOK_TASK_MANAGER_NAME_SIZE = 32,
};

enum {
    BROOK_TASK_MANAGER_CAN_END = 1u << 0,
    BROOK_TASK_MANAGER_RESIDENT_KNOWN = 1u << 1,
    BROOK_TASK_MANAGER_GPU_MEMORY_KNOWN = 1u << 2,
    BROOK_TASK_MANAGER_SYSTEM = 1u << 3,
};

// syscall(SNAPSHOT, version, header pointer, row pointer, row capacity)
// Returns 0 or -errno. Each row is one thread group in the caller's PID view.
typedef struct BrookTaskManagerSnapshot {
    uint32_t version;
    uint32_t rows;
    uint32_t total;
    uint32_t online_cpus;
    uint64_t timestamp_ms;
} BrookTaskManagerSnapshot;

typedef struct BrookTaskManagerRow {
    uint64_t incarnation;
    uint64_t cpu_ms;
    uint64_t resident_bytes;
    uint64_t gpu_memory_bytes;
    uint32_t pid;
    uint32_t uid;
    uint32_t threads;
    uint32_t flags;
    char name[BROOK_TASK_MANAGER_NAME_SIZE];
} BrookTaskManagerRow;

// syscall(END_TASK, version, visible pid, thread group incarnation)
// Terminates exactly the selected task, if the caller may end it.
