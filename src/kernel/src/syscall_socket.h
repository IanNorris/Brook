#pragma once

// syscall_socket.h -- internal declarations shared from syscall_socket.cpp.
// Internal to the syscall layer; split from syscall.cpp
// (see docs/agent/syscall-split-plan.md).

#include "syscall_unix_socket.h"

namespace brook {

void UnixFdSnapRelease(const UnixFdSnap* snap);

} // namespace brook
