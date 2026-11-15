#include "compat_paths.h"
#include "mount_namespace.h"
#include "serial.h"
#include "tmpfs.h"
#include "vfs.h"
#include "vfs_path.h"

namespace brook {
namespace {

// A handful of symlinks; the quota bounds accidental root additions too.
constexpr uint64_t CompatTmpfsBytes = 64 * 1024;
// NOSUID | NODEV, writable so root may add entries.
constexpr uint64_t CompatMountAttributes = 6;

struct BootContext {
    VfsPath root;
};

int Probe(void* context, const char* path)
{
    auto* boot = static_cast<BootContext*>(context);
    VnodeStat stat{};
    const int error = VfsStatAt(boot->root, boot->root, path, &stat, false);
    if (error) return error;
    return stat.isDir && !stat.isSymlink ? 0 : -20;
}

int MakeDirectory(void* context, const char* path)
{
    auto* boot = static_cast<BootContext*>(context);
    return VfsMkdirAt(boot->root, boot->root, path, 0755, 0);
}

int Entries(void* context, const char* path)
{
    auto* boot = static_cast<BootContext*>(context);
    int error = 0;
    Vnode* directory = VfsOpenAt(boot->root, boot->root, path, VFS_O_READ, 0, 0, &error, false);
    if (!directory) return error ? error : -5;
    DirEntry entry{};
    uint32_t cookie = 0;
    int result = 0;
    for (;;) {
        const int read = VfsReaddir(directory, &entry, &cookie);
        if (read < 0) { result = read; break; }
        if (read == 0) break;
        const char* n = entry.name;
        if ((n[0] == '.' && !n[1]) || (n[0] == '.' && n[1] == '.' && !n[2])) continue;
        result = 1;
        break;
    }
    VfsClose(directory);
    return result;
}

int MountTmpfs(void* context, const char* path)
{
    auto* boot = static_cast<BootContext*>(context);
    VfsPath target{};
    int error = VfsPathResolve(boot->root, boot->root, path, &target, false);
    if (!error) {
        Tmpfs* instance = TmpfsCreate(0, 0, nullptr, CompatMountAttributes, &error, 0755, CompatTmpfsBytes);
        if (instance) {
            error = VfsMountNamespaceMountFresh(VfsMountNamespaceInitial(), target, "tmpfs",
                                                TmpfsOperations(), instance, 0, CompatMountAttributes);
            if (error) TmpfsDestroy(instance);
        } else if (!error) {
            error = -12;
        }
    }
    VfsPathRelease(target);
    if (error) return error;
    // Keep exec/stat aliases such as /bin/bash -> /boot/BIN/bash working:
    // the new directory must not mask them as an ordinary overmount would.
    VfsPath mounted{};
    int alias = VfsPathResolve(boot->root, boot->root, path, &mounted, false);
    if (!alias) alias = VfsPathBrokerTransparentRegister(mounted);
    VfsPathRelease(mounted);
    if (alias) SerialPrintf("VFS: compat path %s alias passthrough unavailable (%d)\n", path, alias);
    return 0;
}

int Symlink(void* context, const char* target, const char* path)
{
    auto* boot = static_cast<BootContext*>(context);
    return VfsSymlinkAt(boot->root, boot->root, target, path);
}

void Report(void*, const char* path, const char* event, int error)
{
    SerialPrintf("VFS: compat path %s %s (%d)\n", path, event, error);
}

} // namespace

CompatPathSummary CompatPathsProvisionBoot()
{
    BootContext boot{};
    CompatPathSummary summary{};
    if (int error = VfsPathRoot(&boot.root)) {
        SerialPrintf("VFS: compat paths unavailable: root error %d\n", error);
        summary.failed = 1;
        return summary;
    }
    const CompatPathOps ops{&boot, Probe, MakeDirectory, Entries, MountTmpfs, Symlink, Report};
    summary = CompatPathsProvision(kCompatPathLinks, kCompatPathLinkCount, ops);
    VfsPathRelease(boot.root);
    SerialPrintf("VFS: compat paths mounted=%u links=%u skipped=%u failed=%u (tmpfs 0755 root,nodev,nosuid)\n",
                 summary.mounted, summary.links, summary.skipped, summary.failed);
    return summary;
}

} // namespace brook
