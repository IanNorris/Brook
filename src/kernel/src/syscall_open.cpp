// syscall_open.cpp -- open/openat/creat path resolution and descriptor creation.
//
// Split from syscall.cpp (see docs/agent/syscall-split-plan.md).
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "guard_checker_session.h"
#if defined(BROOK_SSD_GUARD_REPAIR)
#include "guard_repair_session.h"
#endif

#include "user_access_scope.h"
#include "desktop_identity.h"
#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_memfd.h"
#include "syscall_dsp.h"
#include "syscall_pipe.h"
#include "syscall_maintenance_block.h"
#include "syscall_drm.h"
#include "syscall_open.h"
#include "syscall_handlers.h"

namespace brook {

// ---------------------------------------------------------------------------
// sys_write (1)
// ---------------------------------------------------------------------------

// Procfs fd links name capabilities; opening their printable link text is never
// a substitute. This scope supports the current fd table only, not foreign PIDs.
static int OpenProcMemFd(Process* proc, const char* path, uint64_t flags, bool* matched)
{
    *matched = false;
    if (strncmp(path, "/proc/", 6)) return -ENOENT;
    const char* part = path + 6;
    bool self = false;
    if (!strncmp(part, "self/", 5)) { self = true; part += 5; }
    else {
        uint64_t pid = 0;
        if (*part < '0' || *part > '9') return -ENOENT;
        while (*part >= '0' && *part <= '9') {
            if (pid > 65535) { *matched = true; return -ENOENT; }
            pid = pid * 10 + *part++ - '0';
        }
        if (*part++ != '/') return -ENOENT;
        self = pid == proc->pid || pid == proc->tgid;
    }
    if (strncmp(part, "fd/", 3)) return -ENOENT;
    if (!self) { *matched=true;return -13; }
    part += 3;
    uint64_t number = 0;
    if (*part < '0' || *part > '9') return -ENOENT;
    while (*part >= '0' && *part <= '9') {
        if (number >= MAX_FDS) { *matched=true;return -ENOENT; }
        number = number * 10 + *part++ - '0';
    }
    if (number >= MAX_FDS) { *matched=true;return -ENOENT; }
    KernelOperationGuard operation(proc);
    FdRef source(proc, static_cast<int>(number));
    if (!source) { *matched=true;return -ENOENT; }
    // Only memfds belong to this explicit broker. Directory capabilities,
    // including suffix traversal, must reach retained procfs magic following.
    if (source.get()->type != FdType::MemFd || !source.get()->handle)return -95;
    *matched=true;
    if (*part) return -20;
    if ((flags & 3) == 3) return -EINVAL;
    if (flags & 0x20000) return -40; // O_NOFOLLOW on a magic symlink
    if (flags & 0x10000) return -20; // O_DIRECTORY
    if (flags & (0x200000 | 0x200 | 0x40 | 0x80)) return -95;
    return MemFdInstall(proc, static_cast<MemFdData*>(source.get()->handle), static_cast<uint32_t>(flags));
}

static int64_t OpenAtInternal(int dirfd,uint64_t pathAddr,uint64_t flags,uint64_t mode)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;

    UserAccessScope pathAccess(proc);
    char pathBuf[512];
    const char* path = nullptr;
    if (pathAddr < 0x0000800000000000ULL)
    {
        if (pathAddr < 0x1000 || !CopyUserCString(pathAddr, pathBuf, sizeof(pathBuf))) {
            Process* p = ProcessCurrent();
            SerialPrintf("sys_open: EFAULT pathAddr=0x%lx pid=%u (%s)\n",
                         pathAddr, p ? p->pid : 0, p ? p->name : "?");
            return -EFAULT;
        }
        path = pathBuf;
    }
    else if ((pathAddr >> 47) == 0x1FFFFULL)
    {
        // Internal callers such as openat() pass kernel-built absolute paths.
        path = reinterpret_cast<const char*>(pathAddr);
    }
    else
    {
        return -EFAULT;
    }

