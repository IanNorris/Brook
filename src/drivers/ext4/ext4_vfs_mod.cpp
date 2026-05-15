// ext4_vfs_mod.cpp — lwext4 filesystem driver module for Brook VFS.
//
// Bridges lwext4's POSIX-like API to Brook's VfsFsOps/VnodeOps interface.
// Loaded as a kernel module; registers "ext4" filesystem type with VFS.
//
// Architecture:
//   Brook Device* → ext4_blockdev_iface (bread/bwrite) → lwext4 → VfsFsOps
//
// Usage (from kernel init):
//   Ext4BindDevice(0, someBlockDev);
//   VfsMount("/nix", "ext4", 0);

#include "module_abi.h"
#include "vfs.h"
#include "device.h"
#include "serial.h"
#include "kprintf.h"
#include "string.h"
#include "memory/heap.h"
#include "spinlock.h"

// lwext4 headers (C)
extern "C" {
#include <ext4.h>
#include <ext4_blockdev.h>
#include <ext4_mkfs.h>
}

MODULE_IMPORT_SYMBOL(VfsRegisterFs);
MODULE_IMPORT_SYMBOL(VfsUnregisterFs);
MODULE_IMPORT_SYMBOL(kmalloc);
MODULE_IMPORT_SYMBOL(kfree);
MODULE_IMPORT_SYMBOL(krealloc);
MODULE_IMPORT_SYMBOL(DeviceFind);
MODULE_IMPORT_SYMBOL(SerialPrintf);
MODULE_IMPORT_SYMBOL(SerialVPrintf);

using namespace brook;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

static constexpr uint8_t  EXT4_MAX_MOUNTS = 4;
static constexpr uint32_t MAX_PATH_LEN    = 4096;

// ---------------------------------------------------------------------------
// Per-mount state
// ---------------------------------------------------------------------------

struct Ext4Mount {
    Device*               dev;
    ext4_blockdev_iface   bdif;
    ext4_blockdev         bdev;
    char                  devName[32];  // "ext4dev0", registered with lwext4
    char                  mountPt[16];  // "/mp0/", the lwext4 mount prefix
    bool                  mounted;
    ext4_lock             lock;
    SpinLock              spinlock;
};

static Ext4Mount* g_mounts[EXT4_MAX_MOUNTS] = {};
static Device*    g_devices[EXT4_MAX_MOUNTS] = {};

// ---------------------------------------------------------------------------
// Block device adapter: Brook Device* → ext4_blockdev_iface
// ---------------------------------------------------------------------------

static Ext4Mount* bdev_to_mount(ext4_blockdev* bdev) {
    // bdev is embedded in Ext4Mount, recover the container
    return reinterpret_cast<Ext4Mount*>(
        reinterpret_cast<char*>(bdev) - offsetof(Ext4Mount, bdev));
}

static int brook_bdev_open(ext4_blockdev* bdev) {
    (void)bdev;
    return 0;  // already open
}

static int brook_bdev_bread(ext4_blockdev* bdev, void* buf,
                            uint64_t blk_id, uint32_t blk_cnt) {
    Ext4Mount* m = bdev_to_mount(bdev);
    uint32_t bsize = m->bdif.ph_bsize;
    uint64_t offset = blk_id * bsize;
    uint64_t len = (uint64_t)blk_cnt * bsize;

    int ret = m->dev->ops->read(m->dev, offset, buf, len);
    if (ret < 0) return EIO;
    return 0;
}

static int brook_bdev_bwrite(ext4_blockdev* bdev, const void* buf,
                             uint64_t blk_id, uint32_t blk_cnt) {
    Ext4Mount* m = bdev_to_mount(bdev);
    uint32_t bsize = m->bdif.ph_bsize;
    uint64_t offset = blk_id * bsize;
    uint64_t len = (uint64_t)blk_cnt * bsize;

    int ret = m->dev->ops->write(m->dev, offset, buf, len);
    if (ret < 0) return EIO;
    return 0;
}

static int brook_bdev_close(ext4_blockdev* bdev) {
    (void)bdev;
    return 0;
}

