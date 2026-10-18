#pragma once
#include <stdint.h>

namespace brook {
enum EpollDiagCounter : unsigned {
    EpCalls, EpZeroTimeout, EpIterations, EpInitialReady, EpLaterReady,
    EpTimeouts, EpInterrupts, EpBlocks, EpScanRead, EpScanWrite, EpScanHup,
    EpScanError, EpScanEdge, EpScanOneShot, EpLastFd, EpLastType,
    EpLastRequested, EpLastReturned, EpDiagCount
};
struct EpollDiagSnapshot {
    uint64_t incarnation;
    uint64_t counters[EpDiagCount];
};
void EpollDiagEnable(bool enabled);
bool EpollDiagEnabled();
// Rolling atomic counters. Reject a snapshot spanning numeric PID reuse.
bool EpollDiagRead(unsigned pid, EpollDiagSnapshot* output);
} // namespace brook
