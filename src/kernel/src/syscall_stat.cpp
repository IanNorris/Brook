#include "syscall_user_path.h"
#include "fido_hidraw.h"
// syscall_stat.cpp -- stat family, statx, statfs, access/faccessat.
//
// Split from syscall.cpp.
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "process_user_namespace.h"

#include "syscall.h"
#include "busybox_applets.h"
#include "user_access_scope.h"
#include "desktop_identity.h"
#include "intel_bdw_buffer.h"
#include "net.h"
#include "syscall_internal.h"
#include "syscall_maintenance_block.h"
#include "syscall_drm.h"
#include "synthetic_system_files.h"
#include "smp.h"
#include "syscall_open.h"
#include "syscall_rw.h"
#include "syscall_netlink.h"
#include "syscall_handlers.h"
#include "nv_drm.h"

namespace brook {

// ---------------------------------------------------------------------------
// sys_access (21)
// ---------------------------------------------------------------------------

static bool BusyboxStatFallback(const char* path, VnodeStat* vs);
static bool DrmAccessResult(const char* path,uint8_t mode,bool realIds,int64_t* result);

// Inquiry only: do not use access() as authorization for a later operation.
static int64_t AccessAt(int dirfd, uint64_t pathAddr, uint64_t mode, uint64_t flags)
{
    constexpr uint64_t NOFOLLOW = 0x100, EACCESS = 0x200, EMPTY_PATH = 0x1000;
    if ((mode & ~7ULL) || (flags & ~(NOFOLLOW | EACCESS | EMPTY_PATH))) return -EINVAL;
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    PathBuffer path;
    // Copy once per page instead of acquiring the mapping mutex for every
    // pathname byte. The shared copier recovers concurrent unmap faults and
    // stops at page boundaries, including a NUL in the last mapped byte.
    int pathError=CopyUserPath(pathAddr, path);
    if (pathError) return pathError;
    if (!path[0]) {
        if (!(flags & EMPTY_PATH)) return -ENOENT;
        if (dirfd != -100) {
            FdRef ref(proc, dirfd);
            FdEntry* entry=ref.get();
            if (!entry) return -EBADF;
            if (entry->type==FdType::Vnode && entry->handle)
                return VfsAccess(static_cast<Vnode*>(entry->handle),static_cast<uint8_t>(mode),!(flags&EACCESS));
            return mode ? -95 : 0; // EOPNOTSUPP: non-vnode permission metadata is not implemented.
        }
        FsSnapshot fs{};int error=ProcessFsSnapshot(proc,&fs);
        if(!error)error=VfsPathAccess(fs.cwd,static_cast<uint8_t>(mode),!(flags&EACCESS));
        FsSnapshotRelease(&fs);return error;
    }
    AtLookup at;if(int error=at.Init(dirfd,path))return error;
    char resolved[512];bool hasSyntheticName=!at.SyntheticName(path,resolved,sizeof(resolved));
    const char* lookup=hasSyntheticName?resolved:"";
    VnodeStat st{};
    bool realIds=!(flags&EACCESS);
    int64_t deviceAccess=0;
    if(DrmAccessResult(lookup,static_cast<uint8_t>(mode),realIds,&deviceAccess))return deviceAccess;
    int result=VfsAccessAt(at.fs.root,at.start(),path,static_cast<uint8_t>(mode),realIds,!(flags&NOFOLLOW));
    if (result == 0) return 0;
    if (result != -ENOENT && result != -1) return result;
    static const char* syntheticPaths[] = {
        "/etc/passwd", "/etc/group", "/etc/shells", "/etc/hostname",
        "/etc/nsswitch.conf", "/etc/hosts",
        "/proc/version", nullptr
    };
    for (auto** sp = syntheticPaths; *sp; ++sp)
        if (StrEq(lookup, *sp))
            return ProcessCheckFileAccess(proc,0,0,0100444,static_cast<uint8_t>(mode),realIds);
    if (!BusyboxStatFallback(lookup,&st)) return -ENOENT;
    return ProcessCheckFileAccess(proc,st.uid,st.gid,st.mode ? st.mode : 0100755,
                                  static_cast<uint8_t>(mode),realIds);
}

int64_t sys_access(uint64_t pathAddr, uint64_t mode, uint64_t,
                  uint64_t, uint64_t, uint64_t)
{
    return AccessAt(-100, pathAddr, mode, 0);
}

// ---------------------------------------------------------------------------
// sys_stat (4) / sys_lstat (6) / sys_fstat (5) / sys_newfstatat (262)
// ---------------------------------------------------------------------------

// Linux x86-64 struct stat layout (musl)
struct LinuxStat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    uint64_t st_atime_sec;
    uint64_t st_atime_nsec;
    uint64_t st_mtime_sec;
    uint64_t st_mtime_nsec;
    uint64_t st_ctime_sec;
    uint64_t st_ctime_nsec;
    int64_t  __unused[3];
};

