#pragma once
#include "vfs_path.h"
namespace brook {
struct MountNamespace;
struct UserNamespace;
// Namespace handles own an active topology lease plus storage, independently
// of task membership. Plain paths/snapshots retain storage only.
bool VfsMountNamespaceHandleAcquire(MountNamespace* borrowed);
void VfsMountNamespaceHandleRelease(MountNamespace* owned);
uint64_t VfsMountNamespaceIdentity(MountNamespace* borrowed);
UserNamespace* VfsMountNamespaceUserOwnerAcquire(MountNamespace* ns);
int VfsMountNamespaceSetUserOwnerPrepared(MountNamespace* ns,UserNamespace* owner);
// Storage references never count as live task membership. All returned paths
// and namespace outputs are independently owned; errors leave outputs empty.
MountNamespace* VfsMountNamespaceInitial();
void VfsMountNamespaceRetain(MountNamespace* ns);
void VfsMountNamespaceRelease(MountNamespace* ns);
bool VfsMountNamespaceActivate(MountNamespace* ns);
bool VfsMountNamespaceJoin(MountNamespace* ns);
bool VfsMountNamespaceAbandon(MountNamespace* ns,bool wake=true);
void VfsMountNamespaceLeave(MountNamespace* ns,bool wake=true);
int VfsMountNamespaceCopy(MountNamespace* source,const VfsPath& root,const VfsPath& cwd,
                          MountNamespace** out,VfsPath* newRoot,VfsPath* newCwd);
// Core topology operations; syscall authorization/flag validation is separate.
int VfsMountNamespaceBind(MountNamespace* ns,const VfsPath& source,const VfsPath& target);
// PRIVATE and SLAVE (+ optional REC) coincide while every mount is private
// and has no master/peers. Validate a live owned mount root; no topology change.
// Shared/unbindable support must replace this private-only contract first.
int VfsMountNamespacePrivatePropagation(MountNamespace* ns,const VfsPath& target);
int VfsMountNamespaceDetach(MountNamespace* ns,const VfsPath& target);
// Borrowed namespace must have a live storage owner. Each value is copied
// under the metadata view lock; count/index sequences may race mutation.
uint32_t VfsMountNamespaceCount(MountNamespace* ns);
VfsMountSnapshot VfsMountNamespaceSnapshotAt(MountNamespace* ns,uint32_t index);
// Backend destruction runs only in a sleeping VFS transaction, never in the
// member departure hook. Pending work remains visible until actually drained.
bool VfsMountCleanupPending();
void VfsDrainMountCleanup();
void VfsWakeMountCleanup();
} // namespace brook