    AtLookup at;if(int error=at.Init(dirfd,path))return error;
    char resolvedPath[512];
    bool hasSyntheticName=!at.SyntheticName(path,resolvedPath,sizeof(resolvedPath));
    const char* lookupPath=hasSyntheticName?resolvedPath:path;
    const char* syntheticPath=hasSyntheticName?resolvedPath:"";

#if defined(BROOK_SSD_GUARD_REPAIR)
    if(StrEq(path,"/dev/brook-guard-data") && !hasSyntheticName)return -13;
    if(StrEq(syntheticPath,"/dev/brook-guard-data")) {
        int error=0;Vnode* node=GuardRepairOpenLinux(flags,&error);
        if(!node)return error?error:-13;
        FdEntry entry{};entry.type=FdType::Vnode;entry.handle=node;
        entry.statusFlags=static_cast<uint32_t>(flags);entry.fdFlags=(flags&0x80000)?1:0;
        int fd=FdTablePublishOwned(proc->fds,ProcessFdLock(proc),entry,0,-1,nullptr);
        if(fd<0) {VfsRelease(node);return -EMFILE;}
        return fd;
    }
#endif
    // Reserved exclusively for the boot-owned, read-only recovery checker.
    // Never fall back to a filesystem node if admission is denied.
    if(StrEq(path,"/dev/brook-guard-esp") && !hasSyntheticName)return -13;
    if(StrEq(syntheticPath,"/dev/brook-guard-esp")) {
        int error=0;
        Vnode* node=GuardCheckerOpenLinux(flags,&error);
        if(!node)return error?error:-13;
        FdEntry entry{};entry.type=FdType::Vnode;entry.handle=node;
        entry.statusFlags=static_cast<uint32_t>(flags);entry.fdFlags=(flags&0x80000)?1:0;
        int fd=FdTablePublishOwned(proc->fds,ProcessFdLock(proc),entry,0,-1,nullptr);
        if(fd<0) {VfsRelease(node);return -EMFILE;}
        return fd;
    }

#if defined(BROOK_AHCI_WRITE_TEST_HOOKS) || defined(BROOK_SSD_INSTALLER)
    uint32_t blockPartition=0;
    if(MaintenanceBlockPath(syntheticPath,blockPartition)) {
        if(proc->euid!=0)return -EPERM;
        // O_CREAT is harmless for this already-existing synthetic block node;
        // standard tools such as dd pass it when opening an output device.
        if((flags&3)==3 || (flags&(0x200|0x10000|0x200000)))return -EINVAL;
        if((flags&0xc0)==0xc0)return -EEXIST;
        Device* device=MaintenanceBlockDevice();
        if(!device || !device->ops || !device->ops->read || !device->ops->write)return -ENODEV;
        uint64_t start=0,bytes=0;
        bool first=false;
        if(!MaintenanceBlockReserve(proc,blockPartition,first,start,bytes))return -16;
        if(first && !MaintenanceBlockRange(device,blockPartition,start,bytes)) {
            MaintenanceBlockRelease();return -ENODEV;
        }
        auto* block=static_cast<MaintenanceBlockFd*>(kmalloc(sizeof(MaintenanceBlockFd)));
        if(!block){MaintenanceBlockRelease();return -ENOMEM;}
        block->device=device;block->start=start;block->bytes=bytes;block->position=0;
        block->partition=blockPartition;block->refs=1;KMutexInit(&block->lock);
        if(first) {
            SpinLockAcquire(&maintenanceBlockLock);
            maintenanceBlockStart=start;maintenanceBlockBytes=bytes;maintenanceBlockPublished=blockPartition+1;
            SpinLockRelease(&maintenanceBlockLock);
        }
        FdEntry entry{};entry.type=FdType::BlockDevice;entry.handle=block;
        entry.statusFlags=static_cast<uint32_t>(flags);entry.fdFlags=(flags&0x80000)?1:0;
        const int fd=FdTablePublishOwned(proc->fds,ProcessFdLock(proc),entry,0,-1,nullptr);
        if(fd<0){kfree(block);MaintenanceBlockRelease();return -EMFILE;}
        return fd;
    }
#endif