static void FillStat(LinuxStat* st, const VnodeStat& vs, const Vnode* vn = nullptr)
{
    auto* raw = reinterpret_cast<uint8_t*>(st);
    memset(raw, 0, sizeof(LinuxStat));

    // Use the stable inode number when available (from ext2 or vnode cacheId).
    // This is critical: the dynamic linker uses dev+ino to detect
    // already-loaded libraries.  The same file must return the same ino
    // across multiple stat calls; otherwise libraries get loaded twice.
    uint64_t ino = vs.ino;
    if (!ino && vn && vn->cacheId)
        ino = vn->cacheId;
    if (ino || vs.exactDescriptorMetadata) {
        st->st_ino = ino;
    } else {
        // Fallback for filesystems without stable inode numbers (FATFS, procfs)
        static volatile uint64_t s_nextIno = 100;
        st->st_ino = __atomic_fetch_add(&s_nextIno, 1, __ATOMIC_RELAXED);
    }
    st->st_dev = vs.dev;
    st->st_rdev = vs.rdev;
    st->st_nlink = vs.nlink;
    st->st_blksize = 4096;
    st->st_size = static_cast<int64_t>(vs.size);
    st->st_blocks = vs.exactDescriptorMetadata ? static_cast<int64_t>(vs.blocks) : (st->st_size + 511) / 512;
    st->st_uid = vs.exactDescriptorMetadata ? vs.uid : ProcessReportId(ProcessCurrent(),false,vs.uid);
    st->st_gid = vs.exactDescriptorMetadata ? vs.gid : ProcessReportId(ProcessCurrent(),true,vs.gid);
    st->st_atime_sec = vs.atime.sec;
    st->st_atime_nsec = vs.atime.nsec;
    st->st_mtime_sec = vs.mtime.sec;
    st->st_mtime_nsec = vs.mtime.nsec;
    st->st_ctime_sec = vs.ctime.sec;
    st->st_ctime_nsec = vs.ctime.nsec;

    // Use real mode from VnodeStat if available, otherwise generate from flags
    if (vs.mode)
        st->st_mode = vs.mode;
    else if (vs.isSymlink)
        st->st_mode = 0120777; // S_IFLNK | rwxrwxrwx
    else if (vs.isDir)
        st->st_mode = 0040755; // S_IFDIR | rwxr-xr-x
    else
        st->st_mode = 0100755; // S_IFREG | rwxr-xr-x
}

// Busybox-aware stat: if a file doesn't exist in /boot/BIN/ but busybox does,
// return busybox's stat info. This lets bash's PATH search find busybox applets.
static bool BusyboxStatFallback(const char* path, VnodeStat* vs)
{
    // Check if path is under a bin-like directory
    const char* binPrefixes[] = {
        "/boot/BIN/", "/boot/bin/", "/bin/", "/usr/bin/",
        "/usr/local/bin/", "/sbin/", "/usr/sbin/"
    };
    bool isBinPath = false;
    for (int i = 0; i < 7; ++i)
    {
        const char* pfx = binPrefixes[i];
        const char* a = path;
        const char* b = pfx;
        while (*b && *a == *b) { a++; b++; }
        // Check prefix matches and remainder has no slashes (is a simple filename)
        if (!*b && *a)
        {
            bool hasSlash = false;
            for (const char* c = a; *c; ++c) { if (*c == '/') { hasSlash = true; break; } }
            if (!hasSlash)
            {
                isBinPath = true;
                break;
            }
        }
    }
    if (!isBinPath) return false;
    const char* applet = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/') applet = p + 1;
    if (!BusyboxHasApplet(applet)) return false;

    static const char* busyboxPaths[] = {
        "/boot/BIN/busybox", "/boot/BIN/BUSYBOX",
        "/boot/bin/busybox", "/bin/busybox"
    };
    for (int i = 0; i < 4; i++)
    {
        if (VfsStatPath(busyboxPaths[i], vs) == 0)
            return true;
    }
    return false;
}