static int brook_bdev_lock(ext4_blockdev* bdev) {
    Ext4Mount* m = bdev_to_mount(bdev);
    SpinLockAcquire(&m->spinlock);
    return 0;
}

static int brook_bdev_unlock(ext4_blockdev* bdev) {
    Ext4Mount* m = bdev_to_mount(bdev);
    SpinLockRelease(&m->spinlock);
    return 0;
}

// ---------------------------------------------------------------------------
// Mount-point lock (for ext4_device_register's lock callbacks)
// ---------------------------------------------------------------------------

// We use per-slot static spinlocks for the mount-point lock.
static SpinLock g_mpLock[EXT4_MAX_MOUNTS] = {};

static void mp_lock_0() { SpinLockAcquire(&g_mpLock[0]); }
static void mp_unlock_0() { SpinLockRelease(&g_mpLock[0]); }
static void mp_lock_1() { SpinLockAcquire(&g_mpLock[1]); }
static void mp_unlock_1() { SpinLockRelease(&g_mpLock[1]); }
static void mp_lock_2() { SpinLockAcquire(&g_mpLock[2]); }
static void mp_unlock_2() { SpinLockRelease(&g_mpLock[2]); }
static void mp_lock_3() { SpinLockAcquire(&g_mpLock[3]); }
static void mp_unlock_3() { SpinLockRelease(&g_mpLock[3]); }

static const ext4_lock g_mpLocks[EXT4_MAX_MOUNTS] = {
    { mp_lock_0, mp_unlock_0 },
    { mp_lock_1, mp_unlock_1 },
    { mp_lock_2, mp_unlock_2 },
    { mp_lock_3, mp_unlock_3 },
};

// ---------------------------------------------------------------------------
// Helper: build the lwext4 internal path from a mount + relative path
// ---------------------------------------------------------------------------

static void build_lwext4_path(const Ext4Mount* m, const char* relPath,
                              char* out, uint32_t outSize) {
    // lwext4 expects paths like "/mp0/path/to/file"
    // relPath from Brook VFS is already relative, e.g. "path/to/file"
    uint32_t mpLen = strlen(m->mountPt);
    uint32_t relLen = strlen(relPath);

    if (mpLen + relLen + 1 >= outSize) {
        out[0] = '\0';
        return;
    }

    memcpy(out, m->mountPt, mpLen);
    memcpy(out + mpLen, relPath, relLen);
    out[mpLen + relLen] = '\0';
}

// ---------------------------------------------------------------------------
// Vnode private data
// ---------------------------------------------------------------------------

struct Ext4FilePriv {
    ext4_file file;
    Ext4Mount* mount;
    bool isDir;
    ext4_dir dir;
    char path[MAX_PATH_LEN];  // full lwext4 path, needed for readdir
};

// ---------------------------------------------------------------------------
// VnodeOps — file operations
// ---------------------------------------------------------------------------

static int ext4_vn_open(Vnode* vn, int flags) {
    (void)vn; (void)flags;
    return 0;  // already opened during VfsFsOps::open
}

static int ext4_vn_read(Vnode* vn, void* buf, uint64_t len, uint64_t* offset) {
    auto* fp = static_cast<Ext4FilePriv*>(vn->priv);
    if (fp->isDir) return -EISDIR;

    // Seek to offset
    int r = ext4_fseek(&fp->file, *offset, SEEK_SET);
    if (r != 0) return -EIO;

    size_t bytesRead = 0;
    r = ext4_fread(&fp->file, buf, len, &bytesRead);
    if (r != 0 && bytesRead == 0) return -EIO;

    *offset += bytesRead;
    return (int)bytesRead;
}

static int ext4_vn_write(Vnode* vn, const void* buf, uint64_t len, uint64_t* offset) {
    auto* fp = static_cast<Ext4FilePriv*>(vn->priv);
    if (fp->isDir) return -EISDIR;

    int r = ext4_fseek(&fp->file, *offset, SEEK_SET);
    if (r != 0) return -EIO;

    size_t bytesWritten = 0;
    r = ext4_fwrite(&fp->file, buf, len, &bytesWritten);
    if (r != 0 && bytesWritten == 0) return -EIO;

    *offset += bytesWritten;
    return (int)bytesWritten;
}

