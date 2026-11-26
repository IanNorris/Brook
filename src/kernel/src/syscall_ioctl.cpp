// syscall_ioctl.cpp -- ioctl dispatch for terminals, framebuffer, OSS, DRM and sockets.
//
// Split from syscall.cpp.
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "pid_namespace.h"
#include "procfs_vfs.h"
#include "ahci_system_volume.h"
#include "boot_timing_runtime.h"
#include "guard_checker_runner.h"
#include "process_user_namespace.h"
#include "../../shared/brook_system_esp.h"
#include "../../shared/brook_boot_stick.h"
#include "boot_stick.h"
#include "../../shared/update/guard_slot_health.h"

#include "syscall.h"
#include "timeout_value.h"
#include "dsp_geometry.h"
#include "intel_bdw_buffer.h"
#include "tty.h"
#include "compositor.h"
#include "ahci_readonly.h"
#include "window.h"
#include "terminal.h"
#include "net.h"
#include "hda_observe.h"
#include "syscall_internal.h"
#include "syscall_dsp.h"
#include "syscall_pipe.h"
#include "syscall_unix_socket.h"
#include "syscall_maintenance_block.h"
#include "syscall_drm.h"
#include "syscall_time.h"
#include "syscall_rw.h"
#include "syscall_netlink.h"
#include "syscall_handlers.h"
#include "nv_drm.h"

