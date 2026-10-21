#pragma once
#include <stdint.h>

namespace brook {

// Policy for a caught handler with SA_RESTART. Arguments are the interrupted
// syscall's RSI and R10, retained in its own frame, not per-CPU scratch state.
inline bool SignalSyscallMayRestart(uint64_t number, uint64_t argument1,
                                   uint64_t argument3)
{
    switch (number) {
    case 0:   // read
    case 1:   // write
    case 2:   // open
    case 3:   // close (existing policy)
    case 16:  // ioctl
    case 17:  // pread64
    case 18:  // pwrite64
    case 19:  // readv
    case 20:  // writev
    case 61:  // wait4
        return true;
    case 202: {
        // FUTEX_PRIVATE_FLAG and FUTEX_CLOCK_REALTIME do not change the
        // operation. Only an untimed WAIT/WAIT_BITSET may restart after a
        // caught handler; replaying a relative timeout would extend it.
        const uint32_t operation = static_cast<uint32_t>(argument1) & ~(128u | 256u);
        return (operation == 0 || operation == 9) && argument3 == 0;
    }
    default:
        return false;
    }
}

} // namespace brook