// GL shim (M0): fill a LinuxStat for the synthetic DRM devfs/sysfs entries
// (/dev/dri[/renderD128], /sys/dev/char/226:128/device/...). Returns true if
// `path` is a DRM-shim entry (and fills *st), false otherwise. Gated on a 3D
// driver being present so we never advertise an unusable node.
static bool DrmStatFill(const char* path, LinuxStat* st)
{
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS) || defined(BROOK_SSD_INSTALLER)
    uint32_t partition=0;
    if(MaintenanceBlockPath(path,partition)) {
        Device* device=MaintenanceBlockDevice();
        if(!device)return false;
        bool valid=false,temporary=false;
        SpinLockAcquire(&maintenanceBlockLock);
        if(maintenanceBlockHolders)valid=maintenanceBlockPublished==partition+1;
        else {maintenanceBlockHolders=1;maintenanceBlockPublished=0;temporary=true;}
        SpinLockRelease(&maintenanceBlockLock);
        if(temporary) {
            uint64_t start=0,bytes=0;
            valid=MaintenanceBlockRange(device,partition,start,bytes);
            MaintenanceBlockRelease();
        }
        if(!valid)return false;
        memset(st,0,sizeof(*st));st->st_mode=0060600;st->st_nlink=1;
        st->st_ino=1+partition;st->st_rdev=0xf000+partition;st->st_blksize=DeviceBlockSize(device);
        return true;
    }
#endif
    if (const char* cpus = SysfsCpuAttribute(path, SmpGetCpuCount(), MAX_CPUS))
    {
        uint64_t len = 0; while (cpus[len]) ++len;
        memset(st, 0, sizeof(*st));
        st->st_mode = 0100444; // S_IFREG | r--r--r--
        st->st_nlink = 1;
        st->st_size = static_cast<int64_t>(len);
        st->st_blksize = 4096;
        return true;
    }
    const bool virt=brook::GpuAppGet()!=nullptr,intel=brook::BdwIntelUserNodeEnabled(),nv=brook::NvDrmNodeEnabled();
    if(!virt && !intel && !nv)return false;
    if (nv && StrEq(path,"/dev/dri/renderD130"))
    {
        memset(st, 0, sizeof(*st));
        st->st_mode = 0020666; // S_IFCHR | rw-rw-rw-
        st->st_rdev = DRM_RENDER_RDEV+2;
        st->st_nlink = 1;
        st->st_blksize = 4096;
        return true;
    }
    if ((virt && StrEq(path, "/dev/dri/renderD128")) || (intel && StrEq(path,"/dev/dri/renderD129")))
    {
        memset(st, 0, sizeof(*st));
        st->st_mode = 0020666; // S_IFCHR | rw-rw-rw-
        st->st_rdev = intel && StrEq(path,"/dev/dri/renderD129")?DRM_RENDER_RDEV+1:DRM_RENDER_RDEV;
        st->st_nlink = 1;
        st->st_blksize = 4096;
        return true;
    }
    if (StrEq(path, "/dev/dri") || (virt && DrmSysIsDir(path)) || (intel && IntelDrmSysIsDir(path)) || brook::NvDrmSysIsDir(path))
    {
        memset(st, 0, sizeof(*st));
        st->st_mode = 0040755; // S_IFDIR | rwxr-xr-x
        st->st_nlink = 1;
        st->st_blksize = 4096;
        return true;
    }
    const char* drmAttr = virt?DrmSysAttrContent(path):nullptr;
    if(!drmAttr && intel)drmAttr=IntelDrmSysAttrContent(path);
    if(!drmAttr)drmAttr=brook::NvDrmSysAttrContent(path);
    if (drmAttr)
    {
        uint64_t len = 0; while (drmAttr[len]) ++len;
        memset(st, 0, sizeof(*st));
        st->st_mode = 0100444; // S_IFREG | r--r--r--
        st->st_nlink = 1;
        st->st_size = static_cast<int64_t>(len);
        st->st_blksize = 4096;
        st->st_blocks = 1;
        return true;
    }
    return false;
}
static bool DrmAccessResult(const char* path,uint8_t mode,bool realIds,int64_t* result)
{
    LinuxStat stat{};
    if(!DrmStatFill(path,&stat))return false;
    *result=ProcessCheckFileAccess(ProcessCurrent(),stat.st_uid,stat.st_gid,stat.st_mode,mode,realIds);
    return true;
}

