#pragma once

#include "process.h"
#include "seccomp_bpf.h"

namespace brook {
// A filter and its ancestor pointer become immutable before publication.
struct SeccompFilter {
    SeccompFilter* previous;
    uint32_t references;
    unsigned count;
    unsigned pathInstructions;
    bool logRequested;
    SeccompInstruction code[];
};

inline SeccompFilter* SeccompLoad(const Process* process)
{
    return __atomic_load_n(&process->seccompFilter, __ATOMIC_ACQUIRE);
}
void SeccompRetain(SeccompFilter* filter);
void SeccompRelease(SeccompFilter* filter);
int64_t SeccompPrctl(uint64_t option, uint64_t mode, uint64_t program);
int64_t SysSeccomp(uint64_t operation, uint64_t flags, uint64_t argument,
                  uint64_t, uint64_t, uint64_t);
int64_t SeccompDenied(Process* process, uint64_t syscall, uint32_t verdict);
uint32_t SeccompEvaluate(const SeccompFilter* filter, const SeccompData& data, bool* audit);
void SeccompAudit(const Process* process, uint64_t syscall, uint32_t verdict);
}