    if(flags&0x200000) { // O_PATH: retained metadata, never synthetic data I/O.
        const uint64_t normalized=flags&(0x200000u|0x10000u|0x20000u);
        int error=0;
        auto* node=VfsOpenAt(at.fs.root,at.start(),path,VFS_O_METADATA,0,at.fs.mask,&error,!(flags&0x20000));
        if(!node)return error?error:-ENOENT;
        if((flags&0x10000) && node->type!=VnodeType::Dir) {VfsRelease(node);return -ENOTDIR;}
        int fd=FdAlloc(proc,FdType::Path,node);
        if(fd<0) {VfsRelease(node);return -EMFILE;}
        proc->fds[fd].statusFlags=normalized;
        proc->fds[fd].fdFlags=(flags&0x80000)?1:0; // FD_CLOEXEC
        return fd;
    }

    bool procFdPath = false;
    int procFdResult = OpenProcMemFd(proc, syntheticPath, flags, &procFdPath);
    if (procFdPath) return procFdResult;

    // Only the two allowlisted synthetic devices may follow a genuine /dev
    // bind. Other synthetic services retain the general masking policy.
    static constexpr const char* canonicalDevices[]={"/dev/null","/dev/urandom"};
    for(const char* device:canonicalDevices) {
        const char* leaf=device+5;
        if(!StrEq(path,device) && !StrEq(path,leaf) && !StrEq(syntheticPath,device))continue;
        bool allowed=false;
        int error=VfsCanonicalDeviceAllowed(at.fs.root,at.start(),path,&allowed,
            !(flags&0x20000) && !((flags&0x40)&&(flags&0x80)),leaf);
        if(error)return error;
        if(allowed)syntheticPath=device;
        else if(StrEq(syntheticPath,device))syntheticPath="";
    }

    // Device paths
    if(StrEq(syntheticPath,"keyboard") || StrEq(syntheticPath,"/dev/fb0") || StrEq(syntheticPath,"/dev/keyboard") ||
       StrEq(syntheticPath,"/dev/tty") || StrEq(syntheticPath,"/dev/console") ||
       StrEq(syntheticPath,"/dev/dri/renderD128") ||
       StrEq(syntheticPath,"/dev/dri/renderD129") ||
       StrEq(syntheticPath,"/dev/random") ||
       StrEq(syntheticPath,"/dev/klog") || StrEq(syntheticPath,"/dev/dsp")) {
        const char* deviceName=StrEq(syntheticPath,"keyboard")?"/dev/keyboard":syntheticPath;
        if(int error=VfsPathDeviceActivation(at.fs.root,deviceName))return error;
    }
    if (StrEq(syntheticPath, "/dev/fb0"))
    {
        int fd = FdAlloc(proc, FdType::DevFramebuf, nullptr);
        if (fd < 0) return -EMFILE;
        DbgPrintf("sys_open: /dev/fb0 → fd %d\n", fd);
        return fd;
    }

    if (StrEq(syntheticPath, "keyboard") || StrEq(syntheticPath, "/dev/keyboard"))
    {
        int fd = FdAlloc(proc, FdType::DevKeyboard, nullptr);
        if (fd < 0) return -EMFILE;
        DbgPrintf("sys_open: keyboard → fd %d\n", fd);
        return fd;
    }

