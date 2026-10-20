#pragma once
#include "sched_ops.h"

namespace brook {

// Called with the mechanism's ready lock held, while rebuilding empty policy
// state. A Ready task may still own a CPU stack during a switch-out; its CPU's
// post-switch drain, rather than migration, must publish it as dispatchable.
inline bool SchedRestoreProcess(const SchedOps* ops, void* state, uint16_t pid,
                                uint8_t priority, bool terminated, bool ready,
                                int32_t runningOnCpu, bool runningState = false) {
    if (!pid || pid >= SCHED_MAX_PIDS || terminated) return false;
    ops->InitProcess(state, pid, priority);
    if (runningOnCpu != -1 && (ready || runningState)) ops->SetRunnable(state, pid, true);
    if (!ready || runningOnCpu != -1) return false;
    ops->Enqueue(state, pid);
    return true;
}

} // namespace brook
