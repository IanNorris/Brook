#pragma once
#include <stdint.h>
namespace brook {
constexpr uint64_t OomUserReservePages=1024;
inline bool OomUserAllocationFits(uint64_t freePages,uint64_t requestedPages) {
    return freePages>OomUserReservePages && requestedPages<=freePages-OomUserReservePages;
}
struct OomRecoveryStatus {
    bool enabled=false;
    uint32_t phase=0;
    uint64_t failures=0,lastVictimIncarnation=0,lastFreeBeforeBytes=0,lastFreeAfterBytes=0;
    uint64_t exclusivePagesEstimate=0,lastEventSequence=0;
    uint32_t lastVictimPid=0;
};
OomRecoveryStatus OomRecoverySnapshot();
void OomRecoveryStartWorker();
bool OomRecoveryAdmitApplication(const char* executable,uint32_t uid);
bool OomRecoveryEnabled();
// Nonblocking atomic diagnostics only; safe after dropping allocator locks.
void OomRecoveryAllocationFailure(uint64_t requestedPages,uint64_t freePages,bool contiguous);
}