    // /dev/tty — controlling terminal
    // In WM terminal mode (stdin is a pipe), dup the stdin pipe so bash's
    // /dev/tty reads go through the terminal's input pipe instead of consuming
    // raw keyboard events from the input subsystem.
    if (StrEq(syntheticPath, "/dev/tty") || StrEq(syntheticPath, "/dev/console"))
    {
        if (proc->fds[0].type == FdType::Pipe && proc->fds[0].handle &&
            proc->fds[1].type == FdType::Pipe && proc->fds[1].handle)
        {
            // Terminal mode: /dev/tty reads from stdin pipe, writes to stdout pipe
            auto* pair = static_cast<TtyDevicePair*>(kmalloc(sizeof(TtyDevicePair)));
            if (!pair) return -ENOMEM;
            pair->readPipe = proc->fds[0].handle;
            pair->writePipe = proc->fds[1].handle;
            int fd = FdAlloc(proc, FdType::DevTty, pair);
            if (fd < 0) { kfree(pair); return -EMFILE; }
            proc->fds[fd].statusFlags = 2; // O_RDWR
            auto* rp = static_cast<PipeBuffer*>(pair->readPipe);
            auto* wp = static_cast<PipeBuffer*>(pair->writePipe);
            __atomic_fetch_add(&rp->readers, 1, __ATOMIC_RELEASE);
            __atomic_fetch_add(&wp->writers, 1, __ATOMIC_RELEASE);
            return fd;
        }
        int fd = FdAlloc(proc, FdType::DevKeyboard, nullptr);
        if (fd < 0) return -EMFILE;
        DbgPrintf("sys_open: %s → fd %d (keyboard/tty)\n", path, fd);
        return fd;
    }

    // /dev/dri/renderD128 — virtio-gpu DRM render node. A Linux-compatible
    // render node that maps the virtio-gpu DRM uABI onto Brook's native
    // per-process GPU path (GpuAppOps), so unmodified Mesa's virgl driver can
    // drive it. Only the render node (no card0/KMS) is offered. Available only
    // when a 3D driver has registered GpuAppOps. (GL shim — see
    // artifacts/gl-drm-enumeration-spike.md.)
    if(StrEq(syntheticPath,"/dev/dri/renderD129") && brook::BdwIntelUserNodeEnabled()){
        if((flags&0xc0)==0xc0)return -EEXIST;
        if((flags&3)==3 || (flags&0x10000))return -EINVAL;
        auto* file=brook::BdwGpuFileCreate();if(!file)return -ENOMEM;
        FdEntry owned{};owned.type=FdType::DevIntelDri;owned.handle=file;
        owned.statusFlags=static_cast<uint32_t>(flags)&~(0x80000u|0x40u|0x80u|0x200u);
        owned.fdFlags=(flags&0x80000u)?1:0;
        const int fd=FdTablePublishOwned(proc->fds,ProcessFdLock(proc),owned,0,-1,nullptr);
        if(fd<0){brook::BdwGpuFileUnref(file);return -EMFILE;}return fd;
    }
    if (StrEq(syntheticPath, "/dev/dri/renderD128"))
    {
        if (!brook::GpuAppGet()) return -ENODEV;
        auto* dctx = static_cast<DrmCtx*>(kmalloc(sizeof(DrmCtx)));
        if (!dctx) return -ENOMEM;
        memset(dctx, 0, sizeof(DrmCtx));
        KMutexInit(&dctx->mappingLock);
        dctx->refCount = 1;
        dctx->owner=static_cast<DrmGpuOwner*>(kmalloc(sizeof(DrmGpuOwner)));
        if (!dctx->owner) { kfree(dctx);return -ENOMEM; }
        *dctx->owner={1,0,{}};
        int fd = FdAlloc(proc, FdType::DevDri, dctx);
        if (fd < 0) { DrmOwnerUnref(dctx->owner);kfree(dctx);return -EMFILE; }
        DbgPrintf("sys_open: /dev/dri/renderD128 → fd %d\n", fd);
        return fd;
    }

    // /dev/dri directory — enumerable so libdrm's drmGetDevices2 can list the
    // render node. Allocate a synthetic-directory fd (recognised by the marker
    // handle in getdents64/fstat). Only when a 3D driver is present.
    if (StrEq(syntheticPath, "/dev/dri") && (brook::GpuAppGet() || brook::BdwIntelUserNodeEnabled()))
    {
        int fd = FdAlloc(proc, FdType::SyntheticMem,
                         const_cast<char*>(g_drmDriDirTag));
        if (fd < 0) return -EMFILE;
        proc->fds[fd].seekPos = 0;
        proc->fds[fd].dirPath[0] = '\0';
        return fd;
    }