static int ext4_vn_readdir(Vnode* vn, DirEntry* out, uint32_t* cookie) {
    auto* fp = static_cast<Ext4FilePriv*>(vn->priv);
    if (!fp->isDir) return -ENOTDIR;

    // cookie == 0 means start iteration
    if (*cookie == 0) {
        ext4_dir_close(&fp->dir);
        int r = ext4_dir_open(&fp->dir, fp->path);
        if (r != 0) return -EIO;
    }

    const ext4_direntry* de;
    while ((de = ext4_dir_entry_next(&fp->dir)) != nullptr) {
        // Skip . and ..
        if (de->name_length == 1 && de->name[0] == '.') continue;
        if (de->name_length == 2 && de->name[0] == '.' && de->name[1] == '.') continue;

        uint32_t nameLen = de->name_length;
        if (nameLen >= sizeof(out->name)) nameLen = sizeof(out->name) - 1;
        memcpy(out->name, de->name, nameLen);
        out->name[nameLen] = '\0';

        out->isDir = (de->inode_type == EXT4_DE_DIR);
        out->size = 0;  // size requires inode lookup — expensive, skip for readdir

        (*cookie)++;
        return 1;  // got an entry
    }

    return 0;  // end of directory
}

static void ext4_vn_close(Vnode* vn) {
    auto* fp = static_cast<Ext4FilePriv*>(vn->priv);
    if (!fp) return;

    if (fp->isDir) {
        ext4_dir_close(&fp->dir);
    } else {
        ext4_fclose(&fp->file);
    }

    kfree(fp);
    vn->priv = nullptr;
}

static int ext4_vn_stat(Vnode* vn, VnodeStat* st) {
    auto* fp = static_cast<Ext4FilePriv*>(vn->priv);

    if (fp->isDir) {
        st->size = 0;
        st->isDir = true;
        st->isSymlink = false;
        st->ino = 0;  // TODO: recover inode from ext4_dir
        st->mode = 0040755;
        st->uid = 0;
        st->gid = 0;
        return 0;
    }

    st->size = ext4_fsize(&fp->file);
    st->isDir = false;
    st->isSymlink = false;  // symlinks are resolved during open
    st->ino = 0;  // TODO: expose inode number
    st->mode = 0100644;
    st->uid = 0;
    st->gid = 0;
    return 0;
}

static const VnodeOps g_ext4VnodeOps = {
    ext4_vn_open,
    ext4_vn_read,
    ext4_vn_write,
    ext4_vn_readdir,
    ext4_vn_close,
    ext4_vn_stat,
};

// ---------------------------------------------------------------------------
// VfsFsOps — filesystem operations
// ---------------------------------------------------------------------------

static Ext4Mount* find_mount_by_priv(void* mountPriv) {
    return static_cast<Ext4Mount*>(mountPriv);
}

