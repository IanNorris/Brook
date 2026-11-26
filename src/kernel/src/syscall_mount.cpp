#include "syscall_user_path.h"
// syscall_mount.cpp -- mount namespace syscalls: mount/umount2/pivot_root.
//
// Split from syscall.cpp.
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "mount_namespace.h"
#include "procfs_vfs.h"
#include "tmpfs_mount_options.h"
#include "posix_mqueue.h"
#include "process_user_namespace.h"

#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_open.h"
#include "syscall_handlers.h"

namespace brook {

#include "syscall_mount_namespace.inc"

} // namespace brook