namespace brook {

// ---------------------------------------------------------------------------
// sys_ioctl (16) -- framebuffer and keyboard ioctls
// ---------------------------------------------------------------------------

// Linux framebuffer ioctl commands
static constexpr uint64_t FBIOGET_VSCREENINFO = 0x4600;
static constexpr uint64_t FBIOPUT_VSCREENINFO = 0x4601;
static constexpr uint64_t FBIOGET_FSCREENINFO = 0x4602;

// Linux fb_var_screeninfo (simplified — only fields DOOM uses)
struct FbVarScreeninfo {
    uint32_t xres;
    uint32_t yres;
    uint32_t xres_virtual;
    uint32_t yres_virtual;
    uint32_t xoffset;
    uint32_t yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
    // red/green/blue/transp bitfields (4 x 3 uint32_t each = 48 bytes)
    uint32_t red_offset, red_length, red_msb_right;
    uint32_t green_offset, green_length, green_msb_right;
    uint32_t blue_offset, blue_length, blue_msb_right;
    uint32_t transp_offset, transp_length, transp_msb_right;
    // rest
    uint32_t nonstd;
    uint32_t activate;
    uint32_t height;    // mm
    uint32_t width;     // mm
    uint32_t accel_flags;
    uint32_t pixclock;
    uint32_t left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len;
    uint32_t sync;
    uint32_t vmode;
    uint32_t rotate;
    uint32_t colorspace;
    uint32_t reserved[4];
};

// Linux fb_fix_screeninfo (simplified)
struct FbFixScreeninfo {
    char     id[16];
    uint64_t smem_start;     // physical start of framebuffer memory
    uint32_t smem_len;       // length of framebuffer memory
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep;
    uint16_t ypanstep;
    uint16_t ywrapstep;
    uint32_t line_length;    // bytes per scanline
    uint64_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};

// Terminal ioctls (keyboard and terminal-pipe descriptors). Results are
// built in kernel memory and copied once with the checked user copy.
// termios (TCGETS) is 36 bytes; termios2 (TCGETS2) appends two speeds.
static int64_t CopyTermiosOut(const Process* proc, uint64_t arg, bool termios2)
{
    uint8_t out[44] = {};
    uint32_t flags[4] = {0x0500,  // c_iflag: ICRNL | IXON
                         0x0005,  // c_oflag: OPOST | ONLCR
                         0x00BF,  // c_cflag: CS8 | CREAD | HUPCL | B38400
                         0x8A31}; // c_lflag base: ECHOE|ECHOK|ISIG|IEXTEN|ECHOCTL|ECHOKE
    if (proc->ttyCanonical) flags[3] |= 0x0002; // ICANON
    if (proc->ttyEcho)      flags[3] |= 0x0008; // ECHO
    memcpy(out, flags, sizeof(flags));
    // out[16] is c_line (N_TTY); c_cc (19 bytes) starts at offset 17:
    // VINTR ^C, VQUIT ^\, VERASE DEL, VKILL ^U, VEOF ^D, VTIME 0, VMIN 1,
    // VSWTC, VSTART ^Q, VSTOP ^S, VSUSP ^Z, VEOL, VREPRINT ^R, VDISCARD ^O,
    // VWERASE ^W, VLNEXT ^V, VEOL2.
    static const uint8_t cc[17] = {0x03,0x1C,0x7F,0x15,0x04,0,1,0,0x11,0x13,0x1A,0,0x12,0x0F,0x17,0x16,0};
    memcpy(out + 17, cc, sizeof(cc));
    if (termios2) {
        const uint32_t speed = 38400; // c_ispeed, c_ospeed
        memcpy(out + 36, &speed, sizeof(speed));
        memcpy(out + 40, &speed, sizeof(speed));
    }
    return CopyToUser(arg, out, termios2 ? 44 : 36) ? 0 : -EFAULT;
}

// TCSETS/TCSETSW/TCSETSF: track ICANON and ECHO.
static int64_t SetTermiosFrom(uint64_t arg)
{
    uint32_t flags[4];
    if (!CopyFromUser(flags, arg, sizeof(flags))) return -EFAULT;
    if (Process* cur = ProcessCurrent()) {
        cur->ttyCanonical = (flags[3] & 0x0002) != 0; // ICANON
        cur->ttyEcho      = (flags[3] & 0x0008) != 0; // ECHO
    }
    return 0;
}

static int64_t CopyPgrpOut(uint64_t arg)
{
    int pgrp = 1;
    if (Process* cur = ProcessCurrent()) {
        Terminal* term = TerminalFindByProcess(cur);
        const auto* view = ProcessPidView(cur);
        pgrp = term ? static_cast<int>(SchedulerTranslateProcessGroup(cur, term->foregroundPgid, false))
                    : static_cast<int>(ProcessVisibleGroup(cur, view, false));
    }
    return CopyToUser(arg, &pgrp, sizeof(pgrp)) ? 0 : -EFAULT;
}

static int64_t SetPgrpFrom(uint64_t arg)
{
    int newPgid;
    if (!CopyFromUser(&newPgid, arg, sizeof(newPgid))) return -EFAULT;
    if (Process* cur = ProcessCurrent()) {
        if (Terminal* term = TerminalFindByProcess(cur)) {
            if (ProcessPidView(cur)) {
                if (newPgid <= 0) return -EINVAL;
                const uint32_t global = SchedulerTranslateProcessGroup(cur, newPgid, true);
                if (!global) return -EPERM;
                term->foregroundPgid = static_cast<uint16_t>(global);
            } else {
                term->foregroundPgid = static_cast<uint16_t>(newPgid);
            }
            DbgPrintf("TIOCSPGRP: pid %u set fg pgid to %d\n", cur->pid, newPgid);
        }
    }
    return 0;
}

static int64_t CopyWinsizeOut(Process* proc, uint64_t arg)
{
    TerminalGeometry geometry{25,80,0,0};
    TerminalSnapshotGeometry(proc,&geometry);
    const uint16_t ws[4] = {static_cast<uint16_t>(geometry.rows), static_cast<uint16_t>(geometry.cols),
                            static_cast<uint16_t>(geometry.width), static_cast<uint16_t>(geometry.height)};
    return CopyToUser(arg, ws, sizeof(ws)) ? 0 : -EFAULT;
}

int64_t sys_ioctl(uint64_t fd, uint64_t cmd_raw, uint64_t arg,
                   uint64_t, uint64_t, uint64_t)
{
    // ioctl cmd is a 32-bit value; mask off sign-extension from syscall ABI
    uint64_t cmd = cmd_raw & 0xFFFFFFFFULL;
    Process* proc = ProcessCurrent();
    if (!proc || fd >= MAX_FDS) return -EBADF;
    FdRef ref(proc, static_cast<int>(fd));
    FdEntry* fde = ref.get();
    if (!fde || fde->type==FdType::Path) return -EBADF;

    if (cmd == BROOK_IOCTL_BOOT_STICK_ARM || cmd == BROOK_IOCTL_BOOT_STICK_DISARM ||
        cmd == BROOK_IOCTL_BOOT_STICK_STATUS || (cmd >= BROOK_IOCTL_BOOT_STICK_STORE_OPEN &&
        cmd <= BROOK_IOCTL_BOOT_STICK_STORE_CLOSE)) {
        if (!ProcessCapable(proc, nullptr, CapSysAdmin)) return -EPERM;
        return BootStickIoctl(static_cast<uint32_t>(cmd), arg,
            [](uint64_t to, const void* from, uint64_t n) { return CopyToUser(to, from, n); },
            [](void* to, uint64_t from, uint64_t n) { return CopyFromUser(to, from, n); });
    }

    if (cmd == BROOK_IOCTL_GET_SYSTEM_ESP_BOOT_SLOT) {
        static_assert(sizeof(BrookSystemEspBootSlot) == 56);
        if (!ProcessCapable(proc, nullptr, CapSysAdmin)) return -EPERM;
        if (fde->type != FdType::Vnode || !fde->handle) return -25;
        auto* node = static_cast<Vnode*>(fde->handle);
        if (node->type != VnodeType::Dir) return -25;
        if (!AhciSystemVolumeOwnsRoot(node)) return -ENODEV;
        BrookSystemEspBootSlot result{};
        result.version = BROOK_ESP_BOOT_SLOT_VERSION;
        const auto slot = BootTimingHandoffSlot();
        if (GuardSlotKeyValid(slot)) {
            result.mode = BROOK_ESP_BOOT_SLOT_VERIFIED;
            result.generation = slot.generation;
            result.slot = slot.slot;
            __builtin_memcpy(result.selector_digest, slot.selectorDigest, sizeof(result.selector_digest));
        } else if (BootTimingHandoffFlags() & BootTimingRecovery) {
            result.mode = BROOK_ESP_BOOT_SLOT_RECOVERY;
        }
        return CopyToUser(arg, &result, sizeof(result)) ? 0 : -EFAULT;
    }

    if (cmd == BROOK_IOCTL_CHECK_SYSTEM_ESP_GUARD_IDENTITY) {
        if (!ProcessCapable(proc, nullptr, CapSysAdmin)) return -EPERM;
        if (fde->type != FdType::Vnode || !fde->handle) return -25; // ENOTTY
        auto* node = static_cast<Vnode*>(fde->handle);
        if (node->type != VnodeType::Dir) return -25;
        if (!AhciSystemVolumeOwnsRoot(node)) return -ENODEV;
#if defined(BROOK_SSD_GUARDED_DESKTOP) || defined(BROOK_HOST_TEST)
        uint8_t digest[32];
        if (!CopyFromUser(digest, arg, sizeof(digest))) return -EFAULT;
        return GuardedDesktopCheckGuardIdentity(digest);
#else
        return -25; // The verified ESP has no guarded desktop admission context.
#endif
    }

    if(cmd==BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT || cmd==BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER) {
        if(fde->type!=FdType::Vnode || !fde->handle)return -25; // ENOTTY
        auto* node=static_cast<Vnode*>(fde->handle);
        if(node->type!=VnodeType::Dir)return -25; // ENOTTY
        return (cmd==BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT ? AhciSystemVolumeOwnsRoot(node) :
                AhciSystemVolumeOwnsMember(node))?0:-ENODEV;
    }

#if defined(BROOK_AHCI_WRITE_TEST_HOOKS) || defined(BROOK_SSD_INSTALLER)
    if(fde->type==FdType::BlockDevice && fde->handle) {
        auto* block=static_cast<MaintenanceBlockFd*>(fde->handle);
        if(cmd==0x80081272 || cmd==0x1260) {
            const uint64_t value=cmd==0x1260?block->bytes/512:block->bytes;
            return CopyToUser(arg,&value,sizeof(value))?0:-EFAULT;
        }
        if(cmd==0x125e || cmd==0x1268 || cmd==0x127b) {
            const uint32_t value=cmd==0x125e?0:cmd==0x127b?
                AhciReadOnlyGetStatus().physicalSectorBytes:DeviceBlockSize(block->device);
            return CopyToUser(arg,&value,sizeof(value))?0:-EFAULT;
        }
        return -25;
    }
#endif

    if((fde->type==FdType::Socket || fde->type==FdType::UnixSocket || fde->type==FdType::NetlinkSocket) &&
       ((cmd>=0x8910 && cmd<=0x891b) || cmd==0x8921 || cmd==0x8927 || cmd==0x8933)) {
        if(!fde->handle)return -EBADF;
        uint64_t scope=0;
        switch(fde->type) {
        case FdType::Socket:
            scope=SockNetworkScope(static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle))-1);break;
        case FdType::UnixSocket:
            scope=static_cast<UnixSocketData*>(fde->handle)->networkScope;break;
        case FdType::NetlinkSocket:
            scope=static_cast<NetlinkSocket*>(fde->handle)->networkScope;break;
        default:break;
        }
        return NetlinkInterfaceIoctl(cmd,arg,scope);
    }
    if(fde->type==FdType::Vnode && fde->handle && static_cast<Vnode*>(fde->handle)->type==VnodeType::Namespace)
        return cmd==0xb703?ProcFsOpenedNamespaceType(static_cast<Vnode*>(fde->handle)):-25;
    if(fde->type==FdType::SyncFile) {
        if(!fde->handle)return -EBADF;
        if(cmd!=SYNC_FILE_INFO_IOCTL)return -25;
        SyncFileInfo request{};
        if(!CopyFromUser(&request,arg,sizeof(request)))return -EFAULT;
        if(!SyncFileInfoRequestValid(request))return -EINVAL;
        if(!UserBufferWritable(arg,sizeof(request)))return -EFAULT;
        SyncFileInfo info{};SyncFenceInfo detail{};
        SyncFileDescribe(*static_cast<SyncFileData*>(fde->handle),info,detail);
        info.fences=request.fences;
        if(request.numFences && !CopyToUser(request.fences,&detail,sizeof(detail)))return -EFAULT;
        return CopyToUser(arg,&info,sizeof(info))?0:-EFAULT;
    }
    // libsync uses this query to distinguish a sync_file from an arbitrary fd.
    if(cmd==SYNC_FILE_INFO_IOCTL)return -25;

