// syscall_brook_gpu.cpp -- Brook GPU syscalls: output buffers, handoff control, window validation, per-process virgl.
//
// Split from syscall.cpp.
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "kprintf.h"

#include "syscall.h"
#include "gpu_output_abi.h"
#include "gpu_buffer_abi.h"
#include "gpu_window_source_abi.h"
#include "desktop_layers_export.h"
#include "intel_bdw_buffer.h"
#include "intel_bdw_scanout.h"
#include "intel_bdw_scanout_target.h"
#include "intel_bdw_refusal.h"
#include "compositor.h"
#include "desktop_present_policy.h"
#include "ahci_readonly.h"
#include "audio.h"
#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_unix_socket.h"
#include "syscall_drm.h"
#include "syscall_handlers.h"
#include "pipe.h"

namespace brook {

static_assert(BROOK_SYS_GPU_OUTPUT_ALLOC!=BROOK_SYS_WM_APPEARANCE &&
              BROOK_SYS_GPU_OUTPUT_ALLOC!=BROOK_SYS_WM_TASKBAR_METRICS &&
              BROOK_SYS_GPU_OUTPUT_ALLOC!=BROOK_SYS_AUDIO_CONTROL &&
              BROOK_SYS_GPU_OUTPUT_ALLOC!=BROOK_SYS_DEBUG_FRAME &&
              BROOK_SYS_GPU_OUTPUT_ALLOC!=BROOK_SYS_AHCI_EXPORT,
              "GPU output syscall must not overlap existing Brook extensions");
int64_t sys_brook_gpu_output_alloc(uint64_t bytes,uint64_t flags,uint64_t a,uint64_t b,uint64_t c,uint64_t d)
{
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
    auto* proc=ProcessCurrent();if(!proc || !proc->fds)return -ESRCH;
    KernelOperationGuard operation(proc);
    if(!bytes || bytes>BROOK_GPU_OUTPUT_MAX_BYTES || flags || a || b || c || d)return -EINVAL;
    if(!brook::BdwRenderExperimentEnabled())return -ENODEV;
    auto* buffer=brook::BdwGpuBufferCreateHandoff(bytes);if(!buffer)return -ENOMEM;
    brook::BdwGpuBufferRecordCreator(buffer,proc->tgid?proc->tgid:proc->pid,
        proc->isThread?proc->leaderIncarnation:proc->incarnation);
    FdEntry owned{};owned.type=FdType::IntelPrime;owned.handle=buffer;owned.fdFlags=1;
    brook::BdwGpuBufferTrackUserRef(buffer,brook::BdwBufferUserRef::Prime,true);
    const int fd=FdPublishNew(proc,owned,0);
    if(fd<0){brook::BdwGpuBufferTrackUserRef(buffer,brook::BdwBufferUserRef::Prime,false);brook::BdwGpuBufferUnref(buffer);}
    return fd;
#else
    (void)bytes;(void)flags;(void)a;(void)b;(void)c;(void)d;return -ENODEV;
#endif
}

static_assert(BROOK_SYS_GPU_BUFFER_TRY_CPU_READ==0xB1B && BROOK_SYS_GPU_BUFFER_TRY_CPU_READ!=BROOK_SYS_DESKTOP_OVERLAY,
              "GPU shared-buffer syscall must be distinct");
int64_t sys_brook_gpu_buffer_try_cpu_read(uint64_t fd,uint64_t flags,uint64_t a,uint64_t b,uint64_t c,uint64_t d)
{
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
    auto* proc=ProcessCurrent();if(!proc || !proc->fds)return -ESRCH;
    KernelOperationGuard operation(proc);
    if(flags || a || b || c || d)return -EINVAL;
    if(fd>=MAX_FDS)return -EBADF;
    FdRef ref(proc,static_cast<int>(fd));if(!ref)return -EBADF;
    auto* entry=ref.get();
    if(entry->type!=FdType::IntelPrime || !entry->handle)return -95; // ENOTSUP
    auto* buffer=static_cast<brook::BdwGpuBuffer*>(entry->handle);
    if(!brook::BdwGpuBufferAllowsUserMapping(buffer))return -13; // EACCES
    const auto result=brook::BdwGpuBufferTryCpuTransition(buffer);
    if(result==brook::BdwCpuTransitionResult::Ready)return 0;
    return result==brook::BdwCpuTransitionResult::Busy?-EAGAIN:-EIO;
#else
    (void)fd;(void)flags;(void)a;(void)b;(void)c;(void)d;return -ENODEV;
#endif
}

int64_t sys_brook_gpu_buffer_snapshot_fence(uint64_t fd,uint64_t flags,uint64_t a,uint64_t b,uint64_t c,uint64_t d)
{
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
    auto* proc=ProcessCurrent();if(!proc || !proc->fds)return -ESRCH;
    KernelOperationGuard operation(proc);
    if(flags || a || b || c || d)return -EINVAL;
    if(fd>=MAX_FDS)return -EBADF;
    FdRef ref(proc,static_cast<int>(fd));if(!ref)return -EBADF;
    auto* entry=ref.get();if(entry->type!=FdType::IntelPrime || !entry->handle)return -95;
    auto* buffer=static_cast<brook::BdwGpuBuffer*>(entry->handle);
    if(!brook::BdwGpuBufferAllowsUserMapping(buffer))return -13;
    brook::BdwFence* fence=nullptr;
    const auto result=brook::BdwAcquireApplicationBufferFence(buffer,&fence);if(result)return result;
    const int published=CreateIntelSyncFile(proc,fence);brook::BdwFenceUnref(fence);
    if(published<0)return published;
    SyncFilePublicationPin pin(proc,published);return published;
#else
    (void)fd;(void)flags;(void)a;(void)b;(void)c;(void)d;return -ENODEV;
#endif
}

static_assert(BROOK_SYS_GPU_HANDOFF_CTL!=BROOK_SYS_GPU_OUTPUT_ALLOC && BROOK_SYS_GPU_HANDOFF_CTL==0xB18,
              "GPU handoff syscall must be distinct");
static int64_t ValidateGpuWindowProvider(Process* proc,uint64_t fdValue,uint64_t window,uint64_t incarnation)
{
    if(fdValue>=MAX_FDS || !window || window>WM_MAX_WINDOWS || !incarnation)return -EINVAL;
    FdRef descriptor(proc,static_cast<int>(fdValue));auto* entry=descriptor.get();
    if(!entry || entry->type!=FdType::UnixSocket || !entry->handle)return -EBADF;
    auto* socket=static_cast<UnixSocketData*>(entry->handle);
    UnixCredentials peer{};uint64_t peerIncarnation=0;
    SpinLockAcquire(&g_unixRegistryLock);
    const bool accepted=socket->state==UnixSocketData::State::Connected && socket->acceptedEndpoint && socket->socketType==5;
    if(accepted){peer=socket->peerCredentials.raw;peerIncarnation=socket->peerIncarnation;}
    SpinLockRelease(&g_unixRegistryLock);
    if(!accepted || peer.pid<=0 || !peerIncarnation)return -EPERM;
    return brook::WmGpuSourceProviderMatches(static_cast<uint32_t>(window),incarnation,
                                             static_cast<uint32_t>(peer.pid),peerIncarnation)?0:-EPERM;
}

int64_t sys_brook_gpu_handoff_ctl(uint64_t op,uint64_t arg,uint64_t a,uint64_t b,uint64_t c,uint64_t d)
{
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
    auto* proc=ProcessCurrent();if(!proc || !proc->fds)return -ESRCH;
    KernelOperationGuard operation(proc);
    if((a && op!=BROOK_GPU_HANDOFF_RESERVE_DEPENDENT) || b || c || d)return -EINVAL;
    const uint64_t owner=proc->leaderIncarnation?proc->leaderIncarnation:proc->incarnation;
    if(op==BROOK_GPU_HANDOFF_PRESENT_CAPS)
        return arg?-EINVAL:(BROOK_GPU_PRESENT_CAP_FRAME|BROOK_GPU_PRESENT_CAP_ASYNC|BROOK_GPU_PRESENT_CAP_DEPENDENT
#if defined(BROOK_BDW_ZERO_COPY_SCANOUT)
            |BROOK_GPU_PRESENT_CAP_SCANOUT
#if defined(BROOK_BDW_SCANOUT_FLIP)
            |BROOK_GPU_PRESENT_CAP_CLIENT_SCANOUT
#endif
#endif
            );
#if defined(BROOK_BDW_ZERO_COPY_SCANOUT)
#if defined(BROOK_BDW_SCANOUT_FLIP)
    if(op==BROOK_GPU_HANDOFF_CLIENT_SCANOUT_PRESENT){
        if(!(DesktopPresentPolicy()&BROOK_DESKTOP_POLICY_DIRECT_ON))return -95;
        BrookGpuClientScanoutPresent wire{};
        if(!CopyFromUser(&wire,arg,sizeof(wire)))return -EFAULT;
        const BdwClientScanoutLayout layout{wire.stride,wire.tiling==1};
        if(wire.version!=BROOK_GPU_CLIENT_SCANOUT_VERSION || wire.reserved || !wire.offer ||
           wire.width!=BdwZeroCopyWidth || wire.height!=BdwZeroCopyHeight ||
           wire.fourcc!=BROOK_GPU_SCANOUT_FOURCC_XRGB8888 || wire.tiling>1 || !layout.Valid())return -EINVAL;
        if(wire.primeFd<0 || uint32_t(wire.primeFd)>=MAX_FDS ||
           wire.providerSocket<0 || uint32_t(wire.providerSocket)>=MAX_FDS)return -EBADF;
        if(!BdwOffscreenRendererOwnedBy(owner))return -EPERM;
        const auto provider=ValidateGpuWindowProvider(proc,wire.providerSocket,wire.window,wire.incarnation);
        if(provider)return provider;
        if(!WmClientScanoutWindowMatches(wire.window,wire.incarnation,wire.width,wire.height) ||
           !WmTaskbarHidden())return -EPERM;
        FdRef ref(proc,wire.primeFd);
        if(!ref || ref->type!=FdType::IntelPrime || !ref->handle)return -EBADF;
        auto* buffer=static_cast<BdwGpuBuffer*>(ref->handle);
        uint32_t tiling=0,stride=0;
        BdwGpuBufferTiling(buffer,tiling,stride);
        if(tiling!=wire.tiling || (tiling && stride!=wire.stride))return -EINVAL;
        if(!CompositorRefreshClientScanoutLease(owner))return -EAGAIN;
        return BdwClientScanoutPresent(owner,buffer,layout,wire.offer);
    }
    if(op==BROOK_GPU_HANDOFF_CLIENT_SCANOUT_STATUS){
        BrookGpuClientScanoutStatus status{};
        const auto result=BdwClientScanoutStatus(owner,status);
        return result?result:CopyToUser(arg,&status,sizeof(status))?0:-EFAULT;
    }
    if(op==BROOK_GPU_HANDOFF_CLIENT_SCANOUT_LEAVE)
        return arg?-EINVAL:BdwClientScanoutLeave(owner);
#endif
    if(op==BROOK_GPU_HANDOFF_SCANOUT_REGISTER){
        BrookGpuScanoutImages wire{};
        if(!CopyFromUser(&wire,arg,sizeof(wire)))return -EFAULT;
        if(wire.reserved || !wire.count || wire.count>BROOK_GPU_SCANOUT_MAX_IMAGES)return -EINVAL;
        static_assert(BROOK_GPU_SCANOUT_MAX_IMAGES==brook::BdwZeroCopyMaxImages,"zero-copy image limits must match");
        // Hold a buffer reference (not the fd) across registration.
        brook::BdwGpuBuffer* buffers[BROOK_GPU_SCANOUT_MAX_IMAGES]{};
        auto drop=[&](){for(auto* b:buffers)brook::BdwGpuBufferUnref(b);};
        for(uint32_t i=0;i<wire.count;++i){
            if(wire.fds[i]<0 || uint32_t(wire.fds[i])>=uint32_t(MAX_FDS)){drop();return -EBADF;}
            FdRef ref(proc,wire.fds[i]);
            if(!ref || ref->type!=FdType::IntelPrime || !ref->handle){drop();return -EBADF;}
            buffers[i]=static_cast<brook::BdwGpuBuffer*>(ref->handle);brook::BdwGpuBufferRef(buffers[i]);
        }
        brook::BdwZeroCopyRefusal refusal{};
        const brook::BdwZeroCopyGeometry geometry{wire.width,wire.height,wire.stride,wire.fourcc};
        const int64_t result=brook::BdwZeroCopyRegister(owner,buffers,wire.count,geometry,refusal);
        drop();return result;
    }
    if(op==BROOK_GPU_HANDOFF_SCANOUT_ADOPT){
        if(!UserBufferWritable(arg,sizeof(BrookGpuScanoutImages)))return -EFAULT;
        brook::BdwGpuBuffer* images[BROOK_GPU_SCANOUT_MAX_IMAGES]{};uint32_t count=0;
        const int64_t adopted=brook::BdwZeroCopyAdopt(owner,images,count);
        if(adopted)return adopted==-2?-ENOENT:adopted==-1?-EPERM:adopted;
        BrookGpuScanoutImages wire{count,brook::BdwZeroCopyWidth,brook::BdwZeroCopyHeight,brook::BdwZeroCopyStride,
            BROOK_GPU_SCANOUT_FOURCC_XRGB8888,0,{-1,-1,-1,-1}};
        int64_t result=0;
        for(uint32_t i=0;i<count;++i){
            if(result){brook::BdwGpuBufferUnref(images[i]);continue;}
            FdEntry owned{};owned.type=FdType::IntelPrime;owned.handle=images[i];owned.fdFlags=1;
            brook::BdwGpuBufferTrackUserRef(images[i],brook::BdwBufferUserRef::Prime,true);
            const int fd=FdPublishNew(proc,owned,0);
            if(fd<0){brook::BdwGpuBufferTrackUserRef(images[i],brook::BdwBufferUserRef::Prime,false);brook::BdwGpuBufferUnref(images[i]);result=fd;continue;}
            wire.fds[i]=fd; // the fd table now holds the reference
        }
        if(!result && !CopyToUser(arg,&wire,sizeof(wire)))result=-EFAULT;
        if(result)for(uint32_t i=0;i<count;++i)if(wire.fds[i]>=0)(void)CloseProcessFd(proc,wire.fds[i]);
        return result;
    }
    if(op==BROOK_GPU_HANDOFF_SCANOUT_STATUS){
        brook::BdwZeroCopyStatus s{};brook::BdwZeroCopyReadStatus(s);
        BrookGpuScanoutStatus wire{};
        wire.flags=(s.registered?BROOK_GPU_SCANOUT_REGISTERED:0u)|(s.requested?BROOK_GPU_SCANOUT_REQUESTED:0u)|
            (s.active?BROOK_GPU_SCANOUT_ACTIVE:0u)|(s.disabled?BROOK_GPU_SCANOUT_DISABLED:0u);
        wire.images=s.images;wire.freeMask=s.freeMask;wire.front=s.front;wire.armed=s.armed;wire.queued=s.queued;
        wire.refusal=uint32_t(s.refusal);wire.sequence=s.sequence;wire.presents=s.stats.presents;
        wire.latched=s.stats.latched;wire.missedVblanks=s.stats.missedVblanks;
        if(!UserBufferWritable(arg,sizeof(wire)) || !CopyToUser(arg,&wire,sizeof(wire)))return -EFAULT;
        return 0;
    }
#endif
    if(op==BROOK_GPU_HANDOFF_BUFFER_FAILED){
        if(arg>=MAX_FDS)return -EBADF;
        FdRef ref(proc,static_cast<int>(arg));
        if(!ref || ref->type!=FdType::IntelPrime || !ref->handle)return -EBADF;
        return brook::BdwGpuBufferGpuFailed(static_cast<brook::BdwGpuBuffer*>(ref->handle))?1:0;
    }
    if(op==BROOK_GPU_HANDOFF_PRESENT_RESULT)return brook::BdwOffscreenPresentResult(owner,arg);
    if(op==BROOK_GPU_HANDOFF_PRESENT_FRAME || op==BROOK_GPU_HANDOFF_QUEUE_FRAME){
        BrookGpuPresentFrame wire{};
        if(!CopyFromUser(&wire,arg,sizeof(wire)))return -EFAULT;
        if(op==BROOK_GPU_HANDOFF_QUEUE_FRAME){
            if(wire.reserved)return -EINVAL;
            return brook::BdwQueueOffscreenPresent(owner,wire.ticket,wire.width,wire.height,wire.stride,nullptr,0);
        }
        BrookGpuPresentProbe frame{wire.ticket,0,0,wire.width,wire.height,wire.stride,wire.reserved,{}};
        return brook::BdwOffscreenHandoffPresentProbe(owner,frame,true,false,false);
    }
    if(op==BROOK_GPU_HANDOFF_PRESENT_REGIONS || op==BROOK_GPU_HANDOFF_QUEUE_REGIONS){
        BrookGpuPresentRegions wire{};
        if(!CopyFromUser(&wire,arg,sizeof(wire)))return -EFAULT;
        static_assert(BROOK_GPU_PRESENT_MAX_REGIONS==brook::COMPOSITOR_MAX_REGIONS,"region limits must match");
        if(!wire.ticket || wire.reserved || !wire.count || wire.count>BROOK_GPU_PRESENT_MAX_REGIONS)return -EINVAL;
        brook::CompositorPresentRegion regions[brook::COMPOSITOR_MAX_REGIONS]{};
        for(uint32_t i=0;i<wire.count;++i){const auto& r=wire.regions[i];
            if(r.reserved)return -EINVAL;
            regions[i]={r.x,r.y,r.width,r.height,r.stride,r.offset};
        }
        if(op==BROOK_GPU_HANDOFF_QUEUE_REGIONS)
            return brook::BdwQueueOffscreenPresent(owner,wire.ticket,0,0,0,regions,wire.count);
        return brook::BdwOffscreenHandoffPresentRegions(owner,wire.ticket,regions,wire.count);
    }
    if(op==BROOK_GPU_HANDOFF_PRESENT_PROBE || op==BROOK_GPU_HANDOFF_PRESENT_LEASE){
        BrookGpuPresentProbe probe{};
        if(!CopyFromUser(&probe,arg,sizeof(probe)) || !UserBufferWritable(arg,sizeof(probe)))return -EFAULT;
        const int64_t result=brook::BdwOffscreenHandoffPresentProbe(owner,probe,op==BROOK_GPU_HANDOFF_PRESENT_LEASE);
        if(result)return result;
        return CopyToUser(arg,&probe,sizeof(probe))?0:-EFAULT;
    }
    if(op==BROOK_GPU_HANDOFF_RESERVE || op==BROOK_GPU_HANDOFF_RESERVE_DEPENDENT){
        if(arg>=MAX_FDS)return -EBADF;
        FdRef ref(proc,static_cast<int>(arg));
        if(!ref || ref->type!=FdType::IntelPrime || !ref->handle)return -EBADF;
        const int64_t result=brook::BdwOffscreenHandoffControl(owner,op,arg,static_cast<brook::BdwGpuBuffer*>(ref->handle),a);
        if(result<0 && result!=-116){ // -116 (ESTALE): a dependent baseline moved, the renderer's normal retry
            static uint64_t refusals=0;const char* why=brook::BdwTakeRefusal();
            if(brook::BdwRefusalLogDue(refusals))
                KPrintf("gpu-handoff: reserve refused pid=%u errno=%ld reason=%s count=%lu\n",proc->pid,-result,why?why:"unrecorded",refusals);
        }else (void)brook::BdwTakeRefusal();
        // Reserving proves the renderer alive through a refused-present streak.
        if(result>0)(void)CompositorRenewLiveRendererLease(owner);
        return result;
    }
    return brook::BdwOffscreenHandoffControl(owner,op,arg,nullptr);
#else
    (void)op;(void)arg;(void)a;(void)b;(void)c;(void)d;return -ENODEV;
#endif
}

static_assert(BROOK_SYS_DESKTOP_LAYERS!=BROOK_SYS_GPU_HANDOFF_CTL && BROOK_SYS_DESKTOP_LAYERS!=BROOK_SYS_GPU_OUTPUT_ALLOC,
              "desktop layers syscall must be distinct");

int64_t sys_brook_gpu_window_validate(uint64_t fdValue,uint64_t window,uint64_t incarnation,
                                       uint64_t a,uint64_t b,uint64_t c)
{
    auto* proc=ProcessCurrent();if(!proc)return -ESRCH;
    KernelOperationGuard operation(proc);
    if(a || b || c)return -EINVAL;
    return ValidateGpuWindowProvider(proc,fdValue,window,incarnation);
}

int64_t sys_brook_gpu_window_peer_recovery(uint64_t fdValue,uint64_t op,uint64_t bufferFd,
                                            uint64_t a,uint64_t b,uint64_t c)
{
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
    auto* proc=ProcessCurrent();if(!proc || !proc->fds)return -ESRCH;
    KernelOperationGuard operation(proc);
    if(fdValue>=MAX_FDS || a || b || c || op>BROOK_GPU_WINDOW_PEER_RESTART ||
       (op!=BROOK_GPU_WINDOW_PEER_DRAIN && bufferFd))return -EINVAL;
    FdRef descriptor(proc,static_cast<int>(fdValue));auto* entry=descriptor.get();
    if(!entry || entry->type!=FdType::UnixSocket || !entry->handle)return -EBADF;
    auto* socket=static_cast<UnixSocketData*>(entry->handle);
    if(op==BROOK_GPU_WINDOW_PEER_RETIRE){
        const uint64_t owner=UnixCurrentIncarnation(proc);
        if(!owner || !brook::BdwOffscreenRendererOwnedBy(owner))return -EPERM;
        SpinLockAcquire(&g_unixRegistryLock);
        const bool accepted=socket->state==UnixSocketData::State::Connected && socket->acceptedEndpoint &&
            socket->socketType==5 && socket->rxPipe && socket->txPipe && socket->peerIncomingFds;
        SpinLockRelease(&g_unixRegistryLock);
        if(!accepted)return -EPERM;
        // FdRef pins both directional queues. Stop additional fd transfers
        // before publishing the exact accepted renderer's retirement.
        UnixShutdownDirection(socket->rxPipe,false);
        UnixShutdownDirection(socket->txPipe,true);
        SpinLockAcquire(&g_unixRegistryLock);
        socket->peerIncomingFds->gpuRendererRetiredIncarnation=owner;
        SpinLockRelease(&g_unixRegistryLock);
        return 0;
    }
    UnixCredentials peer{};uint64_t peerIncarnation=0;bool authorized=false,retired=false;
    SpinLockAcquire(&g_unixRegistryLock);
    const bool connected=socket->state==UnixSocketData::State::Connected && !socket->acceptedEndpoint && socket->socketType==5;
    if(connected){
        peer=socket->peerCredentials.raw;peerIncarnation=socket->peerIncarnation;authorized=socket->gpuPeerAuthorized;
        retired=peerIncarnation && socket->incomingFds &&
            socket->incomingFds->gpuRendererRetiredIncarnation==peerIncarnation;
    }
    SpinLockRelease(&g_unixRegistryLock);
    if(!connected || peer.pid<=0 || peer.pid>65535 || !peerIncarnation)return -EPERM;
    if(op==BROOK_GPU_WINDOW_PEER_AUTH){
        if(!brook::BdwOffscreenRendererOwnedBy(peerIncarnation))return -EPERM;
        SpinLockAcquire(&g_unixRegistryLock);
        const bool same=socket->state==UnixSocketData::State::Connected && socket->peerIncarnation==peerIncarnation && socket->peerCredentials.raw.pid==peer.pid;
        if(same)socket->gpuPeerAuthorized=true;
        SpinLockRelease(&g_unixRegistryLock);return same?0:-116; // ESTALE
    }
    if(!authorized)return -EPERM;
    if(op==BROOK_GPU_WINDOW_PEER_RESTART){
        if(ProcessReadIds(proc,false).effective!=0 || !ProcessCapable(proc,nullptr,CapSysAdmin))return -EPERM;
        if(!socket->rxPipe || !socket->txPipe ||
           !__atomic_load_n(&socket->rxPipe->unixReadShutdown,__ATOMIC_ACQUIRE) ||
           !__atomic_load_n(&socket->txPipe->unixWriteShutdown,__ATOMIC_ACQUIRE))return -EPERM;
        if(retired || !brook::BdwOffscreenRendererOwnedBy(peerIncarnation))return 0;
        auto* target=SchedulerFindProcessReference(static_cast<uint16_t>(peer.pid));
        if(!target)return -ESRCH;
        const int result=!target->isThread && !target->isKernelThread && target->incarnation==peerIncarnation
            ? ProcessSendSignal(target,9,nullptr,proc) : -116; // ESTALE
        ProcessUnref(target);
        return result;
    }
    if(bufferFd>=MAX_FDS)return -EBADF;
    FdRef source(proc,static_cast<int>(bufferFd));auto* bufferEntry=source.get();
    if(!bufferEntry || bufferEntry->type!=FdType::IntelPrime || !bufferEntry->handle)return -EBADF;
    auto* buffer=static_cast<brook::BdwGpuBuffer*>(bufferEntry->handle);
    if(!brook::BdwGpuBufferAllowsUserMapping(buffer))return -13;
    if(!retired && !brook::SchedulerThreadGroupGone(static_cast<uint16_t>(peer.pid),peerIncarnation))return -EAGAIN;
    const auto state=brook::BdwGpuBufferTryCpuTransition(buffer);
    return state==brook::BdwCpuTransitionResult::Ready?0:state==brook::BdwCpuTransitionResult::Busy?-EAGAIN:-EIO;
#else
    (void)fdValue;(void)op;(void)bufferFd;(void)a;(void)b;(void)c;return -ENODEV;
#endif
}

// ===========================================================================
// App-GPU syscalls (0xB10..): per-process virgl context for userspace GL-style
// rendering. The app gets its own context + resource ids, submits virgl command
// streams, and reads results back into its window VFB. Backed by GpuAppOps,
// which a 3D display driver registers. All gate on the caller owning the ctx
// (proc->gpuAppCtx). Numbers sit in the high 0xB00 block (clear of Linux/Windows).
// ===========================================================================

// 0xB10: GPU_CTX_CREATE() -> ctxId (>0), or <0. One context per process.
int64_t sys_brook_gpu_ctx_create(uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    const brook::GpuAppOps* ga = brook::GpuAppGet();
    if (!ga || !ga->CtxCreate) return -ENODEV;
    if (proc->gpuAppCtx > 0) return proc->gpuAppCtx;   // already have one
    int32_t ctx = ga->CtxCreate(proc->pid, {});
    if (ctx < 0) return -ENOMEM;
    proc->gpuAppCtx = ctx;
    return ctx;
}

// 0xB11: GPU_RESOURCE_CREATE(ctx, format, bind, w, h) -> host-global resId (>0)
// or <0. The returned id is used directly in the app's virgl streams.
int64_t sys_brook_gpu_resource_create(uint64_t ctx, uint64_t format,
                                      uint64_t bind, uint64_t w, uint64_t h,
                                      uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    const brook::GpuAppOps* ga = brook::GpuAppGet();
    if (!ga || !ga->ResourceCreate3D) return -ENODEV;
    if ((int32_t)ctx != proc->gpuAppCtx || proc->gpuAppCtx <= 0) return -EPERM;
    if (w == 0 || h == 0 || w > 8192 || h > 8192) return -EINVAL;
    // This native ABI only creates 2-D textures; pass PIPE_TEXTURE_2D (2) so the
    // driver picks the texture path (target 0 is reserved for PIPE_BUFFER).
    int32_t res = ga->ResourceCreate3D((int32_t)ctx, /*PIPE_TEXTURE_2D=*/2u,
                                       (uint32_t)format, (uint32_t)bind,
                                       (uint32_t)w, (uint32_t)h,
                                       /*depth=*/1u, /*arraySize=*/1u,
                                       /*lastLevel=*/0u, /*nrSamples=*/0u,
                                       /*flags=*/0u);
    return res < 0 ? -ENOMEM : res;
}

// 0xB12: GPU_ATTACH_WINDOW(ctx, resId, wmId) — back a resource with the caller's
// window VFB (kernel-mapped), so host-rendered pixels land straight in the
// window. Convenience for the common "render into my window" case.
int64_t sys_brook_gpu_attach_window(uint64_t ctx, uint64_t resId,
                                    uint64_t wmId, uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    const brook::GpuAppOps* ga = brook::GpuAppGet();
    if (!ga || !ga->ResourceAttachUser) return -ENODEV;
    if ((int32_t)ctx != proc->gpuAppCtx || proc->gpuAppCtx <= 0) return -EPERM;
    brook::Window* win = brook::WmFindWindowById(proc, static_cast<uint32_t>(wmId));
    if (!win || !win->vfb || !win->vfbBytes) return -EINVAL;
    int32_t r = ga->ResourceAttachUser((int32_t)ctx, (int32_t)resId,
                                       reinterpret_cast<uint64_t>(win->vfb),
                                       static_cast<uint32_t>(win->vfbBytes));
    return r < 0 ? -EINVAL : 0;
}

// 0xB16: GPU_UPLOAD_BUFFER(ctx, srcPtr, bytes) -> host-global resId (>0) or <0.
// Creates a vertex-buffer resource holding a copy of the app's data (e.g.
// vertices) and pushes it to the host; the returned id is used in the app's
// virgl SET_VERTEX_BUFFERS command.
int64_t sys_brook_gpu_upload_buffer(uint64_t ctx, uint64_t srcPtr,
                                    uint64_t bytes, uint64_t,
                                    uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    const brook::GpuAppOps* ga = brook::GpuAppGet();
    if (!ga || !ga->BufferUpload) return -ENODEV;
    if ((int32_t)ctx != proc->gpuAppCtx || proc->gpuAppCtx <= 0) return -EPERM;
    if (bytes == 0 || bytes > 16u * 1024 * 1024) return -EINVAL;
    if (!UserBufferReadable(srcPtr, bytes)) return -EFAULT;
    int32_t r = ga->BufferUpload((int32_t)ctx,
                                 reinterpret_cast<const void*>(srcPtr), (uint32_t)bytes);
    return r < 0 ? -EINVAL : r;
}

// 0xB13: GPU_SUBMIT(ctx, cmdPtr, nDwords) — submit a virgl command stream.
int64_t sys_brook_gpu_submit(uint64_t ctx, uint64_t cmdPtr, uint64_t nDwords,
                             uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    const brook::GpuAppOps* ga = brook::GpuAppGet();
    if (!ga || !ga->Submit3D) return -ENODEV;
    if ((int32_t)ctx != proc->gpuAppCtx || proc->gpuAppCtx <= 0) return -EPERM;
    if (nDwords == 0 || nDwords > 8192) return -EINVAL;
    if (!UserBufferReadable(cmdPtr, nDwords * 4)) return -EFAULT;
    int32_t r = ga->Submit3D((int32_t)ctx, reinterpret_cast<const uint32_t*>(cmdPtr),
                             (uint32_t)nDwords);
    return r < 0 ? -EIO : 0;
}

// 0xB14: GPU_TRANSFER(ctx, resId, dir, w, h) — dir 0=to host, 1=from host.
int64_t sys_brook_gpu_transfer(uint64_t ctx, uint64_t resId, uint64_t dir,
                               uint64_t w, uint64_t h, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    const brook::GpuAppOps* ga = brook::GpuAppGet();
    if (!ga || !ga->Transfer3D) return -ENODEV;
    if ((int32_t)ctx != proc->gpuAppCtx || proc->gpuAppCtx <= 0) return -EPERM;
    if (w == 0 || h == 0 || w > 8192 || h > 8192 || dir > 1) return -EINVAL;
    const brook::GpuTransfer3D transfer{
        0,0,0,(uint32_t)w,(uint32_t)h,1,0,0,(uint32_t)w*4,(uint32_t)(w*h*4)};
    int32_t r = ga->Transfer3D((int32_t)ctx, (int32_t)resId, (int)dir,
                               transfer);
    return r < 0 ? -EIO : 0;
}

// 0xB15: GPU_CTX_DESTROY(ctx) — release the process's GPU context.
int64_t sys_brook_gpu_ctx_destroy(uint64_t ctx, uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    const brook::GpuAppOps* ga = brook::GpuAppGet();
    if (!ga || !ga->CtxDestroy) return -ENODEV;
    if ((int32_t)ctx != proc->gpuAppCtx || proc->gpuAppCtx <= 0) return -EPERM;
    ga->CtxDestroy((int32_t)ctx);
    proc->gpuAppCtx = 0;
    return 0;
}

} // namespace brook
