#pragma once

// syscall_signal.h -- internal declarations shared from syscall_signal.cpp.
// Internal to the syscall layer; split from syscall.cpp
// (see docs/agent/syscall-split-plan.md).

#include <stdint.h>

namespace brook {

int InstallWaitSignalMask(uint64_t address, uint64_t size);

} // namespace brook
