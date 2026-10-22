#pragma once
#include "fs_context.h"
namespace brook {
struct Process;
// Caller must own a live Process reference (or be the running process).
// Acquisition and retention occur together under the publication lock.
FsContext* ProcessFsAcquire(Process* process);
// Consume an owned context in the task's existing namespace. Old contents are
// released after publication unlock; this is not an unshare operation.
void ProcessFsReplaceOwned(Process* process,FsContext* owned);
void ProcessFsClear(Process* process);
int ProcessFsInitialize(Process* process,const char* cwd="/boot");
int ProcessFsInherit(Process* child,Process* parent,bool share);
// Unpublished child only. Copy topology and remap root/cwd before publication.
int ProcessFsIsolateMounts(Process* child);
// Logical-exit hook may run under scheduler locks: no backend work, free or
// wake. It returns an owned storage lease to finish outside all those locks.
MountNamespace* ProcessFsRetireMounts(Process* process);
void ProcessFsFinishMountRetirement(MountNamespace* owned);
FsContext* ProcessFsCopy(Process* process,int* error);
int ProcessFsSnapshot(Process* process,FsSnapshot* out);
int ProcessFsSetCwd(Process* process,const VfsPath& path);
int ProcessFsSetInitialCwd(Process* process,const char* absolute);
int ProcessFsGetcwdFromVfsCallback(Process* process,char* out,uint32_t capacity);
int ProcessFsGetcwd(Process* process,char* out,uint32_t capacity);
int ProcessFsMask(Process* process);
int ProcessFsExchangeMask(Process* process,uint16_t mask);
// Display cache only. Safe under scheduler/registry locks; no VFS work.
void ProcessFsReadDisplay(const Process* process,char* out,uint32_t capacity);
} // namespace brook