static bool ext4_fs_mount(uint8_t pdrv, void** mountPriv) {
    if (pdrv >= EXT4_MAX_MOUNTS || !g_devices[pdrv]) {
        SerialPrintf("ext4: no device bound for pdrv %u\n", pdrv);
        return false;
    }

    Device* dev = g_devices[pdrv];
    auto* blkOps = reinterpret_cast<const BlockDeviceOps*>(dev->ops);

    auto* m = static_cast<Ext4Mount*>(kmalloc(sizeof(Ext4Mount)));
    if (!m) return false;
    memset(m, 0, sizeof(Ext4Mount));

    m->dev = dev;

    // Set up block device interface
    m->bdif.open   = brook_bdev_open;
    m->bdif.bread  = brook_bdev_bread;
    m->bdif.bwrite = brook_bdev_bwrite;
    m->bdif.close  = brook_bdev_close;
    m->bdif.lock   = brook_bdev_lock;
    m->bdif.unlock = brook_bdev_unlock;
    m->bdif.ph_bsize = blkOps->block_size(dev);
    m->bdif.ph_bcnt  = blkOps->block_count(dev);
    m->bdif.ph_bbuf  = static_cast<uint8_t*>(kmalloc(m->bdif.ph_bsize));
    if (!m->bdif.ph_bbuf) {
        kfree(m);
        return false;
    }

    // Set up block device
    m->bdev.bdif = &m->bdif;
    m->bdev.part_offset = 0;
    m->bdev.part_size = m->bdif.ph_bcnt * m->bdif.ph_bsize;

    // Generate unique names
    // lwext4 device name: "ext4dev0"
    const char* digits = "0123456789";
    strcpy(m->devName, "ext4dev");
    char d[2] = { digits[pdrv % 10], '\0' };
    strcat(m->devName, d);

    // lwext4 mount point: "/mp0/"
    strcpy(m->mountPt, "/mp");
    strcat(m->mountPt, d);
    strcat(m->mountPt, "/");

    // Register block device with lwext4
    int r = ext4_device_register(&m->bdev, m->devName);
    if (r != 0) {
        SerialPrintf("ext4: device register failed: %d\n", r);
        kfree(m->bdif.ph_bbuf);
        kfree(m);
        return false;
    }

    // Mount within lwext4
    r = ext4_mount(m->devName, m->mountPt, false);
    if (r != 0) {
        SerialPrintf("ext4: mount failed: %d\n", r);
        ext4_device_unregister(m->devName);
        kfree(m->bdif.ph_bbuf);
        kfree(m);
        return false;
    }

    // Set mount-point lock
    r = ext4_mount_setup_locks(m->mountPt, &g_mpLocks[pdrv]);
    if (r != 0) {
        SerialPrintf("ext4: lock setup failed: %d\n", r);
        ext4_umount(m->mountPt);
        ext4_device_unregister(m->devName);
        kfree(m->bdif.ph_bbuf);
        kfree(m);
        return false;
    }

    // Enable journal recovery
    r = ext4_recover(m->mountPt);
    if (r != 0 && r != ENOTSUP) {
        SerialPrintf("ext4: journal recovery failed: %d (continuing)\n", r);
    }

    r = ext4_journal_start(m->mountPt);
    if (r != 0 && r != ENOTSUP) {
        SerialPrintf("ext4: journal start failed: %d (continuing)\n", r);
    }

    m->mounted = true;
    g_mounts[pdrv] = m;
    *mountPriv = m;

    SerialPrintf("ext4: mounted pdrv %u (%s) as %s — %lu blocks × %u bytes\n",
                 pdrv, dev->name, m->mountPt,
                 (unsigned long)m->bdif.ph_bcnt, m->bdif.ph_bsize);
    return true;
}

static void ext4_fs_unmount(void* mountPriv) {
    auto* m = find_mount_by_priv(mountPriv);
    if (!m || !m->mounted) return;

    ext4_journal_stop(m->mountPt);
    ext4_umount(m->mountPt);
    ext4_device_unregister(m->devName);

    m->mounted = false;
    // Find slot and clear
    for (uint8_t i = 0; i < EXT4_MAX_MOUNTS; ++i) {
        if (g_mounts[i] == m) {
            g_mounts[i] = nullptr;
            break;
        }
    }

    kfree(m->bdif.ph_bbuf);
    kfree(m);
}

