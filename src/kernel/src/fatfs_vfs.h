#pragma once

// FatFS VFS driver — registers "fatfs" filesystem with the VFS layer.
// Call FatFsVfsRegister() at boot before any VfsMount("...", "fatfs", ...).

namespace brook {

void FatFsVfsRegister();

} // namespace brook
