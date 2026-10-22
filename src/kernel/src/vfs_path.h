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
int VfsPathRoot(VfsPath* out);
// The caller retains vnode through acquisition (e.g. an FD snapshot).
int VfsPathFromVnode(Vnode* vnode,VfsPath* out);
// root/start are borrowed live references. Absolute paths restart at root;
// retained start never re-enters a newly covering mount. Errors leave out empty.
int VfsPathResolve(const VfsPath& root,const VfsPath& start,const char* text,
                   VfsPath* out,bool followFinal=true,bool realIds=false);
int VfsPathStat(const VfsPath& path,VnodeStat* out);
int VfsPathAccess(const VfsPath& path,uint8_t required,bool realIds=false);
// Opens an existing exact object. Creation is a separate parent operation.
Vnode* VfsPathOpen(const VfsPath& path,int flags,int* error);
} // namespace brook