static Vnode* ext4_fs_open(void* mountPriv, uint8_t pdrv, const char* relPath, int flags) {
    (void)pdrv;
    auto* m = find_mount_by_priv(mountPriv);
    if (!m || !m->mounted) return nullptr;

    char path[MAX_PATH_LEN];
    build_lwext4_path(m, relPath, path, sizeof(path));

    // Check if it's a directory
    ext4_dir dir;
    if (ext4_dir_open(&dir, path) == 0) {
        // It's a directory
        auto* fp = static_cast<Ext4FilePriv*>(kmalloc(sizeof(Ext4FilePriv)));
        if (!fp) { ext4_dir_close(&dir); return nullptr; }
        memset(fp, 0, sizeof(Ext4FilePriv));
        fp->isDir = true;
        fp->mount = m;
        fp->dir = dir;
        strcpy(fp->path, path);

        auto* vn = static_cast<Vnode*>(kmalloc(sizeof(Vnode)));
        if (!vn) { ext4_dir_close(&dir); kfree(fp); return nullptr; }
        memset(vn, 0, sizeof(Vnode));
        vn->ops = &g_ext4VnodeOps;
        vn->type = VnodeType::Dir;
        vn->priv = fp;
        vn->refCount = 1;
        return vn;
    }

    // Try as a file
    const char* openFlags = "r";
    if (flags & VFS_O_WRITE) {
        if (flags & VFS_O_CREATE)
            openFlags = "w+";  // create + read/write
        else
            openFlags = "r+";  // existing, read/write
    }
    if (flags & VFS_O_CREATE)
        openFlags = "w+";

    ext4_file f;
    int r = ext4_fopen2(&f, path, O_RDWR);
    if (r != 0) {
        // Try read-only
        r = ext4_fopen2(&f, path, O_RDONLY);
        if (r != 0) {
            if (flags & VFS_O_CREATE) {
                r = ext4_fopen2(&f, path, O_RDWR | O_CREAT);
                if (r != 0) return nullptr;
            } else {
                return nullptr;
            }
        }
    }

    if (flags & VFS_O_TRUNC) {
        ext4_ftruncate(&f, 0);
    }

    auto* fp = static_cast<Ext4FilePriv*>(kmalloc(sizeof(Ext4FilePriv)));
    if (!fp) { ext4_fclose(&f); return nullptr; }
    memset(fp, 0, sizeof(Ext4FilePriv));
    fp->isDir = false;
    fp->mount = m;
    fp->file = f;
    strcpy(fp->path, path);

    auto* vn = static_cast<Vnode*>(kmalloc(sizeof(Vnode)));
    if (!vn) { ext4_fclose(&f); kfree(fp); return nullptr; }
    memset(vn, 0, sizeof(Vnode));
    vn->ops = &g_ext4VnodeOps;
    vn->type = VnodeType::File;
    vn->priv = fp;
    vn->refCount = 1;

    return vn;
}

static int ext4_fs_stat_path(void* mountPriv, uint8_t pdrv, const char* relPath, VnodeStat* st) {
    (void)pdrv;
    auto* m = find_mount_by_priv(mountPriv);
    if (!m || !m->mounted) return -ENOENT;

    char path[MAX_PATH_LEN];
    build_lwext4_path(m, relPath, path, sizeof(path));

    // Check directory first
    ext4_dir dir;
    if (ext4_dir_open(&dir, path) == 0) {
        ext4_dir_close(&dir);
        st->size = 0;
        st->isDir = true;
        st->isSymlink = false;
        st->ino = 0;
        st->mode = 0040755;
        st->uid = 0;
        st->gid = 0;
        return 0;
    }

    // Try as file
    ext4_file f;
    int r = ext4_fopen2(&f, path, O_RDONLY);
    if (r != 0) return -ENOENT;

    st->size = ext4_fsize(&f);
    st->isDir = false;
    st->isSymlink = false;
    st->ino = 0;  // TODO
    st->mode = 0100644;
    st->uid = 0;
    st->gid = 0;

    ext4_fclose(&f);
    return 0;
}

static int ext4_fs_lstat_path(void* mountPriv, uint8_t pdrv, const char* relPath, VnodeStat* st) {
    // lwext4 follows symlinks automatically — for now, lstat == stat
    // TODO: if lwext4 gains O_NOFOLLOW support, use it here
    return ext4_fs_stat_path(mountPriv, pdrv, relPath, st);
}

static int ext4_fs_unlink(void* mountPriv, uint8_t pdrv, const char* relPath) {
    (void)pdrv;
    auto* m = find_mount_by_priv(mountPriv);
    if (!m || !m->mounted) return -ENOENT;

    char path[MAX_PATH_LEN];
    build_lwext4_path(m, relPath, path, sizeof(path));

    int r = ext4_fremove(path);
    return (r == 0) ? 0 : -EIO;
}