    // DRM sysfs attribute files (/sys/dev/char/226:128/device/{uevent,vendor,…}).
    // Served as read-only synthetic memory files for libdrm PCI device parsing.
    if (brook::GpuAppGet())
    {
        const char* drmAttr = DrmSysAttrContent(syntheticPath);
        if (drmAttr)
        {
            int fd = FdAlloc(proc, FdType::SyntheticMem, const_cast<char*>(drmAttr));
            if (fd < 0) return -EMFILE;
            proc->fds[fd].seekPos = 0;
            return fd;
        }
    }
    if(brook::BdwIntelUserNodeEnabled()){
        const char* attr=IntelDrmSysAttrContent(syntheticPath);
        if(attr){int fd=FdAlloc(proc,FdType::SyntheticMem,const_cast<char*>(attr));if(fd<0)return -EMFILE;
            proc->fds[fd].seekPos=0;return fd;}
    }

    // /dev/null — discard writes, EOF on read
    if (StrEq(syntheticPath, "/dev/null"))
    {
        if((flags&0xc0)==0xc0)return -EEXIST; // O_CREAT|O_EXCL
        if(flags&0x10000)return -ENOTDIR;
        FdEntry owned{};owned.type=FdType::DevNull;
        owned.statusFlags=static_cast<uint32_t>(flags)&~(0x80000u|0x40u|0x80u|0x200u);
        owned.fdFlags=(flags&0x80000u)?1:0;
        int fd=FdTablePublishOwned(proc->fds,ProcessFdLock(proc),owned,0,-1,nullptr);
        return fd<0?-EMFILE:fd;
    }

    // /dev/urandom, /dev/random — RDRAND-backed random bytes
    if (StrEq(syntheticPath, "/dev/urandom") || StrEq(syntheticPath, "/dev/random"))
    {
        if((flags&0xc0)==0xc0)return -EEXIST;
        if(flags&0x10000)return -ENOTDIR;
        FdEntry owned{};owned.type=FdType::DevUrandom;
        owned.statusFlags=static_cast<uint32_t>(flags)&~(0x80000u|0x40u|0x80u|0x200u);
        owned.fdFlags=(flags&0x80000u)?1:0;
        int fd=FdTablePublishOwned(proc->fds,ProcessFdLock(proc),owned,0,-1,nullptr);
        return fd<0?-EMFILE:fd;
    }

    // /dev/klog — kernel log ring buffer reader
    if (StrEq(syntheticPath, "/dev/klog"))
    {
        // Allocate a cursor (uint32_t) to track read position in ring buffer.
        auto* cursor = static_cast<uint32_t*>(kmalloc(sizeof(uint32_t)));
        if (!cursor) return -ENOMEM;
        *cursor = 0; // start from beginning of ring
        int fd = FdAlloc(proc, FdType::DevKlog, cursor);
        if (fd < 0) { kfree(cursor); return -EMFILE; }
        DbgPrintf("sys_open: /dev/klog → fd %d\n", fd);
        return fd;
    }

