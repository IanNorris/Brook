// devtmpfs.cpp -- boot /dev as a tmpfs instance of real device inodes.
//
// Historically Brook served /dev/null, /dev/tty and friends by matching path
// text in the open syscall. Those names had no inode, so stat/access failed,
// and a container that bound /dev (bubblewrap's --dev-bind, or --dev, which
// binds single nodes onto its own tmpfs) lost them. Device nodes here carry
// their Linux device number; OpenCharacterDevice in syscall_open.cpp selects
// the driver from st_rdev after the ordinary VFS open.

#include "devtmpfs.h"
#include "mount_namespace.h"
#include "serial.h"
#include "tmpfs.h"
#include "vfs.h"
#include "vfs_path.h"

namespace brook {
namespace {

struct DevSeed {
    const char* name;
    uint16_t mode;
    uint64_t rdev;
    const char* target;
};

constexpr DevSeed kDevSeeds[] = {
    {"null",    0020666, DevEncode(1, 3), nullptr},
    {"zero",    0020666, DevEncode(1, 5), nullptr},
    {"full",    0020666, DevEncode(1, 7), nullptr},
    {"random",  0020666, DevEncode(1, 8), nullptr},
    {"urandom", 0020666, DevEncode(1, 9), nullptr},
    {"tty",     0020666, DevEncode(5, 0), nullptr},
    {"console", 0020600, DevEncode(5, 1), nullptr},
    // Covered at boot by the desktop shared-memory tmpfs.
    {"shm",     0041777, 0, nullptr},
    // Canonical DRM anchor: render nodes stay synthetic beneath it.
    {"dri",     0040755, 0, nullptr},
    {"fd",      0120777, 0, "/proc/self/fd"},
    {"stdin",   0120777, 0, "/proc/self/fd/0"},
    {"stdout",  0120777, 0, "/proc/self/fd/1"},
    {"stderr",  0120777, 0, "/proc/self/fd/2"},
};

// NOSUID only: device nodes must open, unlike ordinary NODEV tmpfs mounts.
constexpr uint64_t DevMountAttributes = 2;

} // namespace

int DevtmpfsMountBoot()
{
    int error = 0;
    Tmpfs* instance = TmpfsCreateDeviceDirectory(&error);
    if (!instance) return error ? error : -12;
    for (const auto& seed : kDevSeeds) {
        error = TmpfsSeedEntry(instance, seed.name, seed.mode, seed.rdev, seed.target);
        if (error) {
            SerialPrintf("VFS: /dev seed %s failed (%d)\n", seed.name, error);
            TmpfsDestroy(instance);
            return error;
        }
    }
    VfsPath root{}, target{};
    error = VfsPathRoot(&root);
    if (!error) error = VfsPathResolve(root, root, "/dev", &target);
    if (!error)
        error = VfsMountNamespaceMountFresh(VfsMountNamespaceInitial(), target, "tmpfs",
                                            TmpfsOperations(), instance, 0, DevMountAttributes);
    VfsPathRelease(target);
    if (error) {
        VfsPathRelease(root);
        TmpfsDestroy(instance);
        return error;
    }
    // Brook-specific synthetic names (/dev/fb0, /dev/keyboard, /dev/klog,
    // /dev/dsp, guard and maintenance nodes) are still brokered by path text.
    // This mount, and whole binds of it into containers, must not mask them.
    VfsPath mounted{};
    int alias = VfsPathResolve(root, root, "/dev", &mounted);
    if (!alias) alias = VfsPathBrokerTransparentRegister(mounted);
    VfsPathRelease(mounted);
    VfsPathRelease(root);
    if (alias) SerialPrintf("VFS: /dev synthetic passthrough unavailable (%d)\n", alias);
    return 0;
}

} // namespace brook
