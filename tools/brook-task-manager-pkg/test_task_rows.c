#include "task_rows.h"
#include <assert.h>

int main(void)
{
    BrookTaskManagerRow previous[] = {
        {.pid = 12, .incarnation = 301, .cpu_ms = 160, .flags = BROOK_TASK_MANAGER_CAN_END},
        {.pid = 1, .incarnation = 5},
        {.pid = 40, .incarnation = 40, .cpu_ms = 300, .flags = BROOK_TASK_MANAGER_CAN_END},
    };
    qsort(previous, 3, sizeof(*previous), task_row_compare);

    BrookTaskManagerRow next[] = {
        {.pid = 40, .incarnation = 40, .cpu_ms = 250, .flags = BROOK_TASK_MANAGER_CAN_END},
        {.pid = 1, .incarnation = 5},
        {.pid = 12, .incarnation = 301, .cpu_ms = 410, .flags = BROOK_TASK_MANAGER_CAN_END},
    };
    qsort(next, 3, sizeof(*next), task_row_compare);

    const TaskIdentity selected = {12, 301};
    const BrookTaskManagerRow *old = task_row_find(previous, 3, selected);
    const BrookTaskManagerRow *row = task_row_find(next, 3, selected);
    assert(old && row);
    assert(task_cpu_percent(row, old, 2100, 2000) == 250.0);
    assert(task_cpu_percent(row, row, 2100, 2000) == 0.0);
    assert(task_cpu_percent(row, old, 1900, 2000) < 0);
    assert(task_cpu_percent(row, old, 2000, 2000) < 0);
    assert(task_cpu_percent(row, NULL, 2100, 2000) < 0);
    assert(task_cpu_percent(&next[2], &previous[2], 2100, 2000) < 0);
    assert(task_can_end(next, 3, selected));
    assert(!task_can_end(next, 3, (TaskIdentity){1, 5}));
    assert(!task_can_end(NULL, 0, selected));

    next[1].flags = 0;
    assert(!task_can_end(next, 3, selected)); // permissions changed while confirmation was open
    next[1].incarnation = 302;
    next[1].flags = BROOK_TASK_MANAGER_CAN_END;
    assert(!task_row_find(next, 3, selected)); // selected PID was reused
    assert(!task_can_end(next, 3, selected));
    assert(task_can_end(next, 3, task_identity(&next[1])));
    assert(task_cpu_percent(&next[1], old, 2100, 2000) < 0);
    return 0;
}
