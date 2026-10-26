#pragma once
#include <stdint.h>

namespace brook {

// Internal return code, never an exposed errno. PI mutex waits use Linux's
// unconditional restart class, including absolute timed waits. Normalize it
// before selecting a signal, so ignored/no-longer-pending signals restart too.
static constexpr int64_t SignalRestartNoIntr = -513;
inline bool SignalPrepareUnconditionalRestart(uint64_t number, uint64_t operation,
    int64_t result, uint64_t* rip, int64_t* replacement)
{
    const auto op=static_cast<uint32_t>(operation)&~(128u|256u);
    if(result!=SignalRestartNoIntr || number!=202 || (op!=6 && op!=13) || *rip<2)return false;
    *rip-=2;
    *replacement=static_cast<int64_t>(number);
    return true;
}

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