    // /dev/dsp — OSS audio output
    if (StrEq(syntheticPath, "/dev/dsp"))
    {
        if (!AudioAvailable()) return -ENODEV;
        auto* dsp = static_cast<DspState*>(kmalloc(sizeof(DspState)));
        if (!dsp) return -ENOMEM;
        dsp->sampleRate    = DSP_DEFAULT_RATE;
        dsp->channels      = DSP_DEFAULT_CHANNELS;
        dsp->bitsPerSample = DSP_DEFAULT_BITS;
        dsp->fragmentSize  = 4096;
        dsp->ringBytes     = 0;
        dsp->bufferOffset  = 0;
        dsp->bufferSize    = DSP_BUFFER_SIZE;
        dsp->mixerStreamId = AudioMixerOpen();
        dsp->pointerEpoch = dsp->pointerBaseFrames = 0;
        dsp->controlEpoch = 1;
        if (!dsp->mixerStreamId) { kfree(dsp); return -16; } // EBUSY
        if (!AudioMixerSetCapacity(dsp->mixerStreamId, MIXER_RING_FRAMES, 4096 / MIXER_FRAME_BYTES)) {
            AudioMixerClose(dsp->mixerStreamId); kfree(dsp); return -EIO;
        }
        KMutexInit(&dsp->ioLock);
        dsp->refCount      = 1;
        dsp->buffer        = static_cast<uint8_t*>(kmalloc(DSP_BUFFER_SIZE));
        dsp->inputBuffer = static_cast<uint8_t*>(kmalloc(DSP_BUFFER_SIZE));
        if (!dsp->buffer || !dsp->inputBuffer) {
            AudioMixerClose(dsp->mixerStreamId);
            kfree(dsp->inputBuffer); kfree(dsp->buffer); kfree(dsp); return -ENOMEM;
        }
        FdEntry owned{};
        owned.type = FdType::DevDsp; owned.handle = dsp;
        owned.statusFlags = static_cast<uint32_t>(flags) & ~(0x80000u | 0x40u | 0x80u | 0x200u);
        owned.fdFlags = (flags & 0x80000u) ? 1 : 0;
        int fd = FdTablePublishOwned(proc->fds, ProcessFdLock(proc), owned, 0, -1, nullptr);
        if (fd < 0) {
            AudioMixerClose(dsp->mixerStreamId);
            kfree(dsp->inputBuffer); kfree(dsp->buffer); kfree(dsp); return -EMFILE;
        }
        return fd;
    }

    // Synthetic memory files: /etc/passwd, /etc/group, /proc/self/...
    {
        struct SyntheticFile { const char* path; const char* content; };
        static const SyntheticFile syntheticFiles[] = {
            { "/etc/passwd", DesktopPasswd },
            { "/etc/group",  DesktopGroup },
            { "/etc/shells", "/boot/BIN/BASH\n" },
            { "/etc/hostname", "brook\n" },
            { "/etc/os-release", "NAME=Brook\nID=brook\nPRETTY_NAME=Brook\n" },
            { "/etc/nsswitch.conf", "passwd: files\ngroup: files\nhosts: files dns\n" },
            { "/etc/hosts", "127.0.0.1 localhost brook\n" },
            { "/proc/version", "Linux version 6.1.0-brook (brook@brook) (gcc 14.2) #1 SMP\n" },
            { nullptr, nullptr }
        };

        for (auto* sf = syntheticFiles; sf->path; ++sf)
        {
            if (StrEq(syntheticPath, sf->path))
            {
                // Store pointer to static content in handle, seekPos=0
                int fd = FdAlloc(proc, FdType::SyntheticMem, const_cast<char*>(sf->content));
                if (fd < 0) return -EMFILE;
                proc->fds[fd].seekPos = 0;
                return fd;
            }
        }

    }

    // /proc/self/exe → return ENOENT for now (readlink handles it)
    if (StrEq(syntheticPath, "/proc/self/exe"))
        return -ENOENT;

    // /dev/shm/<name> — POSIX shared memory via memfd
    // glibc shm_open() calls open("/dev/shm/<name>", flags, mode)
    if (syntheticPath[0] == '/' &&
        syntheticPath[1] == 'd' && syntheticPath[2] == 'e' && syntheticPath[3] == 'v' &&
        syntheticPath[4] == '/' && syntheticPath[5] == 's' && syntheticPath[6] == 'h' &&
        syntheticPath[7] == 'm' && syntheticPath[8] == '/')
    {
        return NamedShmOpen(proc,syntheticPath+9,static_cast<uint32_t>(flags));
    }