// Internal stat helper — takes a kernel-space path directly (no user copy).
// statAddr is the user-space address of the stat buffer.
// Fills a kernel LinuxStat; the wrapper below copies it out once, so VFS
// work never writes user memory directly.
static int64_t StatPathFill(const char* path, LinuxStat* st, int dirfd)
{

    // Resolve relative paths (including ".") against CWD
    AtLookup at;if(int error=at.Init(dirfd,path))return error;
    char resolved[512];bool hasSyntheticName=!at.SyntheticName(path,resolved,sizeof(resolved));
    const char* lookup=hasSyntheticName?resolved:"";

    // GL shim (M0): stat the synthetic DRM devfs/sysfs entries so libdrm can
    // enumerate the render node. Only when a 3D driver is present.
    if (DrmStatFill(lookup, st))
        return 0;

    VnodeStat vs{};
    int pathError=VfsStatAt(at.fs.root,at.start(),path,&vs,true);
    if (pathError < 0)
    {
        if (pathError != -ENOENT && pathError != -1) return pathError;
        // Synthetic proc/etc files — stat should succeed for these
        static const char* syntheticPaths[] = {
            "/etc/passwd", "/etc/group", "/etc/shells", "/etc/hostname",
            "/etc/nsswitch.conf", "/etc/hosts",
            "/proc/version", nullptr
        };
        bool isSynthetic = false;
        for (auto** sp = syntheticPaths; *sp; ++sp) {
            if (StrEq(lookup, *sp)) { isSynthetic = true; break; }
        }
        if (isSynthetic) {
            // Return a plausible stat for synthetic files
            __builtin_memset(st, 0, sizeof(*st));
            st->st_mode = 0100444; // regular file, read-only
            st->st_nlink = 1;
            st->st_size = StrEq(lookup, "/etc/passwd") ? sizeof(DesktopPasswd) - 1 :
                          StrEq(lookup, "/etc/group") ? sizeof(DesktopGroup) - 1 : 64;
            st->st_blksize = 4096;
            st->st_blocks = 1;
            return 0;
        }
        if (!BusyboxStatFallback(lookup, &vs))
            return -ENOENT;
    }

    FillStat(st, vs);
    return 0;
}
static int64_t do_stat_internal(const char* path, uint64_t statAddr,int dirfd=-100)
{
    if (!UserBufferWritable(statAddr, sizeof(LinuxStat))) return -EFAULT;
    LinuxStat st{};
    if (int64_t error = StatPathFill(path, &st, dirfd)) return error;
    return CopyToUser(statAddr, &st, sizeof(st)) ? 0 : -EFAULT;
}

// Internal lstat helper — takes a kernel-space path directly.
// Fills a kernel LinuxStat; the wrapper below copies it out once, so VFS
// work never writes user memory directly.
static int64_t LstatPathFill(const char* path, LinuxStat* st, int dirfd)
{

    AtLookup at;if(int error=at.Init(dirfd,path))return error;
    char resolved[512];bool hasSyntheticName=!at.SyntheticName(path,resolved,sizeof(resolved));
    const char* lookup=hasSyntheticName?resolved:"";

    // GL shim (M0): the synthetic DRM entries are not symlinks, so lstat
    // returns the same as stat (lets glibc realpath() resolve the device dir).
    if (DrmStatFill(lookup, st))
        return 0;

    VnodeStat vs{};
    int pathError=VfsStatAt(at.fs.root,at.start(),path,&vs,false);
    if (pathError < 0)
    {
        if (pathError != -ENOENT && pathError != -1) return pathError;
        // Synthetic proc/etc files
        static const char* syntheticPaths[] = {
            "/etc/passwd", "/etc/group", "/etc/shells", "/etc/hostname",
            "/etc/nsswitch.conf", "/etc/hosts",
            "/proc/version", nullptr
        };
        bool isSynthetic = false;
        for (auto** sp = syntheticPaths; *sp; ++sp) {
            if (StrEq(lookup, *sp)) { isSynthetic = true; break; }
        }
        if (isSynthetic) {
            __builtin_memset(st, 0, sizeof(*st));
            st->st_mode = 0100444;
            st->st_nlink = 1;
            st->st_size = StrEq(lookup, "/etc/passwd") ? sizeof(DesktopPasswd) - 1 :
                          StrEq(lookup, "/etc/group") ? sizeof(DesktopGroup) - 1 : 64;
            st->st_blksize = 4096;
            st->st_blocks = 1;
            return 0;
        }
        if (!BusyboxStatFallback(lookup, &vs))
            return -ENOENT;
    }

    FillStat(st, vs);
    return 0;
}
static int64_t do_lstat_internal(const char* path, uint64_t statAddr,int dirfd=-100)
{
    if (!UserBufferWritable(statAddr, sizeof(LinuxStat))) return -EFAULT;
    LinuxStat st{};
    if (int64_t error = LstatPathFill(path, &st, dirfd)) return error;
    return CopyToUser(statAddr, &st, sizeof(st)) ? 0 : -EFAULT;
}

int64_t sys_stat(uint64_t pathAddr, uint64_t statAddr, uint64_t,
                  uint64_t, uint64_t, uint64_t)
{
    PathBuffer pathBuf;
    if (int error = CopyUserPath(pathAddr, pathBuf)) return error;
    return do_stat_internal(pathBuf, statAddr);
}

int64_t sys_lstat(uint64_t pathAddr, uint64_t statAddr, uint64_t,
                   uint64_t, uint64_t, uint64_t)
{
    PathBuffer pathBuf;
    if (int error = CopyUserPath(pathAddr, pathBuf)) return error;
    return do_lstat_internal(pathBuf, statAddr);
}

