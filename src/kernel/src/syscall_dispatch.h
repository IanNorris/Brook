#pragma once

// syscall_dispatch.h -- internal declarations shared from syscall.cpp.
// Internal to the syscall layer; split from syscall.cpp.

#include "syscall.h"

namespace brook {

extern SyscallFn g_syscallTable[SYSCALL_MAX];

} // namespace brook