#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
    if(fde->type==FdType::IntelPrime && cmd==0x40086203) { // DMA_BUF_IOCTL_IMPORT_SYNC_FILE
        if(!fde->handle)return -EBADF;
        auto* target=static_cast<BdwGpuBuffer*>(fde->handle);
        if(!BdwGpuBufferAllowsUserMapping(target))return -13; // EACCES
        struct ImportFence {uint32_t flags;int32_t fd;} request{};
        if(!CopyFromUser(&request,arg,sizeof(request)))return -EFAULT;
        if(!request.flags || (request.flags&~3u) || request.fd<0)return -EINVAL;
        KernelOperationGuard operation(proc);
        FdRef source(proc,request.fd);
        if(!source || source->type!=FdType::SyncFile || !source->handle)return -EINVAL;
        auto* file=static_cast<const SyncFileData*>(source->handle);
        // Consume the immutable dependency synchronously. No descriptor or
        // reservation lock is held while waiting. This compatibility path
        // never reports success for an outstanding writer or reader.
        const uint64_t deadline=TimeoutAddSafe(ClockNowNs(1),5000000000ULL);
        for(;;) {
            const uint32_t state=SyncFileState(*file);
            if(state==1)return 0;
            if(state==2)return -EIO;
            if(proc->terminalPending || HasPendingSignals())return -EINTR;
            if(ClockNowNs(1)>=deadline)return -ETIMEDOUT;
            proc->wakeupTick=ApicTickCount()+1;
            SchedulerBlockInterruptible(proc);
        }
    }
