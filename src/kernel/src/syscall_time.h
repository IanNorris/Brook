#pragma once

// syscall_time.h -- internal declarations shared from syscall_time.cpp.
// Internal to the syscall layer; split from syscall.cpp.

#include <stdint.h>

namespace brook {

uint64_t ClockNowNs(uint64_t clockid);

// LAPIC fires every 1ms — monotonic tick counter.

struct timespec {
    int64_t  tv_sec;
    int64_t  tv_nsec;
};

} // namespace brook
