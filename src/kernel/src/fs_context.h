#pragma once
#include "vfs_path.h"

namespace brook {
struct FsContext;
struct MountNamespace;
// Snapshot outputs must be empty or previously released before reuse.
// Each successful snapshot owns root and cwd independently. Release them
// outside process/context publication locks. Names are not stored as authority.
struct FsSnapshot { VfsPath root{},cwd{};uint16_t mask=022;MountNamespace* mountNamespace=nullptr;uint64_t revision=0; };
void FsSnapshotRelease(FsSnapshot* snapshot);
// Borrowed live paths; validates directory types, acquires independent holds.
FsContext* FsContextCreate(const VfsPath& root,const VfsPath& cwd,uint16_t mask,int* error,MountNamespace* ns=nullptr);
void FsContextRetain(FsContext* context);
void FsContextRelease(FsContext* context);
// Immutable borrowed namespace; caller keeps the context alive.
MountNamespace* FsContextMountNamespace(FsContext* context);
// Caller holds a live context throughout each operation. No VFS/backend work
// or final destructor occurs under the short context lock.
int FsContextSnapshot(FsContext* context,FsSnapshot* out);
FsContext* FsContextCopy(FsContext* context,int* error);
int FsContextSetCwd(FsContext* context,const VfsPath& cwd);
// Path-based resolution must retry on EAGAIN if its snapshot changed. A
// deliberate fchdir through a retained descriptor uses SetCwd instead.
int FsContextSetResolvedCwd(FsContext* context,const VfsPath& cwd,const FsSnapshot& expected);
// Caller holds the VFS transaction and independent old/new path leases.
// Callback must be prevalidated stores only: no allocation/backend/free.
int FsContextCommitRootMove(const VfsPath& oldRoot,const VfsPath& newRoot,
                           void (*publishGraph)(void*),void* argument);
int FsContextReadMask(FsContext* context);
int FsContextExchangeMask(FsContext* context,uint16_t mask);
} // namespace brook
