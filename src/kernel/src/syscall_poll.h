#pragma once

// syscall_poll.h -- internal declarations shared from syscall_poll.cpp.
// Internal to the syscall layer; split from syscall.cpp
// (see docs/agent/syscall-split-plan.md).

#include <stdint.h>
#include "sync_file.h"

namespace brook {

// Wakeup for poll/epoll on Intel sync_file fences (H-26). Every fence
// completion signals the GPU Retire event, so one registration covers every
// pending fence in the set. Construct before the final readiness check (it
// snapshots the event generation), call Want for each pending fence, then
// Arm: true means a fence retired after the snapshot, so recheck instead of
// sleeping. With no slot (prompt wakes off, or the queue full) the wait
// falls back to a one-tick safety timeout instead of the 50/100 ms default.
struct SyncFileWait {
    uint64_t seen = BdwGpuEventGeneration(BdwGpuEvent::Retire);
    BdwGpuWaiter waiter{};
    BdwWaitArm arm = BdwWaitArm::Full;
    bool wanted = false;
    void Want(const SyncFileData& fence) { if (fence.intelFence && !SyncFileState(fence)) wanted = true; }
    bool Arm() {
        if (!wanted) return false;
        arm = BdwGpuEventArm(BdwGpuEvent::Retire, seen, waiter);
        return arm == BdwWaitArm::Stale;
    }
    uint64_t SafetyMs(uint64_t current) const { return wanted && arm != BdwWaitArm::Armed ? 1 : current; }
    SyncFileWait() = default;
    SyncFileWait(const SyncFileWait&) = delete;
    SyncFileWait& operator=(const SyncFileWait&) = delete;
    ~SyncFileWait() { if (arm == BdwWaitArm::Armed) BdwGpuEventDisarm(waiter); }
};

uint64_t WaitWakeupTick(uint64_t deadlineNs, uint64_t safetyMs);

struct pollfd {
    int   fd;
    short events;
    short revents;
};

static constexpr short POLLIN  = 0x0001;
uint64_t WaitDeadlineNs(uint64_t timeoutMs);
int64_t PollWait(pollfd* fds, uint64_t nfds, uint64_t timeout_ms, bool selectMode = false);

} // namespace brook
