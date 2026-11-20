#pragma once

#include "pid_namespace_ids.h"

namespace brook {
struct Process;
struct UserNamespace;
inline constexpr uint64_t CloneNewPid = 0x20000000;

const PidNamespace* ProcessPidView(const Process* process);
uint32_t ProcessVisibleTid(const Process* process, const PidNamespace* view);
uint32_t ProcessVisibleTgid(const Process* process, const PidNamespace* view);
uint32_t ProcessVisibleGroup(const Process* process, const PidNamespace* view, bool session);
void ProcessSetGroupView(Process* process, const Process* source, bool session, bool newGroup);

uint64_t PidNamespaceTag(const PidNamespace* view);
UserNamespace* PidNamespaceOwner(const PidNamespace* view);
bool PidNamespaceContains(const PidNamespace* ancestor, const PidNamespace* view);
void PidNamespaceRetain(const PidNamespace* view);
void PidNamespaceRelease(const PidNamespace* view);

// An unpublished birth owns both its active view and the initial view for its
// future children. Publish with the scheduler registry locked.
int ProcessPidPrepareChild(Process* child, const Process* parent, bool newNamespace);
int ProcessPidPrepareInjectedThread(Process* child, const Process* target);
bool ProcessPidPublish(Process* process, bool runnable = true);
void ProcessPidRelease(Process* process);

// Delayed reports (e.g. queued SCM_CREDENTIALS) may outlive the task and
// its namespace. Use non-reused view tags so they require no owning pointers.
struct PidGroupIdentity {
    uint32_t global;
    unsigned count;
    struct Entry { uint64_t viewTag; uint32_t tgid; } entries[PidNamespaceIds::MaxDepth];

    uint32_t Visible(uint64_t viewTag) const {
        if (!viewTag) return global;
        for (unsigned i = 0; i < count; ++i)
            if (entries[i].viewTag == viewTag) return entries[i].tgid;
        return 0;
    }
};
PidGroupIdentity ProcessPidGroupIdentity(const Process* process);
int ProcessPidUnshare(Process* process);
bool ProcessPidIsNamespaceInit(const Process* process);
bool PidNamespaceStopBirths(const PidNamespace* view);
bool PidNamespaceDraining(const PidNamespace* view);
uint32_t PidNamespaceTaskCount(const PidNamespace* view);
void PidNamespaceFinishDrain(const PidNamespace* view);
}
