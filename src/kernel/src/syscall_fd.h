#pragma once

// syscall_fd.h -- internal declarations shared from syscall_fd.cpp.
// Internal to the syscall layer; split from syscall.cpp
// (see docs/agent/syscall-split-plan.md).

#include "file_locks.h"

namespace brook {

void FdBumpRefcount(const FdEntry* fde);

// Linux file flags (used in fcntl F_GETFL/F_SETFL and dup3)
[[maybe_unused]] static constexpr int O_NONBLOCK  = 0x800;

[[maybe_unused]] static constexpr int O_CLOEXEC   = 0x80000;

} // namespace brook