static int ext4_fs_mkdir(void* mountPriv, uint8_t pdrv, const char* relPath) {
    (void)pdrv;
    auto* m = find_mount_by_priv(mountPriv);
    if (!m || !m->mounted) return -ENOENT;

    char path[MAX_PATH_LEN];
    build_lwext4_path(m, relPath, path, sizeof(path));

    int r = ext4_dir_mk(path);
    return (r == 0) ? 0 : -EIO;
}

static int ext4_fs_rename(void* mountPriv, uint8_t pdrv,
                          const char* oldRelPath, const char* newRelPath) {
    (void)pdrv;
    auto* m = find_mount_by_priv(mountPriv);
    if (!m || !m->mounted) return -ENOENT;

    char oldPath[MAX_PATH_LEN], newPath[MAX_PATH_LEN];
    build_lwext4_path(m, oldRelPath, oldPath, sizeof(oldPath));
    build_lwext4_path(m, newRelPath, newPath, sizeof(newPath));

    int r = ext4_frename(oldPath, newPath);
    return (r == 0) ? 0 : -EIO;
}

static int ext4_fs_symlink(void* mountPriv, uint8_t pdrv,
                           const char* target, const char* linkRelPath) {
    (void)pdrv;
    auto* m = find_mount_by_priv(mountPriv);
    if (!m || !m->mounted) return -ENOENT;

    char linkPath[MAX_PATH_LEN];
    build_lwext4_path(m, linkRelPath, linkPath, sizeof(linkPath));

    int r = ext4_fsymlink(target, linkPath);
    return (r == 0) ? 0 : -EIO;
}

static int ext4_fs_readlink(void* mountPriv, uint8_t pdrv,
                            const char* relPath, char* buf, uint64_t bufsiz) {
    (void)pdrv;
    auto* m = find_mount_by_priv(mountPriv);
    if (!m || !m->mounted) return -ENOENT;

    char path[MAX_PATH_LEN];
    build_lwext4_path(m, relPath, path, sizeof(path));

    size_t outLen = 0;
    int r = ext4_readlink(path, buf, bufsiz, &outLen);
    if (r != 0) return -EIO;
    return (int)outLen;
}

static void ext4_fs_sync(void* mountPriv) {
    auto* m = find_mount_by_priv(mountPriv);
    if (!m || !m->mounted) return;

    ext4_cache_write_back(m->mountPt, false);
    ext4_cache_write_back(m->mountPt, true);
}

static const VfsFsOps g_ext4FsOps = {
    ext4_fs_mount,
    ext4_fs_unmount,
    ext4_fs_open,
    ext4_fs_stat_path,
    ext4_fs_lstat_path,
    ext4_fs_unlink,
    ext4_fs_mkdir,
    ext4_fs_rename,
    ext4_fs_symlink,
    ext4_fs_readlink,
    ext4_fs_sync,
};

// ---------------------------------------------------------------------------
// Module init/exit
// ---------------------------------------------------------------------------

static int Ext4ModInit() {
    VfsRegisterFs("ext4", &g_ext4FsOps);
    SerialPrintf("ext4: filesystem driver registered\n");
    return 0;
}

static void Ext4ModExit() {
    // Unmount all active mounts
    for (uint8_t i = 0; i < EXT4_MAX_MOUNTS; ++i) {
        if (g_mounts[i] && g_mounts[i]->mounted) {
            ext4_fs_unmount(g_mounts[i]);
        }
    }
    VfsUnregisterFs("ext4");
}

DECLARE_MODULE("ext4", Ext4ModInit, Ext4ModExit,
               "ext4 filesystem driver (lwext4)");

// ---------------------------------------------------------------------------
// Public API: bind a block device to an ext4 pdrv slot
// ---------------------------------------------------------------------------

extern "C" bool Ext4BindDevice(uint8_t pdrv, Device* dev) {
    if (pdrv >= EXT4_MAX_MOUNTS || !dev) return false;
    g_devices[pdrv] = dev;
    SerialPrintf("ext4: bound device '%s' to pdrv %u\n", dev->name, pdrv);
    return true;
}
