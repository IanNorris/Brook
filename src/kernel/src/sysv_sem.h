#pragma once
#include <stdint.h>
#include <stddef.h>
#include "sysv_sem_ops.h"
namespace brook {
struct Process;
// Linux x86-64 ipc64_perm / semid64_ds, including reserved time words.
struct SysvIpcPerm {
    int32_t key;
    uint32_t uid, gid, cuid, cgid, mode;
    uint16_t sequence, padding;
    uint32_t padding2;
    uint64_t unused[2];
};
struct SysvSemDs {
    SysvIpcPerm permission;
    int64_t operationTime;
    uint64_t reservedTime1;
    int64_t changeTime;
    uint64_t reservedTime2, count, unused[2];
};
static_assert(sizeof(SysvIpcPerm)==48 && sizeof(SysvSemDs)==104);
static_assert(offsetof(SysvSemDs,count)==80);
int SysvSemGet(Process*, int32_t key, int count, int flags);
int SysvSemCount(Process*, int id);
// Input/output is kernel-owned storage. Control revalidates identity and size.
int SysvSemControl(Process*, int id, int number, int command, int value,
                   SysvSemDs*, uint16_t* values, unsigned count);
int SysvSemOperate(Process*, int id, const SysvSemOp*, unsigned count,
                   uint64_t deadlineNs=~uint64_t{0});
// Before clone publication; CLONE_SYSVSEM shares independently of CLONE_THREAD.
int SysvSemClone(Process* parent, Process* child, bool share);
// Outside scheduler locks, before dropping termination's temporary reference.
// Allocation-free, non-sleeping, idempotent; safe with interrupts disabled.
void SysvSemExit(Process*);
}