static int64_t FstatKernel(uint64_t fd,LinuxStat* st)
{

    Process* proc = ProcessCurrent();
    if (!proc || fd >= MAX_FDS) return -EBADF;
    FdRef ref(proc, static_cast<int>(fd));
    FdEntry* fde = ref.get();
    if (!fde) return -EBADF;

#if defined(BROOK_AHCI_WRITE_TEST_HOOKS) || defined(BROOK_SSD_INSTALLER)
    if(fde->type==FdType::BlockDevice && fde->handle) {
        auto* block=static_cast<MaintenanceBlockFd*>(fde->handle);
        memset(st,0,sizeof(*st));st->st_mode=0060600;st->st_nlink=1;
        st->st_ino=1+block->partition;st->st_rdev=0xf000+block->partition;st->st_blksize=DeviceBlockSize(block->device);
        return 0;
    }
#endif

    if (fde->type == FdType::DevHidraw) {
        VnodeStat stat{};
        HidrawStat(static_cast<HidrawFile*>(fde->handle), stat);
        FillStat(st, stat);
        return 0;
    }
    if (IsTerminalFd(fde)) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        memset(raw, 0, sizeof(LinuxStat));
        st->st_mode = 0020666; // S_IFCHR | rw-rw-rw-
        st->st_rdev = (fde->type == FdType::Pipe || fde->type == FdType::DevTty)
            ? 0x8800 : 0x0400;
        st->st_blksize = 4096;
        return 0;
    }

    if ((fde->type == FdType::Vnode || fde->type == FdType::Path) && fde->handle) {
        auto* vn = static_cast<Vnode*>(fde->handle);
        VnodeStat vs{};
        if (VfsStat(vn, &vs) < 0) return -EBADF;
        FillStat(st, vs, vn);
        return 0;
    }

    if (fde->type == FdType::DevFramebuf) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        memset(raw, 0, sizeof(LinuxStat));
        st->st_mode = 0020666; // S_IFCHR
        st->st_rdev = 0x1D00;
        st->st_blksize = 4096;
        return 0;
    }

    if (fde->type == FdType::DevKeyboard || fde->type == FdType::DevNull ||
        fde->type == FdType::DevUrandom || fde->type == FdType::DevDsp ||
        fde->type == FdType::DevZero || fde->type == FdType::DevFull ||
        fde->type == FdType::DebugSerial || fde->type == FdType::DebugConsole) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        memset(raw, 0, sizeof(LinuxStat));
        st->st_mode = 0020666; // S_IFCHR
        st->st_rdev = (fde->type == FdType::DevNull) ? 0x0103 :
                      (fde->type == FdType::DevUrandom) ? 0x0109 :
                      (fde->type == FdType::DevZero) ? 0x0105 :
                      (fde->type == FdType::DevFull) ? 0x0107 :
                      (fde->type == FdType::DevDsp) ? 0x0E03 :
                      (fde->type == FdType::DebugSerial) ? 0x0440 : 0x0400; // 4,64 = UART
        st->st_blksize = 4096;
        return 0;
    }

    // GL shim: /dev/dri/renderD128 is a char device (libdrm fstats the open fd
    // to recover major:minor for its sysfs lookups).
    if (fde->type == FdType::DevDri || fde->type == FdType::DevIntelDri || fde->type == FdType::DevNvDri) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        memset(raw, 0, sizeof(LinuxStat));
        st->st_mode = 0020666; // S_IFCHR | rw-rw-rw-
        st->st_rdev = fde->type==FdType::DevIntelDri ? DRM_RENDER_RDEV+1 : fde->type==FdType::DevNvDri ? DRM_RENDER_RDEV+2 : DRM_RENDER_RDEV;
        st->st_nlink = 1;
        st->st_blksize = 4096;
        return 0;
    }

    if (fde->type == FdType::Pipe) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        memset(raw, 0, sizeof(LinuxStat));
        st->st_mode = 0010666; // S_IFIFO | rw-rw-rw-
        st->st_blksize = 4096;
        return 0;
    }

    if (fde->type == FdType::SyntheticMem) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        memset(raw, 0, sizeof(LinuxStat));
        // GL shim: the /dev/dri synthetic directory fd reports as a directory
        // (opendir() fstats the fd and requires S_ISDIR).
        if (fde->handle == g_drmDriDirTag) {
            st->st_mode = 0040755; // S_IFDIR | rwxr-xr-x
            st->st_blksize = 4096;
            return 0;
        }
        st->st_mode = 0100444; // S_IFREG | r--r--r--
        if (fde->handle) {
            auto* content = static_cast<const char*>(fde->handle);
            uint64_t len = 0;
            while (content[len]) len++;
            st->st_size = len;
        }
        st->st_blksize = 4096;
        return 0;
    }

    if (fde->type == FdType::Socket || fde->type==FdType::NetlinkSocket) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        memset(raw, 0, sizeof(LinuxStat));
        brook::SocketIdentity identity{};
        const int index = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
        if(fde->type==FdType::NetlinkSocket)identity=static_cast<NetlinkSocket*>(fde->handle)->identity;
        else if (!brook::SockGetIdentity(index, &identity)) return -EBADF;
        st->st_ino = identity.inode;
        st->st_uid = ProcessReportId(ProcessCurrent(), false, identity.uid);
        st->st_gid = ProcessReportId(ProcessCurrent(), true, identity.gid);
        st->st_nlink = 1;
        st->st_mode = 0140666; // S_IFSOCK | rw-rw-rw-
        st->st_blksize = 4096;
        return 0;
    }

    if (fde->type==FdType::IntelPrime && fde->handle) {
        auto* buffer=static_cast<brook::BdwGpuBuffer*>(fde->handle);
        memset(st,0,sizeof(LinuxStat));
        st->st_dev=0x42524b01; // boot-local Intel PRIME identity namespace
        st->st_ino=brook::BdwGpuBufferIdentity(buffer);
        st->st_mode=0100600;st->st_nlink=1;st->st_blksize=4096;
        st->st_size=static_cast<int64_t>(brook::BdwGpuBufferBytes(buffer));
        st->st_blocks=(st->st_size+511)/512;
        return 0;
    }

    if (fde->type == FdType::EventFd || fde->type==FdType::SyncFile || fde->type==FdType::SignalFd) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        memset(raw, 0, sizeof(LinuxStat));
        st->st_mode = 0100666; // S_IFREG | rw-rw-rw-
        st->st_blksize = 4096;
        return 0;
    }

    if (fde->type == FdType::EpollFd) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        memset(raw, 0, sizeof(LinuxStat));
        st->st_mode = 0100666;
        st->st_blksize = 4096;
        return 0;
    }

    if (fde->type == FdType::TimerFd) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        memset(raw, 0, sizeof(LinuxStat));
        st->st_mode = 0100666;
        st->st_blksize = 4096;
        return 0;
    }

    if (fde->type == FdType::MemFd && fde->handle) {
        VnodeStat metadata{};
        MemFdHandleStat(fde->handle, &metadata);
        FillStat(st, metadata);
        return 0;
    }

    if (fde->type == FdType::UnixSocket) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        memset(raw, 0, sizeof(LinuxStat));
        st->st_mode = 0140666; // S_IFSOCK | rw-rw-rw-
        st->st_blksize = 4096;
        return 0;
    }

    return -EBADF;
}

