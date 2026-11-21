#pragma once
#include <stdint.h>

// ProcFS VFS driver — registers "procfs" filesystem with the VFS layer.

namespace brook {

void ProcFsVfsRegister();
struct Vnode;
int ProcFsOpenedNamespaceType(const Vnode* node);
struct MountNamespace;
struct VfsPath;
int ProcFsMountPrivate(MountNamespace* ns,const VfsPath& root,const VfsPath& target,uint64_t attributes);

} // namespace brook
