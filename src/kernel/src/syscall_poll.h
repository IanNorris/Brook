#pragma once

// syscall_poll.h -- internal declarations shared from syscall_poll.cpp.
// Internal to the syscall layer; split from syscall.cpp
// (see docs/agent/syscall-split-plan.md).

#include <stdint.h>

namespace brook {

uint64_t WaitWakeupTick(uint64_t deadlineNs, uint64_t safetyMs);

} // namespace brook
