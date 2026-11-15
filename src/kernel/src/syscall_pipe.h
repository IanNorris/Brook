#pragma once

// syscall_pipe.h -- internal declarations shared from syscall_pipe.cpp.
// Internal to the syscall layer; split from syscall.cpp
// (see docs/agent/syscall-split-plan.md).

#include "mount_namespace.h"

#include "pipe.h"

namespace brook {

#include "pipe_poll_types.inc"
#include "named_fifo_types.inc"

} // namespace brook
