#pragma once
#include <stdint.h>
#include "intel_bdw_preparation.h"
namespace brook {
struct BdwGpuFile;
struct BdwExecSnapshot;
struct BdwGpuSync;
struct BdwFence;
// Serialize external sync-file replacement with exec fence publication.
bool BdwIntelSyncReplaceAdmitted(BdwGpuSync* sync,BdwFence* fence);
bool BdwIntelSyncPointAdmitted(BdwGpuSync* sync,uint64_t point,BdwFence* fence);
// Copy callbacks operate on userspace addresses and must fail cleanly on
// faults. The file is retained by the caller throughout the ioctl.
struct BdwI915UserAccess {
    void* context;
    bool (*read)(void*,void*,uint64_t,uint64_t);
    bool (*writable)(void*,uint64_t,uint64_t);
    bool (*write)(void*,uint64_t,const void*,uint64_t);
    // Monotonic nanoseconds and an interruptible bounded sleep; sleep returns
    // false when interrupted. The caller pins the file for the entire wait.
    uint64_t (*nowNs)(void*)=nullptr;
    bool (*sleepNs)(void*,uint64_t)=nullptr;
    // Optional submission owner. On zero it owns the snapshot and has
    // accounted its buffers before returning. On error it owns nothing and
    // must leave the snapshot unarmed so the dispatcher can release it.
    // Installing this callback alone does not advertise execution support.
    int64_t (*submit)(void*,BdwExecSnapshot*)=nullptr;
    // Explicitly gated bring-up mode only; never implies complete i915 ABI.
    bool renderExperiment=false;
    // Diagnostic identity sampled at admission; PID may later be reused.
    uint32_t submitterPid=0;
    // Optional kernel-owned handoff router, called after immutable copy and
    // fence preparation, before queue admission. Return true only if claimed;
    // result follows submit's ownership contract. False leaves it untouched.
    bool (*handoffSubmit)(void*,BdwExecSnapshot*,int64_t* result)=nullptr;
    // Leader incarnation, not PID; zero denotes an unknown/untracked owner.
    uint64_t submitterIncarnation=0;
    // Optional paired tracking for the entire exec preparation/publication.
    int64_t (*beginExec)(void*,uint64_t,BdwSubmissionPreparation&)=nullptr;
    void (*endExec)(void*,BdwSubmissionPreparation&)=nullptr;
};
int64_t BdwIntelRenderIoctl(BdwGpuFile* file,uint64_t cmd,uint64_t arg,const BdwI915UserAccess& user);
// Decode a limited soft-pinned EXECBUFFER2 request into a retained snapshot.
// This is not an ioctl entry point or permission to execute commands. The
// caller owns a successful snapshot and must release it using its lifetime API.
int64_t BdwIntelCopyExecSnapshot(BdwGpuFile* file,uint64_t arg,const BdwI915UserAccess& user,BdwExecSnapshot** out);
}