    // Translate Linux open flags to VFS flags
    uint32_t vfsFlags = VFS_O_READ;
    static constexpr uint64_t LINUX_O_WRONLY = 1;
    static constexpr uint64_t LINUX_O_RDWR   = 2;
    static constexpr uint64_t LINUX_O_CREAT  = 0x40;
    static constexpr uint64_t LINUX_O_EXCL   = 0x80;
    static constexpr uint64_t LINUX_O_TRUNC  = 0x200;
    static constexpr uint64_t LINUX_O_APPEND = 0x400;
    static constexpr uint64_t LINUX_O_DIRECTORY = 0x10000;
    if (flags & LINUX_O_WRONLY || flags & LINUX_O_RDWR) vfsFlags |= VFS_O_WRITE;
    if ((flags & 3) == LINUX_O_RDWR) vfsFlags |= VFS_O_READWRITE;
    if (flags & LINUX_O_CREAT)  vfsFlags |= VFS_O_CREATE;
    if (flags & LINUX_O_EXCL)   vfsFlags |= VFS_O_EXCL;
    if (flags & LINUX_O_TRUNC)  vfsFlags |= VFS_O_TRUNC;
    if (flags & LINUX_O_APPEND) vfsFlags |= VFS_O_APPEND;

    int openError = 0;
    Vnode* vn=VfsOpenAt(at.fs.root,at.start(),path,vfsFlags,static_cast<uint16_t>(mode&07777),at.fs.mask,&openError,
                          !(flags&0x20000)); // O_NOFOLLOW
    if (!vn && openError && openError != -ENOENT) return openError;

    // Preserve /boot aliases for legacy applications, but Nix store paths
    // identify exact package contents. In particular, a missing libc.so in
    // a compiler's -L directory must not resolve to the boot disk's musl:
    // doing so links musl into otherwise glibc-based executables.
    bool nixStorePath = strncmp(lookupPath, "/nix/store/", 11) == 0;
    if (!vn && hasSyntheticName && lookupPath[0] == '/' && !nixStorePath)
    {
        // Try /boot prefix first
        char bootPath[256] = "/boot";
        uint32_t bi = 5;
        const char* p = lookupPath;
        while (*p && bi + 1 < sizeof(bootPath)) bootPath[bi++] = *p++;
        bootPath[bi] = '\0';
        if (!*p) { // Never open a silently truncated alias.
            vn=VfsOpenAt(at.fs.root,at.fs.root,bootPath,vfsFlags,static_cast<uint16_t>(mode&07777),at.fs.mask,&openError);
            if (!vn && openError && openError != -ENOENT) return openError;
        }

        if (!vn && !(flags & (LINUX_O_DIRECTORY | LINUX_O_CREAT | LINUX_O_TRUNC | LINUX_O_APPEND)) &&
            !(flags & 3))
        {
            const char* fname = lookupPath;
            for (p = lookupPath; *p; ++p)
                if (*p == '/') fname = p + 1;
            auto hasPrefix = [lookupPath](const char* prefix) {
                const char* candidate = lookupPath;
                while (*prefix) if (*candidate++ != *prefix++) return false;
                return true;
            };
            bool libraryPrefix = hasPrefix("/lib/") || hasPrefix("/lib64/") ||
                hasPrefix("/usr/lib/") || hasPrefix("/usr/lib64/");
            bool libraryName = false;
            for (const char* suffix = fname; *suffix; ++suffix) {
                if (suffix[0] == '.' && suffix[1] == 's' && suffix[2] == 'o' &&
                    (suffix[3] == '\0' || suffix[3] == '.')) {
                    const char* version = suffix + 3;
                    libraryName = true;
                    while (*version) {
                        if (*version++ != '.' || *version < '0' || *version > '9') {
                            libraryName = false; break;
                        }
                        while (*version >= '0' && *version <= '9') ++version;
                    }
                    break;
                }
            }
            // A trailing slash has no basename. In particular, a missing Git
            // refs directory must never become the /boot/lib directory.
            if (libraryPrefix && *fname && libraryName) {
                char libPath[256] = "/boot/lib/";
                uint32_t li = 10;
                while (*fname && li + 1 < sizeof(libPath)) libPath[li++] = *fname++;
                libPath[li] = '\0';
                if (!*fname) vn=VfsOpenAt(at.fs.root,at.fs.root,libPath,vfsFlags,0,at.fs.mask,&openError);
                if (vn && vn->type != VnodeType::File) {
                    VfsClose(vn);
                    vn = nullptr;
                }
            }
        }
    }

