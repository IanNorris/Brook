#pragma once

// syscall_fd.h -- internal declarations shared from syscall_fd.cpp.
// Internal to the syscall layer; split from syscall.cpp.

#include "file_locks.h"

namespace brook {

void FdBumpRefcount(const FdEntry* fde);
// Retain the handle and the shared open file description for a new alias
// (dup, fork, SCM_RIGHTS). False only when a deep-copied handle cannot be
// allocated; nothing is retained then.
bool FdRetainCopy(FdEntry* copy);

// Linux file flags (used in fcntl F_GETFL/F_SETFL and dup3)
[[maybe_unused]] static constexpr int O_NONBLOCK  = 0x800;

[[maybe_unused]] static constexpr int O_CLOEXEC   = 0x80000;

} // namespace brook
