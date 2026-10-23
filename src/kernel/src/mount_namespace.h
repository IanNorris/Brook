#pragma once
#include "vfs_path.h"
namespace brook {
struct MountNamespace;
struct UserNamespace;
struct VfsFsOps;
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
// Publish a fresh filesystem instance over an existing directory. The registered
// provider must match ops. On success ownership of privateData transfers to the
// new backing; on any error the caller still owns it and must dispose it. No
// driver mount callback or (driver,pdrv) backing coalescing occurs. Bind/copy
// retain the resulting backing normally. Authorization/options are caller work.
int VfsMountNamespaceMountFresh(MountNamespace* ns,const VfsPath& target,
    const char* fsName,const VfsFsOps* ops,void* privateData,uint8_t pdrv=0,uint64_t attributes=0);
// Owned copied metadata; no backend callbacks or nested VFS lock acquisition.
// Caller holds namespace/root leases. Rows are ordered parent before child.
struct VfsMountInfo {
    uint64_t id,parent,device,attributes;
    char root[512],mountPoint[64],fsType[16];
};
int VfsMountNamespaceInfo(MountNamespace* ns,const VfsPath& root,VfsMountInfo** rows,uint32_t* count);
void VfsMountInfoFree(VfsMountInfo* rows);
// Core topology operations; syscall authorization/flag validation is separate.
// Atomic pathname entry point also supports unique regular-file and socket entries.
// Hardlinked entries and other nondirectory types remain unsupported.
int VfsMountNamespaceBindAt(MountNamespace* ns,const VfsPath& root,const VfsPath& start,
    const char* source,const char* target,bool recursive=false);
int VfsMountNamespaceBind(MountNamespace* ns,const VfsPath& source,const VfsPath& target,bool recursive=false);
// Serialize exact namespace-root validation with creation of an FsContext.
// The callback only registers a fully allocated context; no backend/free work.
int VfsMountNamespacePublishContext(MountNamespace* ns,const VfsPath& root,void (*publish)(void*),void* argument);
int VfsMountNamespacePivotRoot(MountNamespace* ns,const VfsPath& oldRoot,const VfsPath& newRoot,const VfsPath& putOld);
// PRIVATE and SLAVE (+ optional REC) coincide while every mount is private
// and has no master/peers. Validate a live owned mount root; no topology change.
// Shared/unbindable support must replace this private-only contract first.
int VfsMountNamespacePrivatePropagation(MountNamespace* ns,const VfsPath& target);
// Bounded bind remount: NOSUID is mandatory; NODEV and RO may be added.
// Unsupported flags, initial namespaces and weakened restrictions fail without
// mutation. First RO admission requires a retained root and retired data holds.
int VfsMountNamespaceRemount(MountNamespace* ns,const VfsPath& target,uint64_t attributes);
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