// Build descriptor metadata entirely in kernel memory; copy only after all
// backend locks/references have left their scopes. statx also uses the helper.
int64_t sys_fstat(uint64_t fd,uint64_t statAddr,uint64_t,uint64_t,uint64_t,uint64_t) {
    LinuxStat value{};
    const int64_t result=FstatKernel(fd,&value);
    if(result)return result;
    UserAccessScope access(ProcessCurrent());
    return CopyToUser(statAddr,&value,sizeof(value))?0:-EFAULT;
}

static int FillCurrentDirectoryStat(LinuxStat* result)
{
    FsSnapshot fs{};
    int error = ProcessFsSnapshot(ProcessCurrent(), &fs);
    if (error) return error;
    VnodeStat metadata{};
    error = VfsPathStat(fs.cwd, &metadata);
    FsSnapshotRelease(&fs);
    if (error) return error;
    FillStat(result, metadata);
    return 0;
}

int64_t sys_newfstatat(uint64_t dirfd, uint64_t pathAddr, uint64_t statAddr,
                        uint64_t flags, uint64_t, uint64_t)
{
    static constexpr uint64_t AT_SYMLINK_NOFOLLOW = 0x100;
    static constexpr uint64_t AT_NO_AUTOMOUNT = 0x800;
    static constexpr uint64_t AT_EMPTY_PATH = 0x1000;
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_EMPTY_PATH))
        return -EINVAL;
    PathBuffer pathBuf;
    // Linux 6.12 accepts NULL for its real-descriptor empty-path fast path.
    // NULL with AT_FDCWD still faults; an empty string can name the cwd.
    if (!pathAddr)
        return flags == AT_EMPTY_PATH && static_cast<int>(dirfd) != -100
            ? sys_fstat(dirfd, statAddr, 0, 0, 0, 0) : -EFAULT;
    if (int error = CopyUserPath(pathAddr, pathBuf)) return error;
    const char* path = pathBuf;
    if (path[0] == '\0') {
        if (!(flags & AT_EMPTY_PATH)) return -ENOENT;
        if (static_cast<int>(dirfd) == -100) {
            LinuxStat metadata;
            if (int error = FillCurrentDirectoryStat(&metadata)) return error;
            return CopyToUserBytes(statAddr, &metadata, sizeof(metadata)) == sizeof(metadata)
                ? 0 : -EFAULT;
        }
        return sys_fstat(dirfd, statAddr, 0, 0, 0, 0);
    }

    bool noFollow = (flags & AT_SYMLINK_NOFOLLOW) != 0;

    if(noFollow)return do_lstat_internal(path,statAddr,static_cast<int>(dirfd));
    return do_stat_internal(path,statAddr,static_cast<int>(dirfd));
}