    if (!vn)
    {
        // Optional Quake pak diagnostics. Missing pak-number probes are normal
        // discovery, not kernel errors; logging thousands of them directly to
        // the UART made game startup spend substantial time with IRQs off.
        bool isPak = false;
        constexpr uint32_t fileTraceCategory = 1u << 1; // SC_FILE below
        if (proc->straceEnabled &&
            (proc->straceFilter == 0 || (proc->straceFilter & fileTraceCategory))) {
            for (const char* p = lookupPath; *p; ++p)
                if (p[0] == 'p' && p[1] == 'a' && p[2] == 'k') { isPak = true; break; }
        }
        if (isPak)
        {
            uint32_t usedFds = 0;
            for (uint32_t fi = 0; fi < MAX_FDS; fi++)
                if (proc->fds[fi].type != FdType::None) usedFds++;
            SerialPrintf("sys_open FAIL: '%s' (resolved '%s') fds=%u/%u\n",
                         path, lookupPath, usedFds, MAX_FDS);
        }
        return -ENOENT;
    }

    uint32_t lookupLength = 0;
    while (lookupPath[lookupLength]) ++lookupLength;
    if (((flags & LINUX_O_DIRECTORY) ||
         (lookupLength && lookupPath[lookupLength - 1] == '/')) && vn->type != VnodeType::Dir) {
        VfsClose(vn);
        return -ENOTDIR;
    }
    VnodeStat openedStat{};
    if(int error=VfsStat(vn,&openedStat)) {VfsClose(vn);return error;}
    if((openedStat.mode&0170000)==0010000) {
        if(int error=NamedFifoAttach(vn,static_cast<uint32_t>(flags))) {VfsClose(vn);return error;}
    }
    FdEntry openedEntry{};openedEntry.type=FdType::Vnode;openedEntry.handle=vn;
    openedEntry.statusFlags=static_cast<uint32_t>(flags);openedEntry.fdFlags=(flags&0x80000)?1:0;
    int fd=FdTablePublishOwned(proc->fds,ProcessFdLock(proc),openedEntry,0,-1,nullptr);
    if(fd<0) {VfsClose(vn);return -EMFILE;}

    // Keep a bounded diagnostic label; this is never a resolution prefix.
    if (vn->type == VnodeType::Dir)
    {
        uint32_t pi = 0;
        while (lookupPath[pi] && pi < 62) { proc->fds[fd].dirPath[pi] = lookupPath[pi]; pi++; }
        // Ensure trailing slash
        if (pi > 0 && proc->fds[fd].dirPath[pi-1] != '/' && pi < 63)
            proc->fds[fd].dirPath[pi++] = '/';
        proc->fds[fd].dirPath[pi] = '\0';
    }

    return fd;
}
int64_t sys_open(uint64_t path,uint64_t flags,uint64_t mode,uint64_t,uint64_t,uint64_t)
{
    return OpenAtInternal(-100,path,flags,mode);
}

// Linux creat is precisely open(O_WRONLY|O_CREAT|O_TRUNC), including mode,
// umask, descriptor access and the regular-file truncate/mapping safeguards.
int64_t sys_creat(uint64_t path,uint64_t mode,uint64_t,uint64_t,uint64_t,uint64_t) {
    return sys_open(path,0x1u|0x40u|0x200u,mode,0,0,0);
}

// ---------------------------------------------------------------------------
// sys_openat (257) -- delegate to sys_open
// ---------------------------------------------------------------------------

int64_t sys_openat(uint64_t dirfd,uint64_t path,uint64_t flags,uint64_t mode,uint64_t,uint64_t)
{
    return OpenAtInternal(static_cast<int>(dirfd),path,flags,mode);
}

} // namespace brook
