#pragma once

// syscall_poll.h -- internal declarations shared from syscall_poll.cpp.
// Internal to the syscall layer; split from syscall.cpp
// (see docs/agent/syscall-split-plan.md).

#include <stdint.h>

namespace brook {

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
