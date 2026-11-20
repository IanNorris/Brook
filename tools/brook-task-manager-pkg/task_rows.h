#pragma once

#include "brook_task_manager.h"
#include <stdbool.h>
#include <stdlib.h>

typedef struct TaskIdentity {
    uint32_t pid;
    uint64_t incarnation;
} TaskIdentity;

static inline TaskIdentity task_identity(const BrookTaskManagerRow *row)
{
    return (TaskIdentity){row->pid, row->incarnation};
}

static inline int task_identity_compare(TaskIdentity a, TaskIdentity b)
{
    if (a.pid != b.pid)
        return a.pid < b.pid ? -1 : 1;
    return (a.incarnation > b.incarnation) - (a.incarnation < b.incarnation);
}

static inline int task_row_compare(const void *a, const void *b)
{
    return task_identity_compare(task_identity(a), task_identity(b));
}

static inline const BrookTaskManagerRow *task_row_find(const BrookTaskManagerRow *rows,
                                                       size_t count, TaskIdentity identity)
{
    if (!identity.pid || !identity.incarnation || !count)
        return NULL;
    const BrookTaskManagerRow key = {.pid = identity.pid, .incarnation = identity.incarnation};
    return bsearch(&key, rows, count, sizeof(*rows), task_row_compare);
}

// A full core is 100.0%. A thread group can use several cores simultaneously.
// A newly observed or reset counter has no preceding interval.
static inline double task_cpu_percent(const BrookTaskManagerRow *current,
                                      const BrookTaskManagerRow *previous,
                                      uint64_t now_ms, uint64_t previous_ms)
{
    if (!previous || task_identity_compare(task_identity(current), task_identity(previous)) ||
        now_ms <= previous_ms || current->cpu_ms < previous->cpu_ms)
        return -1;
    return (double)(current->cpu_ms - previous->cpu_ms) * 100 / (double)(now_ms - previous_ms);
}

// Validate against the most recent successful sample before sending a request.
// The kernel also checks this exact identity and the current permissions.
static inline bool task_can_end(const BrookTaskManagerRow *rows, size_t count, TaskIdentity identity)
{
    const BrookTaskManagerRow *row = task_row_find(rows, count, identity);
    return row && (row->flags & BROOK_TASK_MANAGER_CAN_END);
}
