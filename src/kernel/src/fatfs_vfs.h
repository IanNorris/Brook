#pragma once

// FatFS VFS driver — registers "fatfs" filesystem with the VFS layer.
// Call FatFsVfsRegister() at boot before any VfsMount("...", "fatfs", ...).

namespace brook {

struct Vnode;
void FatFsVfsRegister();
// True only for a streaming regular FIL whose retained drive binding is immutable.
bool FatFsFileSourceReadOnly(Vnode* node);

} // namespace brook
