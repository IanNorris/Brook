#pragma once

#include "pid_namespace_ids.h"

namespace brook {
struct Process;
struct UserNamespace;

const PidNamespace* ProcessPidView(const Process* process);
uint32_t ProcessVisibleTid(const Process* process, const PidNamespace* view);
uint32_t ProcessVisibleTgid(const Process* process, const PidNamespace* view);
uint32_t ProcessVisibleGroup(const Process* process, const PidNamespace* view, bool session);
void ProcessSetGroupView(Process* process, const Process* source, bool session, bool newGroup);

void PidNamespaceRetain(const PidNamespace* view);
void PidNamespaceRelease(const PidNamespace* view);

// An unpublished birth owns both its active view and the initial view for its
// future children. Publish with the scheduler registry locked.
int ProcessPidPrepareChild(Process* child, const Process* parent, bool newNamespace);
bool ProcessPidPublish(Process* process);
void ProcessPidRelease(Process* process);

int ProcessPidUnshare(Process* process);
bool ProcessPidIsNamespaceInit(const Process* process);
void PidNamespaceStopBirths(const PidNamespace* view);
}