#endif

    if(fde->type==FdType::DrmPrime) {
        if(!fde->handle)return -EBADF;
        struct FenceIoctl {uint32_t flags;int32_t fd;};
        static_assert(sizeof(FenceIoctl)==8);
        constexpr uint32_t exportFence=0xc0086202,importFence=0x40086203;
        if(cmd!=exportFence && cmd!=importFence)return -25;
        FenceIoctl request{};
        if(!CopyFromUser(&request,arg,sizeof(request)))return -EFAULT;
        if(!request.flags || (request.flags&~3u))return -EINVAL;
        auto* resource=static_cast<DrmPrimeObj*>(fde->handle)->resource;
        if(cmd==importFence) {
            if(request.fd<0)return -EINVAL;
            FdRef source(proc,request.fd);
            if(!source || source->type!=FdType::SyncFile || !source->handle)return -EINVAL;
            // Virtgpu currently imports completed fences only; a live Intel
            // fence cannot be silently treated as already synchronized.
            const uint32_t sourceState=SyncFileState(*static_cast<SyncFileData*>(source->handle));
            if(sourceState!=1)return sourceState==2?-EIO:-EAGAIN;
            KMutexLock(&resource->reservationLock);
            const bool accepted=resource->reservation.ImportCompleted(request.flags);
            KMutexUnlock(&resource->reservationLock);
            return accepted?0:-EIO;
        }
        if(!UserBufferWritable(arg,sizeof(request)))return -EFAULT;
        if(resource->owner->config.capsetId!=4) {
            const auto* ga=GpuAppGet();
            if(!ga || !ga->PresentBarrier || ga->PresentBarrier(resource->owner->ctxId)<0)return -EIO;
        }
        // This implementation exports completed snapshots. Wait for admitted
        // BO submissions rather than dropping their dependencies from the fd.
        const uint64_t deadline=ClockNowNs(1)+5000000000ull;
        for(;;) {
            KMutexLock(&resource->reservationLock);
            const bool uncertain=resource->reservation.uncertain;
            const bool pending=resource->reservation.pending!=0 || resource->reservation.capturing;
            KMutexUnlock(&resource->reservationLock);
            if(uncertain)return -EIO;
            if(!pending)break;
            if(HasPendingSignals())return -EINTR;
            if(ClockNowNs(1)>=deadline)return -110; // ETIMEDOUT
            proc->wakeupTick=ApicTickCount()+1;
            SchedulerBlockInterruptible(proc);
        }
        const int descriptor=CreateCompletedSyncFile(proc);
        if(descriptor<0)return descriptor;
        SyncFilePublicationPin publication(proc,descriptor);
        request.fd=descriptor;
        if(!CopyToUser(arg,&request,sizeof(request))) {CloseProcessFd(proc,descriptor);return -EFAULT;}
        return 0;
    }

    // FIONBIO / FIONREAD are socket ioctls, but their Linux request numbers
    // live in the same 0x54xx range as terminal ioctls.
    if (cmd == 0x5421) // FIONBIO
    {
        int requested = 0;
        if (!CopyFromUser(&requested, arg, sizeof(requested))) return -EFAULT;
        bool nonblock = requested != 0;
        if (fde->type == FdType::Socket || fde->type == FdType::NetlinkSocket || fde->type == FdType::UnixSocket ||
            FdPipe(fde) || fde->type == FdType::DevTty || fde->type == FdType::DevDsp) {
            // O_NONBLOCK of the shared open file description (H-20).
            OpenFileUpdateStatus(fde->file, 0x800u, nonblock ? 0x800u : 0u);
            if(fde->type==FdType::NetlinkSocket && fde->handle)
                __atomic_store_n(&static_cast<NetlinkSocket*>(fde->handle)->nonblock,nonblock,__ATOMIC_RELEASE);
            if (fde->type == FdType::UnixSocket && fde->handle)
                static_cast<UnixSocketData*>(fde->handle)->nonblock = nonblock;
            if (fde->type == FdType::Socket && fde->handle)
                brook::SockSetNonblock(static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1, nonblock);
            return 0;
        }
        return -25; // ENOTTY
    }

    if (cmd == 0x541B) // FIONREAD
    {
        if (!UserBufferWritable(arg, sizeof(int))) return -EFAULT;
        int available = 0;
        if (auto* pipe=FdPipe(fde)) {
            available = static_cast<int>(pipe->count());
        } else if (fde->type == FdType::UnixSocket && fde->handle) {
            auto* usd = static_cast<UnixSocketData*>(fde->handle);
            if (usd->state != UnixSocketData::State::Connected || !usd->rxPipe)
                return -ENOTCONN;
            available = static_cast<int>(usd->rxPipe->count());
        } else if(fde->type==FdType::NetlinkSocket && fde->handle) {
            available=static_cast<int>(NetlinkAvailable(static_cast<NetlinkSocket*>(fde->handle)));
        } else if (fde->type == FdType::Socket && fde->handle) {
            available = 0;
        } else {
            return -25; // ENOTTY
        }
        return CopyToUser(arg, &available, sizeof(available)) ? 0 : -EFAULT;
    }

    if(fde->type==FdType::DevIntelDri)return IntelRenderIoctl(static_cast<brook::BdwGpuFile*>(fde->handle),cmd,arg,fd);
    if(fde->type==FdType::DevNvDri)return brook::NvDrmIoctl(static_cast<brook::NvDrmFile*>(fde->handle),cmd,arg);
    if (fde->type == FdType::DevDri)
    {
        uint8_t type = static_cast<uint8_t>((cmd >> 8) & 0xFF);
        uint8_t nr   = static_cast<uint8_t>(cmd & 0xFF);
        if (type != DRM_IOCTL_TYPE) return -25; // ENOTTY
        if (!fde->handle) return -EBADF;
        return DrmRenderIoctl(static_cast<DrmCtx*>(fde->handle), nr, arg);
    }

    if (fde->type == FdType::DevFramebuf)
    {
        uint64_t physBase;
        uint32_t fbW, fbH, fbStride;
        if (!TtyGetFramebufferPhys(&physBase, &fbW, &fbH, &fbStride))
            return -ENODEV;

        // Auto-create a VFB + WM window when a process without one queries
        // the framebuffer in WM mode. This handles programs launched from
        // bash that open /dev/fb0 (e.g. DOOM, Quake 2) — they need a composited VFB.
        // Use a default window size of 640×480 rather than the full physical FB,
        // so the window fits on screen with WM decorations.
        if (!proc->fbVirtual && WmIsActive() &&
            (cmd == FBIOGET_VSCREENINFO || cmd == FBIOGET_FSCREENINFO))
        {
            static uint32_t s_autoWinCount = 0;
            int16_t winX = static_cast<int16_t>(60 + (s_autoWinCount % 6) * 40);
            int16_t winY = static_cast<int16_t>(60 + (s_autoWinCount % 6) * 40);

            uint32_t autoW = 640;
            uint32_t autoH = 480;
            if (autoW > fbW) autoW = fbW;
            if (autoH > fbH) autoH = fbH;

            CompositorSetupProcess(proc,
                                   winX + static_cast<int16_t>(WmDesktopMetrics().clientOffsetX()),
                                   winY + static_cast<int16_t>(WmDesktopMetrics().clientOffsetY()),
                                   autoW, autoH, 1);

            WmCreateWindow(proc, winX, winY,
                          static_cast<uint16_t>(autoW),
                          static_cast<uint16_t>(autoH),
                          proc->name);

            s_autoWinCount++;
            SerialPrintf("sys_ioctl: auto-created WM window for pid %u (%ux%u)\n",
                         proc->pid, autoW, autoH);
        }
        uint32_t repW = (proc->fbVfbWidth  > 0) ? proc->fbVfbWidth  : fbW;
        uint32_t repH = (proc->fbVfbHeight > 0) ? proc->fbVfbHeight : fbH;
        uint32_t repStride = repW * 4; // bytes per line

        if (cmd == FBIOGET_VSCREENINFO)
        {
            FbVarScreeninfo staged{};
            auto* info = &staged;

            info->xres = repW;
            info->yres = repH;
            info->xres_virtual = repW;
            info->yres_virtual = repH;
            info->bits_per_pixel = 32;
            // BGRA pixel format (common UEFI framebuffer)
            info->blue_offset  = 0;  info->blue_length  = 8;
            info->green_offset = 8;  info->green_length = 8;
            info->red_offset   = 16; info->red_length   = 8;
            info->transp_offset = 24; info->transp_length = 8;
            return CopyToUser(arg, &staged, sizeof(staged)) ? 0 : -EFAULT;
        }

        if (cmd == FBIOGET_FSCREENINFO)
        {
            FbFixScreeninfo staged{};
            auto* info = &staged;

            const char* name = "brook_fb";
            for (int i = 0; name[i] && i < 15; ++i) info->id[i] = name[i];

            info->smem_start  = physBase;
            info->smem_len    = repStride * repH;
            info->type        = 0; // FB_TYPE_PACKED_PIXELS
            info->visual      = 2; // FB_VISUAL_TRUECOLOR
            info->line_length = repStride;
            info->mmio_start  = physBase;
            info->mmio_len    = repStride * repH;
            return CopyToUser(arg, &staged, sizeof(staged)) ? 0 : -EFAULT;
        }

        if (cmd == FBIOPUT_VSCREENINFO)
        {
            // Honor requested xres/yres: resize the process VFB and its
            // associated WM window so the renderer blits 1:1.
            FbVarScreeninfo requested{};
            if (!CopyFromUser(&requested, arg, sizeof(requested))) return -EFAULT;
            const auto* info = &requested;
            uint32_t newW = info->xres;
            uint32_t newH = info->yres;
            if (newW == 0 || newH == 0) return -EINVAL;
            if (!proc->fbVirtual) return 0; // no VFB yet — nothing to resize

            if (newW > fbW) newW = fbW;
            if (newH > fbH) newH = fbH;

            if (!CompositorResizeVfb(proc, newW, newH))
                return -ENOMEM;

            if (WmIsActive())
            {
                int widx = WmFindWindowForProcess(proc);
                if (widx >= 0)
                    WmResizeWindow(widx,
                                   static_cast<uint16_t>(newW),
                                   static_cast<uint16_t>(newH));
            }

            SerialPrintf("sys_ioctl: FBIOPUT_VSCREENINFO pid %u → %ux%u\n",
                         proc->pid, newW, newH);
            return 0;
        }

        SerialPrintf("sys_ioctl: fb unknown cmd 0x%lx\n", cmd);
        return -EINVAL;
    }

    if (fde->type == FdType::DevKeyboard)
    {
        // Custom ioctl 1 = enter non-blocking mode (Enkel compat)
        if (cmd == 1)
        {
            fde->flags |= 1; // mark as non-blocking
            return 0;
        }

        // TCGETS — return current termios state
        if (cmd == 0x5401)
        {
            return CopyTermiosOut(proc, arg, false);
        }

        // TCSETS/TCSETSW/TCSETSF — track ICANON and ECHO flags
        if (cmd >= 0x5402 && cmd <= 0x5404)
        {
            return SetTermiosFrom(arg);
        }

        // TIOCGPGRP
        if (cmd == 0x540F)
        {
            return CopyPgrpOut(arg);
        }

        // TIOCSPGRP
        if (cmd == 0x5410)
        {
            return SetPgrpFrom(arg);
        }

        // TIOCSCTTY
        if (cmd == 0x540E)
            return 0;

        // TIOCGWINSZ
        if (cmd == 0x5413)
        {
            return CopyWinsizeOut(proc, arg);
        }

        return 0;
    }

    // tcgetattr/tcsetattr arrive as ioctl on stdin (fd 0)
    // TCGETS = 0x5401, TCSETS/TCSETSW/TCSETSF = 0x5402-0x5404
    // A pipe is a TTY only if its FdEntry has flags bit 0x04 set (marked by
    // the terminal when setting up bash stdio). That way the terminal's
    // shell sees its stdio as a TTY (for readline, job control, etc.) but
    // anonymous pipes created by pipe() — e.g. the curl | xz | nar-unpack
    // pipeline — are not TTYs and isatty() returns false on them.
    bool isTtyFd = IsTerminalFd(fde);
    if (isTtyFd && cmd == 0x5401)
    {
        return CopyTermiosOut(proc, arg, false);
    }
    if (isTtyFd && cmd >= 0x5402 && cmd <= 0x5404)
    {
        return SetTermiosFrom(arg);
    }

    // TIOCGPGRP = 0x540F — get foreground process group
    if (isTtyFd && cmd == 0x540F)
    {
        return CopyPgrpOut(arg);
    }

    // TIOCSPGRP = 0x5410 — set foreground process group
    if (isTtyFd && cmd == 0x5410)
    {
        return SetPgrpFrom(arg);
    }

    // TIOCSCTTY = 0x540E — set controlling terminal
    if (isTtyFd && cmd == 0x540E)
        return 0;

    // TIOCGWINSZ = 0x5413 — terminal window size
    if (isTtyFd && cmd == 0x5413)
    {
        return CopyWinsizeOut(proc, arg);
    }

    // TIOCSWINSZ = 0x5414 — set terminal window size (accept but ignore)
    if (isTtyFd && cmd == 0x5414)
        return 0;

    // TCXONC = 0x540A — flow control (XON/XOFF), accept but ignore
    if (cmd == 0x540A)
        return 0;

    // TCFLSH = 0x540B — flush input/output, accept but ignore
    if (cmd == 0x540B)
        return 0;

    // ---------------------------------------------------------------------------
    // OSS /dev/dsp ioctls
    // ---------------------------------------------------------------------------
    if (fde->type == FdType::DevDsp && fde->handle)
    {
        auto* dsp = static_cast<DspState*>(fde->handle);

        AudioMixerRefresh(); // Driver query before taking the per-open control mutex.
        if (cmd == SNDCTL_DSP_SYNC) {
            uint64_t epoch, target, controlEpoch;
            {
                DspIoGuard held(dsp);
                AudioStreamProgress progress{};
                if (!AudioMixerGetProgress(dsp->mixerStreamId, &progress)) return -EBADF;
                if (!progress.clocked) return -95; // No fictitious legacy completion.
                if (progress.discontinuity) return -32;
                epoch = progress.epoch; target = progress.submittedFrames;
                controlEpoch = dsp->controlEpoch;
                // A finite paused queue must be started even without another write.
                AudioMixerSetEnabled(dsp->mixerStreamId, true);
            }
            // Select one time domain for the whole drain. Dell's admitted HDA
            // clock is private and serialized by its driver; it is not a
            // general replacement for the KVM clock or for completion proofs.
            const bool useDellClock=HdaRuntimeDrainClockAvailable();
            const auto drainClock=[useDellClock](){return useDellClock?HdaRuntimeDrainClockNs():AudioClockNowNs();};
            uint64_t lastCompleted = 0, lastProgressTime = drainClock();
            for (;;) {
                AudioMixerRefresh(); // Never hold the open-description mutex here.
                uint64_t completed;
                {
                    DspIoGuard held(dsp);
                    AudioStreamProgress progress{};
                    if (!AudioMixerGetProgress(dsp->mixerStreamId, &progress)) return -EBADF;
                    if (progress.epoch != epoch || dsp->controlEpoch != controlEpoch)
                        return -125; // ECANCELED: reset, pause or reconfiguration.
                    if (progress.discontinuity) return -32;
                    if (progress.completedFrames >= target) return 0;
                    completed = progress.completedFrames;
                }
                if (HasReadInterruptSignal()) return -EINTR;
                const uint64_t now = drainClock();
                if (!now || !lastProgressTime || now < lastProgressTime) return -EIO;
                if (completed != lastCompleted) { lastCompleted = completed; lastProgressTime = now; }
                else if (now - lastProgressTime >= 2000000000ULL) return -EIO;
                Process* self = SchedulerCurrentProcess();
                if (self) {
                    self->wakeupTick = ApicTickCount() + 5;
                    SchedulerBlockInterruptible(self);
                }
            }
        }
        AudioStreamProgress beforeControl{};
        if (!AudioMixerGetProgress(dsp->mixerStreamId, &beforeControl)) return -EBADF;
        const uint32_t legacyDelayFrames = cmd == SNDCTL_DSP_GETODELAY && !beforeControl.clocked
            ? AudioGetPosition() / MIXER_FRAME_BYTES : 0;
        DspIoGuard held(dsp);
        AudioStreamProgress progress{};
        if (!AudioMixerGetProgress(dsp->mixerStreamId, &progress)) return -EBADF;
        if (cmd == SNDCTL_DSP_RESET) {
            return DspResetStreamLocked(dsp)?0:-EBADF;
        }
        if(cmd==SNDCTL_DSP_POST || cmd==SNDCTL_DSP_GETOSPACE || cmd==SNDCTL_DSP_GETODELAY)
            if(!DspRecoverStreamLocked(dsp,progress))return -EBADF;
        if(cmd==SNDCTL_DSP_POST)return AudioMixerSetEnabled(dsp->mixerStreamId,true)?0:-EBADF;
        if (cmd == SNDCTL_DSP_GETOPTR) {
            if (progress.discontinuity) return -32;
            // The current write-local resampler has no exact cumulative mapping
            // back to input frames. Expose a pointer only for its native rate.
            if (!progress.clocked || dsp->sampleRate != MIXER_HW_RATE) return -95;
            const bool sameEpoch = dsp->pointerEpoch == progress.epoch;
            const uint64_t base = sameEpoch ? dsp->pointerBaseFrames : 0;
            if (progress.completedFrames < base) return -32;
            const uint64_t frameBytes = dsp->channels * (dsp->bitsPerSample / 8);
            const uint64_t frames = progress.completedFrames - base;
            if (frames > ~uint64_t(0) / frameBytes) return -75; // EOVERFLOW
            const uint64_t bytes = frames * frameBytes;
            // Linux non-mmap OSS reports queued whole fragments in blocks;
            // the query-delta variant belongs to mmap, which is unsupported.
            const uint64_t blocks = uint64_t(progress.queuedFrames) * frameBytes / dsp->fragmentSize;
            struct CountInfo { int bytes, blocks, ptr; } info{};
            info.bytes = static_cast<int>(bytes & 0x7fffffffULL);
            info.blocks = static_cast<int>(blocks);
            info.ptr = static_cast<int>(bytes % (uint64_t(progress.capacityFrames) * frameBytes));
            if (!CopyToUser(arg, &info, sizeof(info))) return -EFAULT;
            dsp->pointerEpoch = progress.epoch;
            dsp->pointerBaseFrames = base;
            return 0;
        }
        if (cmd == BROOK_DSP_GETQUEUE) {
            // Exact native units only: converted OSS frame rounding cannot
            // represent reusable storage and physical completion coherently.
            if (!progress.clocked || dsp->sampleRate != MIXER_HW_RATE ||
                dsp->channels != 2 || dsp->bitsPerSample != 16) return -95;
            const auto state=AudioStreamQueueSnapshot(progress);
            return CopyToUser(arg,&state,sizeof(state))?0:-EFAULT;
        }
        if (cmd == SNDCTL_DSP_GETOSPACE) {
            struct AudioBufInfo { int fragments, fragstotal, fragsize, bytes; } info{};
            // Software plus in-flight capacity in negotiated input bytes.
            const uint64_t frameBytes = dsp->channels * (dsp->bitsPerSample / 8);
            const uint64_t capacity = static_cast<uint64_t>(progress.capacityFrames) * dsp->sampleRate / MIXER_HW_RATE * frameBytes;
            const uint64_t available = static_cast<uint64_t>(AudioMixerAvailableFrames(dsp->mixerStreamId)) * dsp->sampleRate / MIXER_HW_RATE * frameBytes;
            info.fragsize = static_cast<int>(dsp->fragmentSize);
            info.fragstotal = static_cast<int>(capacity / dsp->fragmentSize);
            info.fragments = static_cast<int>(available / dsp->fragmentSize);
            info.bytes = static_cast<int>(available);
            return CopyToUser(arg, &info, sizeof(info)) ? 0 : -EFAULT;
        }
        int value = 0;
        const bool input = cmd == SNDCTL_DSP_SPEED || cmd == SNDCTL_DSP_SETFMT ||
            cmd == SNDCTL_DSP_CHANNELS || cmd == SNDCTL_DSP_STEREO ||
            cmd == SNDCTL_DSP_SETFRAGMENT || cmd == SNDCTL_DSP_SETTRIGGER ||
            cmd == SNDCTL_DSP_LOW_WATER;
        if (input && !CopyFromUser(&value, arg, sizeof(value))) return -EFAULT;
        if (cmd == SNDCTL_DSP_SPEED) {
            if (value > 0) {
                // Report a realizable conversion rate and bounded OSS geometry.
                if (value < 4000) value = 4000;
                if (value > 192000) value = 192000;
                if (static_cast<uint32_t>(value) != dsp->sampleRate && progress.queuedFrames) return -16;
                if (static_cast<uint32_t>(value) != dsp->sampleRate) {
                    const int error = DspChangeFormat(dsp, progress, value, dsp->channels, dsp->bitsPerSample);
                    if (error) return error;
                }
            }
            value = static_cast<int>(dsp->sampleRate);
        } else if (cmd == SNDCTL_DSP_SETFMT) {
            uint8_t bits = value == AFMT_S16_LE ? 16 : value == AFMT_U8 ? 8 : dsp->bitsPerSample;
            if (bits != dsp->bitsPerSample && progress.queuedFrames) return -16;
            if (bits != dsp->bitsPerSample) {
                const int error = DspChangeFormat(dsp, progress, dsp->sampleRate, dsp->channels, bits);
                if (error) return error;
            }
            value = dsp->bitsPerSample == 16 ? AFMT_S16_LE : AFMT_U8;
        } else if (cmd == SNDCTL_DSP_CHANNELS) {
            if (value >= 1 && value <= 2) {
                if (value != dsp->channels && progress.queuedFrames) return -16;
                if (value != dsp->channels) {
                    const int error = DspChangeFormat(dsp, progress, dsp->sampleRate, value, dsp->bitsPerSample);
                    if (error) return error;
                }
            }
            value = dsp->channels;
        } else if (cmd == SNDCTL_DSP_STEREO) {
            const uint8_t channels = value ? 2 : 1;
            if (channels != dsp->channels && progress.queuedFrames) return -16;
            if (channels != dsp->channels) {
                const int error = DspChangeFormat(dsp, progress, dsp->sampleRate, channels, dsp->bitsPerSample);
                if (error) return error;
            }
            value = dsp->channels == 2;
        } else if (cmd == SNDCTL_DSP_GETODELAY) {
            if (progress.discontinuity) return -32;
            uint64_t frames = progress.queuedFrames;
            // Unclocked drivers still expose the legacy estimate; a clocked
            // endpoint charges only this description's incomplete contribution.
            if (!progress.clocked) frames += legacyDelayFrames;
            uint64_t bytes = (frames * dsp->sampleRate + MIXER_HW_RATE - 1) / MIXER_HW_RATE * dsp->channels * (dsp->bitsPerSample / 8);
            value = static_cast<int>(bytes > 0x7fffffffULL ? 0x7fffffffULL : bytes);
        } else if (cmd == SNDCTL_DSP_GETCAPS) value = DSP_CAP_TRIGGER;
        else if (cmd == SNDCTL_DSP_GETFMTS) value = AFMT_U8 | AFMT_S16_LE;
        else if (cmd == SNDCTL_DSP_SETFRAGMENT) {
            if (progress.submittedFrames) return -16; // RESET before changing pointer geometry.
            if (dsp->sampleRate != MIXER_HW_RATE) return -95;
            const uint32_t frameBytes = dsp->channels * (dsp->bitsPerSample / 8);
            DspGeometry geometry;
            if(!DspPlanGeometry(static_cast<uint32_t>(value),frameBytes,MIXER_RING_FRAMES,
                                AudioMixerHorizonEnabled(),&geometry))return -EINVAL;
            if (!AudioMixerSetCapacity(dsp->mixerStreamId, geometry.capacityFrames, geometry.writableFrames)) return -16;
            dsp->fragmentSize = geometry.fragmentBytes; dsp->ringBytes = geometry.totalBytes;
            ++dsp->controlEpoch;
            return 0; // Actual negotiated size/count are reported by GETBLKSIZE/GETOSPACE.
        } else if (cmd == SNDCTL_DSP_LOW_WATER) {
            // This endpoint's readiness accounting is in native mixer frames.
            // Do not silently reinterpret converted-rate byte counts.
            if (!progress.clocked || dsp->sampleRate != MIXER_HW_RATE) return -95;
            const uint32_t frameBytes = dsp->channels * (dsp->bitsPerSample / 8);
            if (value <= 0 || static_cast<uint32_t>(value) % frameBytes ||
                static_cast<uint32_t>(value) / frameBytes > progress.capacityFrames) return -EINVAL;
            if (!AudioMixerSetWritableFrames(dsp->mixerStreamId, static_cast<uint32_t>(value) / frameBytes)) return -EBADF;
            return 0; // _IOW: no output copy and no queue/pointer reset.
        } else if (cmd == SNDCTL_DSP_GETBLKSIZE) value = static_cast<int>(dsp->fragmentSize);
        else if (cmd == SNDCTL_DSP_SETTRIGGER) {
            if (value & ~2) return -95; // EOPNOTSUPP: output only; no capture endpoint.
            if (!(value & 2)) ++dsp->controlEpoch;
            AudioMixerSetEnabled(dsp->mixerStreamId, (value & 2) != 0);
            return 0;
        } else {
            SerialPrintf("sys_ioctl: dsp unknown cmd 0x%lx\n", cmd);
            return -EINVAL;
        }
        return CopyToUser(arg, &value, sizeof(value)) ? 0 : -EFAULT;
    }

    // TCGETS2 = 0x802c542a — termios2 (like TCGETS plus ispeed/ospeed uint32s)
    if (isTtyFd && cmd == 0x802c542aUL)
    {
        // termios2: 4 × uint32 flags + 1 c_line + 19 c_cc + 2 × uint32 speeds = 44 bytes
        return CopyTermiosOut(proc, arg, true);
    }

    // Any unhandled 'T'-type ioctl (termios family) on a non-tty or for
    // commands we don't implement -> ENOTTY is the correct POSIX reply.
    // Go runtime uses this to detect isatty; returning ENOSYS makes it loop.
    if (((cmd >> 8) & 0xFF) == 'T')
    {
        return -25; // ENOTTY
    }

    SerialPrintf("sys_ioctl: unhandled fd=%lu cmd=0x%lx\n", fd, cmd);
    return -ENOSYS;
}

} // namespace brook