// ---------------------------------------------------------------------------
// sys_statx (332) — extended stat
// ---------------------------------------------------------------------------

struct linux_statx_timestamp {
    int64_t  tv_sec;
    uint32_t tv_nsec;
    int32_t  pad;
};

struct linux_statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t pad1;
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    linux_statx_timestamp stx_atime;
    linux_statx_timestamp stx_btime;
    linux_statx_timestamp stx_ctime;
    linux_statx_timestamp stx_mtime;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint64_t stx_mnt_id;
    uint64_t pad2[13];
};

int64_t sys_statx(uint64_t dirfd, uint64_t pathAddr, uint64_t flags,
                   uint64_t mask, uint64_t bufAddr, uint64_t)
{
    static constexpr uint64_t AT_EMPTY_PATH = 0x1000;
    static constexpr uint64_t AT_NO_AUTOMOUNT = 0x800;
    static constexpr uint64_t AT_STATX_SYNC_TYPE = 0x6000;
    if ((mask & 0x80000000) || (flags & AT_STATX_SYNC_TYPE) == AT_STATX_SYNC_TYPE ||
        (flags & ~(0x100 | AT_NO_AUTOMOUNT | AT_EMPTY_PATH | AT_STATX_SYNC_TYPE)))
        return -EINVAL;
    PathBuffer pathBuf;
    if (!pathAddr) {
        if ((flags & ~(AT_NO_AUTOMOUNT | AT_STATX_SYNC_TYPE)) != AT_EMPTY_PATH ||
            static_cast<int>(dirfd) == -100)
            return -EFAULT;
        if (!pathBuf.allocate()) return -ENOMEM;
    } else if (int error = CopyUserPath(pathAddr, pathBuf)) return error;
    if (!UserBufferWritable(bufAddr, sizeof(linux_statx))) return -EFAULT;
    const char* path = pathBuf;
    // Filled in kernel memory; copyOut writes the user buffer once.
    linux_statx staged{};
    auto* stx = &staged;
    auto copyOut = [&]() -> int64_t { return CopyToUser(bufAddr, &staged, sizeof(staged)) ? 0 : -EFAULT; };

    auto fillStatxFromLinuxStat = [&](const LinuxStat& st) {
        auto* raw = reinterpret_cast<uint8_t*>(stx);
        memset(raw, 0, sizeof(linux_statx));
        stx->stx_mask = 0x7FF; // STATX_BASIC_STATS
        stx->stx_blksize = static_cast<uint32_t>(st.st_blksize ? st.st_blksize : 4096);
        stx->stx_nlink = static_cast<uint32_t>(st.st_nlink);
        stx->stx_uid = st.st_uid;
        stx->stx_gid = st.st_gid;
        stx->stx_mode = static_cast<uint16_t>(st.st_mode);
        stx->stx_ino = st.st_ino;
        stx->stx_dev_major = static_cast<uint32_t>(((st.st_dev >> 8) & 0xfff) | ((st.st_dev >> 32) & 0xfffff000));
        stx->stx_dev_minor = static_cast<uint32_t>((st.st_dev & 0xff) | ((st.st_dev >> 12) & 0xffffff00));
        stx->stx_rdev_major = static_cast<uint32_t>(((st.st_rdev >> 8) & 0xfff) | ((st.st_rdev >> 32) & 0xfffff000));
        stx->stx_rdev_minor = static_cast<uint32_t>((st.st_rdev & 0xff) | ((st.st_rdev >> 12) & 0xffffff00));
        stx->stx_size = static_cast<uint64_t>(st.st_size);
        stx->stx_blocks = static_cast<uint64_t>(st.st_blocks);
        stx->stx_atime.tv_sec = static_cast<int64_t>(st.st_atime_sec);
        stx->stx_atime.tv_nsec = static_cast<uint32_t>(st.st_atime_nsec);
        stx->stx_ctime.tv_sec = static_cast<int64_t>(st.st_ctime_sec);
        stx->stx_ctime.tv_nsec = static_cast<uint32_t>(st.st_ctime_nsec);
        stx->stx_mtime.tv_sec = static_cast<int64_t>(st.st_mtime_sec);
        stx->stx_mtime.tv_nsec = static_cast<uint32_t>(st.st_mtime_nsec);
    };

    if (path[0] == '\0')
    {
        if (!(flags & AT_EMPTY_PATH)) return -ENOENT;
        LinuxStat st;
        int64_t ret = static_cast<int>(dirfd) == -100
            ? FillCurrentDirectoryStat(&st)
            : FstatKernel(dirfd,&st);
        if (ret < 0) return ret;
        fillStatxFromLinuxStat(st);
        return copyOut();
    }

    VnodeStat st{};

    AtLookup at;if(int error=at.Init(static_cast<int>(dirfd),path))return error;
    char resolved[512];
    if(!at.SyntheticName(path,resolved,sizeof(resolved))) {
        LinuxStat dst;
        if(DrmStatFill(resolved,&dst)) {fillStatxFromLinuxStat(dst);return copyOut();}
    }
    int ret=VfsStatAt(at.fs.root,at.start(),path,&st,!(flags&0x100));
    if(ret)return ret==-1?-ENOENT:ret;

    // Use the same filesystem metadata as stat/fstatat. Inventing mode0644
    // made Git reject installed HTTPS helpers; inode1 hid file identities.
    LinuxStat dst;
    FillStat(&dst, st);
    fillStatxFromLinuxStat(dst);

    return copyOut();
}

