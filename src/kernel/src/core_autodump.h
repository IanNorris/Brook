#pragma once

#include "core_dump.h"
#include "core_autodump_policy.h"

namespace brook {
struct Process;

// Call under the task registry lock, before publishing group termination.
// The queue owns the address-space pin and copied notes once committed.
struct CoreAutoJob {
    CoreDumpSnapshot snapshot{};
    int signal=0;
    uint64_t started=0;
    uint64_t byteLimit=0;
};
CoreAutoJob* CoreAutoClaim(const Process* target,const Process* leader,int signal);
void CoreAutoCommit(CoreAutoJob*);
void CoreAutoStart();
void CoreAutoSetMode(CoreAutoMode);
struct CoreAutoFailure {
    const char* stage=nullptr;
    uint32_t pid=0,error=0;
    CoreDumpDiscarded discarded{};
};
struct CoreAutoStats {
    CoreAutoMode mode;
    uint64_t queued, completed, failed, timedOut, busy, rateLimited, activeMs;
    bool worker;
    CoreAutoFailure lastError;
};
CoreAutoStats CoreAutoReadStats();

int SchedulerCoreAutoQueue(uint16_t pid);
// RLIMIT_CORE (4) or RLIMIT_NOFILE (7) of the target's thread-group leader.
int SchedulerCoreResourceLimit(Process* caller,uint32_t pid,const CoreResourceLimits* replace,
                               CoreResourceLimits& previous,bool privileged,uint32_t resource = 4);
}
