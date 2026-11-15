// syscall_ioctl.cpp -- ioctl dispatch for terminals, framebuffer, OSS, DRM and sockets.
//
// Split from syscall.cpp (see docs/agent/syscall-split-plan.md).
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "ahci_system_volume.h"
#include "../../shared/brook_system_esp.h"

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

    if((fde->type==FdType::Socket || fde->type==FdType::UnixSocket || fde->type==FdType::NetlinkSocket) && ((cmd>=0x8910 && cmd<=0x891b) || cmd==0x8921 || cmd==0x8927 || cmd==0x8933))
        return NetlinkInterfaceIoctl(cmd,arg);
    if(fde->type==FdType::Vnode && fde->handle && static_cast<Vnode*>(fde->handle)->type==VnodeType::Namespace)
        return cmd==0xb703?0x20000:-25; // NS_GET_NSTYPE; other namespace ioctls unsupported.
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
        if (!UserBufferReadable(arg, sizeof(int))) return -EFAULT;
        bool nonblock = *reinterpret_cast<int*>(arg) != 0;
        if (fde->type == FdType::Socket || fde->type == FdType::NetlinkSocket || fde->type == FdType::UnixSocket ||
            FdPipe(fde) || fde->type == FdType::DevTty || fde->type == FdType::DevDsp) {
            SpinLockAcquire(ProcessFdLock(proc));
            if (nonblock)
                fde->statusFlags |= 0x800; // O_NONBLOCK
            else
                fde->statusFlags &= ~0x800u;
            if(fde->type==FdType::NetlinkSocket && fde->handle)
                __atomic_store_n(&static_cast<NetlinkSocket*>(fde->handle)->nonblock,nonblock,__ATOMIC_RELEASE);
            if (fde->type == FdType::UnixSocket && fde->handle)
                static_cast<UnixSocketData*>(fde->handle)->nonblock = nonblock;
            if (fde->type == FdType::Socket && fde->handle)
                brook::SockSetNonblock(static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1, nonblock);
            SpinLockRelease(ProcessFdLock(proc));
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
        *reinterpret_cast<int*>(arg) = available;
        return 0;
    }

    if(fde->type==FdType::DevIntelDri)return IntelRenderIoctl(static_cast<brook::BdwGpuFile*>(fde->handle),cmd,arg,fd);
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
                                   winX + static_cast<int16_t>(WmMetric(WM_BORDER_WIDTH)),
                                   winY + static_cast<int16_t>(WmMetric(WM_TITLE_BAR_HEIGHT) + WmMetric(WM_BORDER_WIDTH)),
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
            if (!UserBufferWritable(arg, sizeof(FbVarScreeninfo))) return -EFAULT;
            auto* info = reinterpret_cast<FbVarScreeninfo*>(arg);
            auto* raw = reinterpret_cast<uint8_t*>(info);
            memset(raw, 0, sizeof(FbVarScreeninfo));

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
            return 0;
        }

        if (cmd == FBIOGET_FSCREENINFO)
        {
            if (!UserBufferWritable(arg, sizeof(FbFixScreeninfo))) return -EFAULT;
            auto* info = reinterpret_cast<FbFixScreeninfo*>(arg);
            auto* raw = reinterpret_cast<uint8_t*>(info);
            memset(raw, 0, sizeof(FbFixScreeninfo));

            const char* name = "brook_fb";
            for (int i = 0; name[i] && i < 15; ++i) info->id[i] = name[i];

            info->smem_start  = physBase;
            info->smem_len    = repStride * repH;
            info->type        = 0; // FB_TYPE_PACKED_PIXELS
            info->visual      = 2; // FB_VISUAL_TRUECOLOR
            info->line_length = repStride;
            info->mmio_start  = physBase;
            info->mmio_len    = repStride * repH;
            return 0;
        }

        if (cmd == FBIOPUT_VSCREENINFO)
        {
            // Honor requested xres/yres: resize the process VFB and its
            // associated WM window so the renderer blits 1:1.
            const auto* info = reinterpret_cast<const FbVarScreeninfo*>(arg);
            if (!UserBufferReadable(arg, sizeof(FbVarScreeninfo))) return -EFAULT;
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
            // termios struct: 4 × uint32_t + 1 byte c_line + 19 bytes c_cc = 36 bytes
            if (!UserBufferWritable(arg, 36)) return -EFAULT;
            auto* t = reinterpret_cast<uint32_t*>(arg);
            t[0] = 0x0500;   // c_iflag: ICRNL | IXON
            t[1] = 0x0005;   // c_oflag: OPOST | ONLCR
            t[2] = 0x00BF;   // c_cflag: CS8 | CREAD | HUPCL | B38400
            // Build c_lflag from actual state
            uint32_t lflag = 0x8A31; // base: ECHOE|ECHOK|ISIG|IEXTEN|ECHOCTL|ECHOKE
            if (proc->ttyCanonical) lflag |= 0x0002; // ICANON
            if (proc->ttyEcho)      lflag |= 0x0008; // ECHO
            t[3] = lflag;
            // c_line at offset 16
            auto* raw = reinterpret_cast<uint8_t*>(&t[4]);
            raw[0] = 0;  // c_line = N_TTY
            // c_cc starts at offset 17 (raw[1])
            auto* cc = &raw[1];
            __builtin_memset(cc, 0, 19); // NCCS = 19 for old termios
            cc[0]  = 0x03;  // VINTR  = Ctrl+C
            cc[1]  = 0x1C;  // VQUIT  = Ctrl+backslash
            cc[2]  = 0x7F;  // VERASE = DEL
            cc[3]  = 0x15;  // VKILL  = Ctrl+U
            cc[4]  = 0x04;  // VEOF   = Ctrl+D
            cc[5]  = 0;     // VTIME  = 0
            cc[6]  = 1;     // VMIN   = 1
            cc[7]  = 0;     // VSWTC
            cc[8]  = 0x11;  // VSTART = Ctrl+Q (XON)
            cc[9]  = 0x13;  // VSTOP  = Ctrl+S (XOFF)
            cc[10] = 0x1A;  // VSUSP  = Ctrl+Z
            cc[11] = 0;     // VEOL
            cc[12] = 0x12;  // VREPRINT = Ctrl+R
            cc[13] = 0x0F;  // VDISCARD = Ctrl+O
            cc[14] = 0x17;  // VWERASE = Ctrl+W
            cc[15] = 0x16;  // VLNEXT  = Ctrl+V
            cc[16] = 0;     // VEOL2
            return 0;
        }

        // TCSETS/TCSETSW/TCSETSF — track ICANON and ECHO flags
        if (cmd >= 0x5402 && cmd <= 0x5404)
        {
            if (!UserBufferReadable(arg, 16)) return -EFAULT;
            auto* t = reinterpret_cast<const uint32_t*>(arg);
            Process* cur = ProcessCurrent();
            if (cur)
            {
                uint32_t lflag = t[3];
                cur->ttyCanonical = (lflag & 0x0002) != 0; // ICANON
                cur->ttyEcho      = (lflag & 0x0008) != 0; // ECHO
            }
            return 0;
        }

        // TIOCGPGRP
        if (cmd == 0x540F)
        {
            if (!UserBufferWritable(arg, sizeof(int))) return -EFAULT;
            auto* pgrp = reinterpret_cast<int*>(arg);
            Process* cur = ProcessCurrent();
            if (cur)
            {
                Terminal* term = TerminalFindByProcess(cur);
                if (term)
                    *pgrp = static_cast<int>(term->foregroundPgid);
                else
                    *pgrp = static_cast<int>(cur->pgid);
            }
            else
            {
                *pgrp = 1;
            }
            return 0;
        }

        // TIOCSPGRP
        if (cmd == 0x5410)
        {
            if (!UserBufferReadable(arg, sizeof(int))) return -EFAULT;
            auto* pgrpPtr = reinterpret_cast<const int*>(arg);
            int newPgid = *pgrpPtr;
            Process* cur = ProcessCurrent();
            if (cur)
            {
                Terminal* term = TerminalFindByProcess(cur);
                if (term)
                {
                    term->foregroundPgid = static_cast<uint16_t>(newPgid);
                    DbgPrintf("TIOCSPGRP: pid %u set fg pgid to %d\n", cur->pid, newPgid);
                }
            }
            return 0;
        }

        // TIOCSCTTY
        if (cmd == 0x540E)
            return 0;

        // TIOCGWINSZ
        if (cmd == 0x5413)
        {
            if (!UserBufferWritable(arg, 4 * sizeof(uint16_t))) return -EFAULT;
            auto* ws = reinterpret_cast<uint16_t*>(arg);
            TerminalGeometry geometry{25,80,0,0};
            TerminalSnapshotGeometry(proc,&geometry);
            ws[0] = static_cast<uint16_t>(geometry.rows);
            ws[1] = static_cast<uint16_t>(geometry.cols);
            ws[2] = static_cast<uint16_t>(geometry.width);
            ws[3] = static_cast<uint16_t>(geometry.height);
            return 0;
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
        if (!UserBufferWritable(arg, 36)) return -EFAULT;
        auto* t = reinterpret_cast<uint32_t*>(arg);
        t[0] = 0x0500;   // c_iflag: ICRNL | IXON
        t[1] = 0x0005;   // c_oflag: OPOST | ONLCR
        t[2] = 0x00BF;   // c_cflag: CS8 | CREAD | HUPCL | B38400
        uint32_t lflag = 0x8A31; // base: ECHOE|ECHOK|ISIG|IEXTEN|ECHOCTL|ECHOKE
        if (proc->ttyCanonical) lflag |= 0x0002; // ICANON
        if (proc->ttyEcho)      lflag |= 0x0008; // ECHO
        t[3] = lflag;
        auto* raw = reinterpret_cast<uint8_t*>(&t[4]);
        raw[0] = 0;  // c_line = N_TTY
        auto* cc = &raw[1];
        __builtin_memset(cc, 0, 19);
        cc[0]  = 0x03;  // VINTR  = Ctrl+C
        cc[1]  = 0x1C;  // VQUIT  = Ctrl+backslash
        cc[2]  = 0x7F;  // VERASE = DEL
        cc[3]  = 0x15;  // VKILL  = Ctrl+U
        cc[4]  = 0x04;  // VEOF   = Ctrl+D
        cc[5]  = 0;     // VTIME  = 0
        cc[6]  = 1;     // VMIN   = 1
        cc[7]  = 0;     // VSWTC
        cc[8]  = 0x11;  // VSTART = Ctrl+Q
        cc[9]  = 0x13;  // VSTOP  = Ctrl+S
        cc[10] = 0x1A;  // VSUSP  = Ctrl+Z
        cc[11] = 0;     // VEOL
        cc[12] = 0x12;  // VREPRINT = Ctrl+R
        cc[13] = 0x0F;  // VDISCARD = Ctrl+O
        cc[14] = 0x17;  // VWERASE = Ctrl+W
        cc[15] = 0x16;  // VLNEXT  = Ctrl+V
        cc[16] = 0;     // VEOL2
        return 0;
    }
    if (isTtyFd && cmd >= 0x5402 && cmd <= 0x5404)
    {
        if (!UserBufferReadable(arg, 16)) return -EFAULT;
        auto* t = reinterpret_cast<const uint32_t*>(arg);
        Process* cur = ProcessCurrent();
        if (cur)
        {
            uint32_t lflag = t[3];
            cur->ttyCanonical = (lflag & 0x0002) != 0; // ICANON
            cur->ttyEcho      = (lflag & 0x0008) != 0; // ECHO
        }
        return 0;
    }

    // TIOCGPGRP = 0x540F — get foreground process group
    if (isTtyFd && cmd == 0x540F)
    {
        if (!UserBufferWritable(arg, sizeof(int))) return -EFAULT;
        auto* pgrp = reinterpret_cast<int*>(arg);
        Process* cur = ProcessCurrent();
        if (cur)
        {
            Terminal* term = TerminalFindByProcess(cur);
            if (term)
                *pgrp = static_cast<int>(term->foregroundPgid);
            else
                *pgrp = static_cast<int>(cur->pgid);
        }
        else
        {
            *pgrp = 1;
        }
        return 0;
    }

    // TIOCSPGRP = 0x5410 — set foreground process group
    if (isTtyFd && cmd == 0x5410)
    {
        if (!UserBufferReadable(arg, sizeof(int))) return -EFAULT;
        auto* pgrpPtr = reinterpret_cast<const int*>(arg);
        int newPgid = *pgrpPtr;
        Process* cur = ProcessCurrent();
        if (cur)
        {
            Terminal* term = TerminalFindByProcess(cur);
            if (term)
            {
                term->foregroundPgid = static_cast<uint16_t>(newPgid);
                DbgPrintf("TIOCSPGRP: pid %u set fg pgid to %d\n", cur->pid, newPgid);
            }
        }
        return 0;
    }

    // TIOCSCTTY = 0x540E — set controlling terminal
    if (isTtyFd && cmd == 0x540E)
        return 0;

    // TIOCGWINSZ = 0x5413 — terminal window size
    if (isTtyFd && cmd == 0x5413)
    {
        struct winsize { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; };
        if (!UserBufferWritable(arg, sizeof(winsize))) return -EFAULT;
        auto* ws = reinterpret_cast<winsize*>(arg);
        TerminalGeometry geometry{25,80,0,0};
        TerminalSnapshotGeometry(proc,&geometry);
        ws->ws_row = static_cast<uint16_t>(geometry.rows);
        ws->ws_col = static_cast<uint16_t>(geometry.cols);
        ws->ws_xpixel = static_cast<uint16_t>(geometry.width);
        ws->ws_ypixel = static_cast<uint16_t>(geometry.height);
        return 0;
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
        if (!UserBufferWritable(arg, 44)) return -EFAULT;
        auto* t = reinterpret_cast<uint32_t*>(arg);
        t[0] = 0x0500;
        t[1] = 0x0005;
        t[2] = 0x00BF;
        uint32_t lflag = 0x8A31;
        if (proc->ttyCanonical) lflag |= 0x0002;
        if (proc->ttyEcho)      lflag |= 0x0008;
        t[3] = lflag;
        auto* raw = reinterpret_cast<uint8_t*>(&t[4]);
        raw[0] = 0;
        auto* cc = &raw[1];
        __builtin_memset(cc, 0, 19);
        cc[0]=0x03; cc[1]=0x1C; cc[2]=0x7F; cc[3]=0x15; cc[4]=0x04;
        cc[5]=0;    cc[6]=1;    cc[7]=0;    cc[8]=0x11; cc[9]=0x13;
        cc[10]=0x1A;cc[11]=0;   cc[12]=0x12;cc[13]=0x0F;cc[14]=0x17;
        cc[15]=0x16;cc[16]=0;
        // termios2 appends c_ispeed, c_ospeed (uint32 each) at offset 20 in cc-block
        uint32_t* speeds = reinterpret_cast<uint32_t*>(&raw[20]);
        speeds[0] = 38400;
        speeds[1] = 38400;
        return 0;
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
