#pragma once

// syscall_rw.h -- internal declarations shared from syscall_rw.cpp.
// Internal to the syscall layer; split from syscall.cpp
// (see docs/agent/syscall-split-plan.md).

#include "pipe.h"
#include "user_copy.h"

namespace brook {

// Backend/VFS byte results are signed int. Bound ordinary writes rather than
// narrowing a 64-bit request (which can wrap to zero or a negative result).
static constexpr uint64_t VnodeWriteLimit = 0x7ffff000ULL;

} // namespace brook
