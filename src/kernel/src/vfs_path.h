#pragma once
#include "vfs.h"

namespace brook {
// An owned backend identity plus its actual mount attachment. Copying this
// value does not transfer ownership: retain explicitly for each independent
// owner. Names are never lookup authority. Empty handles are safe to release.
struct VfsPath { void* attachment=nullptr; VfsObject* object=nullptr; };
void VfsPathRetain(const VfsPath& path);
void VfsPathRelease(VfsPath path);
bool VfsPathSame(const VfsPath& a,const VfsPath& b);
// Both operands must be retained; compares the backing object across bind aliases.
bool VfsPathSameObject(const VfsPath& a,const VfsPath& b);
int VfsPathRoot(VfsPath* out);
// Synthetic syscall brokers may serve only paths whose retained ancestry has
// not crossed a directory bind. Missing synthetic tails are permitted; a bind
// must mask broker entries just as it masks ordinary backing entries.
int VfsPathBrokerAllowed(const VfsPath& root,const char* absolute,bool* allowed);
// Narrow null/urandom device compatibility bridge. Provision once before userspace.
// Only a missing allowlisted device entry beneath the pinned canonical /dev is eligible;
// retained parent identity and NODEV are checked in one namespace transaction.
int VfsCanonicalDeviceAnchorInitialize();
int VfsCanonicalDeviceAllowed(const VfsPath& root,const VfsPath& start,const char* text,bool* allowed,bool followFinal=true,const char* leaf="null");
// Device activation only: metadata/O_PATH, directories, /dev/shm and existing
// device descriptors are unaffected. Called for a recognized broker device.
int VfsPathDeviceActivation(const VfsPath& root,const char* absolute);
// The caller retains vnode through acquisition (e.g. an FD snapshot).
int VfsPathFromVnode(Vnode* vnode,VfsPath* out);
// root/start are borrowed live references. Absolute paths restart at root;
// retained start never re-enters a newly covering mount. Errors leave out empty.
int VfsPathResolve(const VfsPath& root,const VfsPath& start,const char* text,
                   VfsPath* out,bool followFinal=true,bool realIds=false);
// Reporting only, including current backend directory names after rename.
// Backend callbacks already run inside a VFS namespace transaction. Only
// those callbacks may use this non-locking reporting variant.
int VfsPathDirectoryPathFromCallback(const VfsPath& path,char* out,uint32_t capacity);
int VfsPathDirectoryPath(const VfsPath& path,char* out,uint32_t capacity);
int VfsPathObjectPathFromCallback(const VfsPath& path,char* out,uint32_t capacity);
int VfsPathReadlink(const VfsPath& path,char* out,uint32_t capacity);
int VfsPathStat(const VfsPath& path,VnodeStat* out);
int VfsPathAccess(const VfsPath& path,uint8_t required,bool realIds=false);
// Opens an existing exact object. Creation is a separate parent operation.
Vnode* VfsPathOpen(const VfsPath& path,int flags,int* error);
// Atomic retained lookup through the following backend operation. Creation
// applies the supplied mask snapshot once. No operation reconstructs a parent
// name to dispatch a backend mutation.
Vnode* VfsOpenAt(const VfsPath& root,const VfsPath& start,const char* text,int flags,
                 uint16_t mode,uint16_t mask,int* error,bool followFinal=true);
// Open and executable permission check use held identities in one walk. The
// resolved name is reporting only, derived from that walk's final parent/leaf.
int VfsOpenExecutableAt(const VfsPath& root,const VfsPath& start,const char* text,
                        Vnode** out,char* resolved,uint32_t capacity);
int VfsStatAt(const VfsPath& root,const VfsPath& start,const char* text,VnodeStat* out,bool followFinal=true);
int VfsAccessAt(const VfsPath& root,const VfsPath& start,const char* text,uint8_t required,bool realIds=false,bool followFinal=true);
int VfsReadlinkAt(const VfsPath& root,const VfsPath& start,const char* text,char* out,uint32_t capacity);
int VfsMkdirAt(const VfsPath& root,const VfsPath& start,const char* text,uint16_t mode,uint16_t mask);
int VfsRemoveAt(const VfsPath& root,const VfsPath& start,const char* text,bool directory);
int VfsSymlinkAt(const VfsPath& root,const VfsPath& start,const char* target,const char* text);
Vnode* VfsCreateFifoAt(const VfsPath& root,const VfsPath& start,const char* text,uint16_t mode,uint16_t mask,int* error);
Vnode* VfsCreateSocketAt(const VfsPath& root,const VfsPath& start,const char* text,uint16_t mode,uint16_t mask,int* error);
int VfsRenameAt(const VfsPath& root,const VfsPath& oldStart,const char* oldText,
                const VfsPath& newStart,const char* newText);
int VfsChmodAt(const VfsPath& root,const VfsPath& start,const char* text,uint16_t mode);
int VfsChownAt(const VfsPath& root,const VfsPath& start,const char* text,uint32_t uid,uint32_t gid,bool followFinal=true);
int VfsSetTimesAt(const VfsPath& root,const VfsPath& start,const char* text,const VnodeTime* times,bool followFinal=true);
} // namespace brook