// ---------------------------------------------------------------------------
// sys_statfs (137) / sys_fstatfs (138) — x86-64 Linux ABI.
// ---------------------------------------------------------------------------

struct LinuxStatFs {
    int64_t type, blockSize;
    uint64_t blocks, freeBlocks, availableBlocks, files, freeFiles;
    int32_t identity[2];
    int64_t maxName, fragmentSize, flags, spare[4];
};
static_assert(sizeof(LinuxStatFs) == 120, "x86-64 statfs ABI");

static int64_t CopySpaceResult(uint64_t address, const VfsSpace& source)
{
    LinuxStatFs result{};
    result.type = source.type;
    result.blockSize = source.blockSize;
    result.blocks = source.blocks;
    result.freeBlocks = source.freeBlocks;
    result.availableBlocks = source.availableBlocks;
    result.files = source.files;
    result.freeFiles = source.freeFiles;
    result.identity[0] = static_cast<int32_t>(source.identity);
    result.identity[1] = static_cast<int32_t>(source.identity >> 32);
    result.maxName = source.maxName;
    result.fragmentSize = source.fragmentSize;
    result.flags = source.flags | 0x20; // Linux ST_VALID: mount flag fields are present.
    // No filesystem lock is held while resolving/copying user memory.
    return CopyToUser(address, &result, sizeof(result)) ? 0 : -EFAULT;
}

int64_t sys_statfs(uint64_t pathAddr, uint64_t bufAddr, uint64_t,
                    uint64_t, uint64_t, uint64_t)
{
    PathBuffer path;
    int error=CopyUserPath(pathAddr, path);if(error)return error;
    AtLookup at;if((error=at.Init(-100,path)))return error;
    Vnode* vn=VfsOpenAt(at.fs.root,at.start(),path,VFS_O_METADATA,0,at.fs.mask,&error);
    if (!vn) return error ? error : -ENOENT;
    VfsSpace space{};
    error = VfsSpaceInfo(vn, &space);
    VfsClose(vn);
    return error ? error : CopySpaceResult(bufAddr, space);
}

int64_t sys_fstatfs(uint64_t fd, uint64_t bufAddr, uint64_t,
                     uint64_t, uint64_t, uint64_t)
{
    Process* process = ProcessCurrent();
    if (!process || fd >= MAX_FDS) return -EBADF;
    FdRef ref(process, static_cast<int>(fd));
    FdEntry* entry = ref.get();
    if (!entry) return -EBADF;
    if ((entry->type != FdType::Vnode && entry->type != FdType::Path) || !entry->handle) return -ENOSYS;
    VfsSpace space{};
    int error = VfsSpaceInfo(static_cast<Vnode*>(entry->handle), &space);
    return error ? error : CopySpaceResult(bufAddr, space);
}

// ---------------------------------------------------------------------------
// sys_faccessat (269) / sys_faccessat2 (439)
// ---------------------------------------------------------------------------

int64_t sys_faccessat(uint64_t dirfd, uint64_t pathAddr, uint64_t mode,
                     uint64_t, uint64_t, uint64_t)
{
    return AccessAt(static_cast<int>(dirfd), pathAddr, mode, 0);
}

int64_t sys_faccessat2(uint64_t dirfd, uint64_t pathAddr, uint64_t mode,
                      uint64_t flags, uint64_t, uint64_t)
{
    return AccessAt(static_cast<int>(dirfd), pathAddr, mode, flags);
}

} // namespace brook
