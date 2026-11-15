#pragma once

// syscall_strace.h -- internal declarations shared from syscall_strace.cpp.
// Internal to the syscall layer; split from syscall.cpp
// (see docs/agent/syscall-split-plan.md).

#include <stdint.h>

namespace brook {

uint32_t SyscallCategory(uint64_t num);

int64_t SyscallDispatchTraced(uint64_t num, uint64_t a0, uint64_t a1,
                               uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);

} // namespace brook
