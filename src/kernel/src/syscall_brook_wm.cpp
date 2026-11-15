// syscall_brook_wm.cpp -- Brook window-manager syscalls.
//
// Split from syscall.cpp (see docs/agent/syscall-split-plan.md).
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "mouse.h"

#include "string.h"
#include "memory/mapping_transaction.h"
#include "compositor.h"
#include "window.h"
#include "gpu_compositor.h"
#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_unix_socket.h"
#include "syscall_drm.h"
#include "syscall_memory.h"
#include "syscall_handlers.h"

namespace brook {

// ---------------------------------------------------------------------------
// Brook WM syscalls 506-509 — multi-window-per-process WM API.
// Phase A of the wayland↔WM unification.  Lets one process (e.g. waylandd)
// own multiple top-level windows, each backed by its own kernel-allocated
// VFB mapped read/write into the caller's address space.
// ---------------------------------------------------------------------------

// 506: WM_CREATE_WINDOW
//   arg0 = clientW (uint16)
//   arg1 = clientH (uint16)
//   arg2 = title pointer (user, 0-terminated)
//   arg3 = out struct pointer (user) — receives:
//          uint32_t wmId, uint32_t vfbStride, uint64_t vfbUser
//   arg4 = flags:
//          bit 0 requests initial client-side decoration/noChrome
//          bit 1 creates a raised but non-keyboard-focusable popup/tool window
//          bit 2 identifies this process itself as the native application client
// Returns 0 on success, -errno on failure.
struct BrookWmCreateOut {
    uint32_t wmId;
    uint32_t vfbStride;
    uint64_t vfbUser;
};

// Window titles are bounded at 63 bytes. A longer user string is truncated
// (the terminated prefix is kept), as the byte loop did before; an
// inaccessible or kernel pointer is -EFAULT.
static int64_t CopyWindowTitle(uint64_t address, char (&title)[64])
{
    const int result = CopyUserCStringResult(address, title, sizeof(title));
    return result == -36 ? 0 : result;
}

static int64_t CopyCreateOut(uint64_t outPtr, const WmCreateWindowResult& res)
{
    BrookWmCreateOut out{};
    out.wmId      = res.wmId;
    out.vfbStride = res.vfbStride;
    out.vfbUser   = reinterpret_cast<uint64_t>(res.vfbUser);
    return CopyToUser(outPtr, &out, sizeof(out)) ? 0 : -EFAULT;
}

int64_t sys_brook_wm_create_window(uint64_t cW, uint64_t cH,
                                    uint64_t titlePtr, uint64_t outPtr,
                                    uint64_t flags, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (cW == 0 || cH == 0 || cW > 0xFFFF || cH > 0xFFFF) return -EINVAL;
    if (!outPtr) return -EFAULT;

    char titleBuf[64] = {};
    if (titlePtr) {
        if (int64_t error = CopyWindowTitle(titlePtr, titleBuf)) return error;
    }

    if (flags & ~7ull) return -EINVAL;
    // Check the result buffer before creating anything; a raced unmap after
    // creation is rolled back below.
    if (!UserBufferWritable(outPtr, sizeof(BrookWmCreateOut))) return -EFAULT;
    bool focusable = (flags & 2u) == 0;
    auto res = brook::WmCreateWindowForProcess(proc,
                                                 static_cast<uint16_t>(cW),
                                                 static_cast<uint16_t>(cH),
                                                 titlePtr ? titleBuf : nullptr,
                                                 focusable, (flags & 4u) != 0);
    if (res.wmId == 0) return -ENOMEM;
    if (flags & 1u) {
        brook::WmSetClientSideDecoration(static_cast<int>(res.wmId) - 1, true);
    }

    if (int64_t error = CopyCreateOut(outPtr, res)) {
        brook::WmDestroyWindowById(proc, res.wmId);
        return error;
    }
    return 0;
}

// 507: WM_DESTROY_WINDOW(wmId)
int64_t sys_brook_wm_destroy_window(uint64_t wmId, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFFFFFu) return -EINVAL;
    brook::WmDestroyWindowById(proc, static_cast<uint32_t>(wmId));
    return 0;
}

// 508: WM_SIGNAL_DIRTY(wmId) — caller has finished writing to the VFB.
int64_t sys_brook_wm_signal_dirty(uint64_t wmId, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0) return -EINVAL;
    brook::WmSignalDirtyById(proc, static_cast<uint32_t>(wmId));
    return 0;
}

int64_t sys_brook_wm_publish_source_commit(uint64_t wmId,uint64_t incarnation,uint64_t commit,
                                            uint64_t a,uint64_t b,uint64_t c)
{
    Process* proc=ProcessCurrent();
    if(!proc)return -ESRCH;
    if(!wmId || wmId>0xffffffffULL || a || b || c)return -EINVAL;
    return brook::WmPublishSourceCommit(proc,static_cast<uint32_t>(wmId),incarnation,commit);
}
int64_t sys_brook_wm_begin_source_write(uint64_t wmId,uint64_t incarnation,uint64_t a,
                                         uint64_t b,uint64_t c,uint64_t d)
{
    Process* proc=ProcessCurrent();if(!proc)return -ESRCH;
    if(!wmId || wmId>0xffffffffULL || a || b || c || d)return -EINVAL;
    return brook::WmBeginSourceWrite(proc,static_cast<uint32_t>(wmId),incarnation);
}
int64_t sys_brook_wm_query_source_commit(uint64_t wmId,uint64_t incarnation,uint64_t generation,
                                          uint64_t output,uint64_t a,uint64_t b)
{
    if(!wmId || wmId>0xffffffffULL || !output || a || b || !UserBufferWritable(output,sizeof(uint64_t)))return -EINVAL;
    uint64_t commit=0;const auto result=brook::WmQuerySourceCommit(static_cast<uint32_t>(wmId),incarnation,generation,&commit);
    if(result)return result;
    return CopyToUser(output,&commit,sizeof(commit))?0:-EFAULT;
}

// 525: WM_PRESENT_GRES(wmId, gres, w, h) — present a hardware-GL (dmabuf) frame.
// waylandd resolves a client's dmabuf to a host virgl resource id (gres) and
// calls this to wait for a compositor-owned capture of the source frame.
// The gres must be one the caller legitimately holds (it came from the
// caller importing a PRIME fd the client sent it over the Wayland socket).
int64_t sys_brook_wm_present_gres(uint64_t wmId, uint64_t gres,
                                  uint64_t w, uint64_t h,
                                  uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    KernelOperationGuard operation(proc);
    constexpr uint64_t wireMax=0xffffffffULL;
    if (!wmId || wmId>wireMax || !gres || gres>wireMax || !w || !h ||
        w>wireMax || h>wireMax) return -EINVAL;
    // This legacy ABI names a resource numerically, but the caller must own
    // its GEM capability. In particular, never permit an unexported resource
    // to become an untracked compositor source before enabling reclamation.
    MappingBackingGuard pin;
    DrmResource* resource=nullptr;
    for (uint32_t fd=0;fd<MAX_FDS && !resource;++fd) {
        FdRef descriptor(proc,static_cast<int>(fd));
        if (!descriptor || descriptor->type!=FdType::DevDri || !descriptor->handle) continue;
        auto* context=static_cast<DrmCtx*>(descriptor->handle);
        DrmMappingGuard guard(context);
        for (auto& gem:context->gem) {
            if (!gem.used || gem.resource->gres!=gres) continue;
            if (!__atomic_load_n(&gem.resource->everExported,__ATOMIC_ACQUIRE)) return -EINVAL;
            resource=gem.resource;MappingBackingRef(resource->lifetime);
            pin.value=resource->lifetime;break;
        }
    }
    if (!resource || resource->owner->config.capsetId==4 || resource->target!=2 || !brook::GpuCaptureFormatSupported(resource->format) ||
        w>resource->width || h>resource->height) return -EINVAL;
    brook::WmCaptureToken token{};
    if (!brook::WmReserveCapture(proc,static_cast<uint32_t>(wmId),
                               static_cast<uint32_t>(w),static_cast<uint32_t>(h),&token)) return -ENOENT;
    return brook::CompositorCaptureExternal(proc,token,resource->lifetime,
                                           resource->owner->ctxId,resource->gres,resource->format);
}

int64_t sys_brook_wm_place_window(uint64_t window, uint64_t incarnation,
                                  uint64_t x, uint64_t y, uint64_t flags, uint64_t reserved)
{
    auto* proc=ProcessCurrent(); if (!proc) return -ESRCH;
    KernelOperationGuard operation(proc);
    const int64_t clientX=static_cast<int64_t>(x), clientY=static_cast<int64_t>(y);
    if (!window || window>WM_MAX_WINDOWS || !incarnation || flags || reserved ||
        clientX < -32768 || clientX > 32767 || clientY < -32768 || clientY > 32767) return -EINVAL;
    return brook::WmPlaceOwnedWindow(proc,static_cast<uint32_t>(window),incarnation,
                                     static_cast<int32_t>(clientX),static_cast<int32_t>(clientY));
}

int64_t sys_brook_wm_window_state(uint64_t window, uint64_t incarnation,
                                  uint64_t output, uint64_t size,
                                  uint64_t flags, uint64_t reserved)
{
    auto* proc = ProcessCurrent(); if (!proc) return -ESRCH;
    KernelOperationGuard operation(proc);
    if (!window || window > WM_MAX_WINDOWS || size != sizeof(BrookWindowState) || flags || reserved)
        return -EINVAL;
    // Prefault outside the window lock. Copy a local snapshot after unlocking,
    // so user faults never run under the compositor's window mutex.
    if (!UserBufferWritable(output, sizeof(BrookWindowState))) return -EFAULT;
    BrookWindowState state{};
    const int result = brook::WmSnapshotOwnedWindowState(proc, static_cast<uint32_t>(window), incarnation, &state);
    if (result) return result;
    return CopyToUser(output, &state, sizeof(state)) ? 0 : -EFAULT;
}

// 0xB03: notify the desktop that installed application metadata changed.
int64_t sys_brook_wm_refresh_apps(uint64_t, uint64_t, uint64_t,
                                 uint64_t, uint64_t, uint64_t)
{
    if (!ProcessCurrent()) return -ESRCH;
    brook::WmRequestLauncherRefresh();
    return 0;
}

// 0xB02: desktop theme query (op 0) or select immutable named palette (op 1).
int64_t sys_brook_wm_theme(uint64_t op, uint64_t theme,
                          uint64_t, uint64_t, uint64_t, uint64_t)
{
    if (!ProcessCurrent()) return -ESRCH;
    if (op == 0) return brook::WmGetTheme();
    if (op != 1 || theme > 0xffffffffULL) return -EINVAL;
    return brook::WmSetTheme(static_cast<uint32_t>(theme)) ? 0 : -EINVAL;
}

// 0xB0B: atomic appearance preferences, independent of material palette/scale.
int64_t sys_brook_wm_appearance(uint64_t op,uint64_t value,uint64_t,uint64_t,uint64_t,uint64_t){
    if(!ProcessCurrent())return -ESRCH;
    if(op==0)return WmGetAppearance();
    if(op!=1||value>0xffffffffULL)return -EINVAL;
    return WmSetAppearance(static_cast<uint32_t>(value))?0:-EINVAL;
}

// 0xB0C: independently selectable taskbar telemetry widgets.
int64_t sys_brook_pointer_speed(uint64_t op,uint64_t value,uint64_t,uint64_t,uint64_t,uint64_t){
    if(!ProcessCurrent())return -ESRCH;
    if(op==0)return MouseTouchpadSpeed();
    if(op!=1||value>0xffffffffULL)return -EINVAL;
    return MouseTouchpadSetSpeed(static_cast<uint32_t>(value))?0:-EINVAL;
}
// 0xB23: touchpad tap-to-click (0 off, 1 on).
int64_t sys_brook_touchpad_tap(uint64_t op,uint64_t value,uint64_t,uint64_t,uint64_t,uint64_t){
    if(!ProcessCurrent())return -ESRCH;
    if(op==0)return MouseTouchpadTap()?1:0;
    if(op!=1||value>1)return -EINVAL;
    MouseTouchpadSetTap(value!=0);
    return 0;
}
int64_t sys_brook_wm_taskbar_metrics(uint64_t op,uint64_t value,uint64_t,uint64_t,uint64_t,uint64_t){
    if(!ProcessCurrent())return -ESRCH;
    if(op==0)return WmGetTaskbarMetrics();
    if(op!=1||value>0xffffffffULL)return -EINVAL;
    return WmSetTaskbarMetrics(static_cast<uint32_t>(value))?0:-EINVAL;
}

// 0xB04: native desktop scale. Query requested (0), request (1), query applied (2).
int64_t sys_brook_wm_scale(uint64_t op,uint64_t scale,uint64_t,uint64_t,uint64_t,uint64_t)
{
    if (!ProcessCurrent()) return -ESRCH;
    if (op==0) return WmGetRequestedScale();
    if (op==2) return WmGetAppliedScale();
    if (op!=1 || scale>0xffffffffULL) return -EINVAL;
    return WmSetScale(static_cast<uint32_t>(scale)) ? 0 : -EINVAL;
}

// 0xB01: WM_SET_APP_ID(wmId, appId*) — metadata for an owned window.
// 0xB05: WM_SET_CLIENT_SOCKET(wmId, accepted AF_UNIX stream fd). The kernel
// authenticates connection origin; the provider supplies the surface association.
// This metadata grants no authority to signal another process.
int64_t sys_brook_wm_set_client_socket(uint64_t wmId,uint64_t fdValue,
    uint64_t,uint64_t,uint64_t,uint64_t)
{
    Process* proc=ProcessCurrent();
    if (!proc) return -ESRCH;
    if (!wmId || wmId>0xffffffffULL || fdValue>0x7fffffffULL) return -EINVAL;
    KernelOperationGuard operation(proc);
    FdRef descriptor(proc,static_cast<int>(fdValue));
    auto* entry=descriptor.get();
    if (!entry) return -EBADF;
    if (entry->type!=FdType::UnixSocket || !entry->handle) return -88; // ENOTSOCK
    auto* socket=static_cast<UnixSocketData*>(entry->handle);
    UnixCredentials credentials{};uint64_t incarnation=0,launchId=0;
    SpinLockAcquire(&g_unixRegistryLock);
    const bool accepted=socket->state==UnixSocketData::State::Connected &&
                        socket->acceptedEndpoint && socket->socketType==1;
    if (accepted) {
        credentials=socket->peerCredentials;incarnation=socket->peerIncarnation;launchId=socket->peerLaunchId;
    }
    SpinLockRelease(&g_unixRegistryLock);
    if (!accepted) return -95; // EOPNOTSUPP
    if (credentials.pid<=0 || credentials.pid>65535 || !incarnation) return -ESRCH;
    return WmRegisterClientOrigin(proc,static_cast<uint32_t>(wmId),
        static_cast<uint16_t>(credentials.pid),incarnation,launchId)?0:-EPERM;
}

int64_t sys_brook_wm_set_app_id(uint64_t wmId, uint64_t appIdPtr,
                               uint64_t, uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (!wmId || wmId > 0xffffffffULL || !appIdPtr) return -EINVAL;
    char appId[128] = {};
    if (!CopyUserCString(appIdPtr, appId, sizeof(appId))) return -EFAULT;
    return brook::WmSetAppIdById(proc, static_cast<uint32_t>(wmId), appId) ? 0 : -ENOENT;
}

// 509: WM_SET_TITLE(wmId, title*)
int64_t sys_brook_wm_set_title(uint64_t wmId, uint64_t titlePtr,
                                uint64_t, uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || !titlePtr) return -EINVAL;
    char buf[64] = {};
    if (int64_t error = CopyWindowTitle(titlePtr, buf)) return error;
    {
        extern volatile uint64_t g_blockCacheHits;
        extern volatile uint64_t g_blockCacheMisses;
        SerialPrintf("[PROFILE] set_title t=%lums pid=%u '%s'"
                     " opens=%lu mmaps=%lu faults=%lu"
                     " cache_hit=%lu cache_miss=%lu preloaded=%lu"
                     " blkcache_hit=%lu blkcache_miss=%lu\n",
                     ApicTickCount(), proc->pid, buf,
                     g_profOpenCount, g_profMmapCount, g_profFaultCount,
                     g_profCacheHit, g_profCacheMiss, g_profPreloadPages,
                     g_blockCacheHits, g_blockCacheMisses);
    }
    brook::WmSetTitleById(proc, static_cast<uint32_t>(wmId), buf);
    return 0;
}

// 0xB0A: owned-window pointer lease. Query is independent of event delivery,
// so a full user queue cannot keep a revoked capture active in userspace.
int64_t sys_brook_wm_pointer_capture(uint64_t wmId,uint64_t operation,uint64_t token,
                                    uint64_t,uint64_t,uint64_t) {
    Process* proc=ProcessCurrent();
    if(!proc)return -ESRCH;
    if(!wmId || wmId>WM_MAX_WINDOWS || operation>3 || token>0xffffffffu)return -EINVAL;
    KernelOperationGuard operationGuard(proc);
    return brook::WmPointerCapture(proc,static_cast<uint32_t>(wmId),
                                   static_cast<uint32_t>(operation),static_cast<uint32_t>(token));
}

// 510: WM_POP_INPUT(wmId, buf*, max) — drain per-window input queue.
// Each event written is a brook::Window::WmInputEvent (12 bytes).
// Returns number of events written, or -errno.
int64_t sys_brook_wm_pop_input(uint64_t wmId, uint64_t bufPtr,
                                uint64_t max, uint64_t,
                                uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || !bufPtr || !max) return -EINVAL;
    if (max > 256) max = 256;
    using Event = brook::Window::WmInputEvent;
    // Check before popping so a bad buffer does not discard events. Pop
    // into kernel memory under the window lock, then copy with it dropped.
    if (!UserBufferWritable(bufPtr, max * sizeof(Event))) return -EFAULT;
    static constexpr uint32_t Batch = 64;
    Event staged[Batch];
    uint64_t produced = 0;
    while (produced < max) {
        const uint32_t want = static_cast<uint32_t>(max - produced < Batch ? max - produced : Batch);
        const int got = brook::WmInputPopById(proc, static_cast<uint32_t>(wmId), staged, want);
        if (got < 0) return produced ? static_cast<int64_t>(produced) : got;
        if (!got) break;
        // A raced unmap loses only this batch; report what already landed.
        if (!CopyToUser(bufPtr + produced * sizeof(Event), staged, static_cast<uint64_t>(got) * sizeof(Event)))
            return produced ? static_cast<int64_t>(produced) : -EFAULT;
        produced += static_cast<uint64_t>(got);
        if (static_cast<uint32_t>(got) < want) break;
    }
    return static_cast<int64_t>(produced);
}

// 511: WM_RESIZE_VFB(wmId, newW, newH, outPtr)
//   Reallocates the kernel VFB for an existing waylandd-hosted toplevel
//   to (newW × newH).  Old user mapping is unmapped and a fresh user
//   mapping is returned via outPtr (BrookWmCreateOut layout).  Should be
//   called by waylandd after the client commits a buffer at the size we
//   asked for via xdg_toplevel.configure.
int64_t sys_brook_wm_resize_vfb(uint64_t wmId, uint64_t newW,
                                 uint64_t newH, uint64_t outPtr,
                                 uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFFFFFu) return -EINVAL;
    if (newW == 0 || newH == 0 || newW > 0xFFFF || newH > 0xFFFF) return -EINVAL;
    if (!outPtr) return -EFAULT;
    // The old mapping is gone once the resize succeeds, so check the result
    // buffer first. A raced unmap afterwards still reports -EFAULT.
    if (!UserBufferWritable(outPtr, sizeof(BrookWmCreateOut))) return -EFAULT;

    brook::WmCreateWindowResult res = {};
    int rc = brook::WmResizeVfbForProcess(proc, static_cast<uint32_t>(wmId),
                                           static_cast<uint16_t>(newW),
                                           static_cast<uint16_t>(newH),
                                           &res);
    if (rc != 0) return rc;
    return CopyCreateOut(outPtr, res);
}

// 512: WM_SET_DECORATION_MODE(wmId, mode, insets)
//   Toggle CSD (client-side decoration) mode for a waylandd-hosted window.
//   Used to honour zxdg_toplevel_decoration_v1.set_mode.  mode 0 is server
//   side; any other value is CSD: the kernel WM stops drawing chrome and the
//   whole outer area is client.  mode 2 additionally keeps the WM's caption
//   buttons over the client's top-right corner; insets packs the client's
//   right (bits 0-15) and top (bits 16-31) shadow margins in VFB pixels, and
//   height is the client's title-strip height in VFB pixels (0: native title
//   height), so the buttons span the whole strip.
//   Returns 2 when the caption overlay is in effect (older kernels return 0
//   and treat mode 2 as plain CSD), otherwise 0.
int64_t sys_brook_wm_set_decoration_mode(uint64_t wmId, uint64_t csd,
                                          uint64_t insets, uint64_t height,
                                          uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFu) return -EINVAL;
    if (insets > 0xFFFFFFFFull || height > 0xFFFFu) return -EINVAL;

    brook::Window* w = brook::WmFindWindowById(proc, static_cast<uint32_t>(wmId));
    if (!w) return -EINVAL;
    int idx = static_cast<int>(wmId) - 1;
    const bool overlay = csd == 2;
    brook::WmSetClientSideDecoration(idx, csd != 0, overlay,
                                     static_cast<uint16_t>(overlay ? insets & 0xFFFFu : 0),
                                     static_cast<uint16_t>(overlay ? insets >> 16 : 0),
                                     static_cast<uint16_t>(overlay ? height : 0));
    return overlay ? 2 : 0;
}

// 0xB00 (2816): WM_SET_WINDOW_PROPERTIES(wmId, mask, opacity, blurRadius)
//   Set per-window display properties. `mask` selects which to apply:
//     bit 0 (WM_PROP_OPACITY) -> opacity  (0..255, 255 = opaque)
//     bit 1 (WM_PROP_BLUR)    -> blurRadius (0 = off)
//   A high (0xB00) number deliberately clear of the Linux and Windows syscall
//   ranges. Replaces the opt/gpuopacity demo hook with a real per-window API;
//   the GPU DRAW path composites each window at its own opacity (and, for frosted
//   glass, its blur radius). Only the owning process may set its window's props.
int64_t sys_brook_wm_set_window_properties(uint64_t wmId, uint64_t mask,
                                            uint64_t opacity, uint64_t blurRadius,
                                            uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFu) return -EINVAL;

    return brook::WmSetOwnedWindowProperties(proc, static_cast<uint32_t>(wmId),
                                             static_cast<uint32_t>(mask),
                                             static_cast<uint8_t>(opacity & 0xFF),
                                             static_cast<uint8_t>(blurRadius & 0xFF)) ? 0 : -EINVAL;
}

// 513: WM_BEGIN_MOVE(wmId)
//   Begin moving a waylandd-hosted window. Used to honour xdg_toplevel.move
//   for client-side-decorated windows, where the titlebar click lands in
//   client space and the client asks the compositor to start an interactive move.
int64_t sys_brook_wm_begin_move(uint64_t wmId, uint64_t,
                                 uint64_t, uint64_t,
                                 uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFu) return -EINVAL;

    brook::Window* w = brook::WmFindWindowById(proc, static_cast<uint32_t>(wmId));
    if (!w) return -EINVAL;
    int idx = static_cast<int>(wmId) - 1;
    return brook::CompositorBeginWindowMove(idx) ? 0 : -EINVAL;
}

// 514: WM_MOVE_RELATIVE(wmId, parentWmId, relX, relY)
//   Position a waylandd-hosted popup relative to the parent window's client
//   area. xdg_popup positioners use parent-surface coordinates, while the
//   kernel WM stores outer window coordinates including optional chrome.
int64_t sys_brook_wm_move_relative(uint64_t wmId, uint64_t parentWmId,
                                    uint64_t relX, uint64_t relY,
                                    uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFu) return -EINVAL;
    if (parentWmId == 0 || parentWmId > 0xFFFFu) return -EINVAL;

    bool ok = brook::WmMoveWindowRelativeToParent(proc,
        static_cast<uint32_t>(wmId),
        static_cast<uint32_t>(parentWmId),
        static_cast<int32_t>(relX),
        static_cast<int32_t>(relY));
    return ok ? 0 : -EINVAL;
}

// 515: WM_SET_MAXIMIZED(wmId, enable)
int64_t sys_brook_wm_set_maximized(uint64_t wmId, uint64_t enable,
                                    uint64_t, uint64_t,
                                    uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFu) return -EINVAL;

    brook::Window* w = brook::WmFindWindowById(proc, static_cast<uint32_t>(wmId));
    if (!w) return -EINVAL;
    brook::WmSetMaximized(static_cast<int>(wmId) - 1, enable != 0);
    return 0;
}

// 516: WM_SET_MINIMIZED(wmId)
int64_t sys_brook_wm_set_minimized(uint64_t wmId, uint64_t,
                                    uint64_t, uint64_t,
                                    uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFu) return -EINVAL;

    brook::Window* w = brook::WmFindWindowById(proc, static_cast<uint32_t>(wmId));
    if (!w) return -EINVAL;
    brook::WmMinimizeWindow(static_cast<int>(wmId) - 1);
    return 0;
}

// 517: WM_BEGIN_RESIZE(wmId, edges)
// Edge bits are Brook-local: 1=top, 2=bottom, 4=left, 8=right.
int64_t sys_brook_wm_begin_resize(uint64_t wmId, uint64_t edges,
                                   uint64_t, uint64_t,
                                   uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFu) return -EINVAL;
    if ((edges & ~0xFULL) != 0 || edges == 0) return -EINVAL;

    brook::Window* w = brook::WmFindWindowById(proc, static_cast<uint32_t>(wmId));
    if (!w) return -EINVAL;
    bool top = (edges & 1u) != 0;
    bool bottom = (edges & 2u) != 0;
    bool left = (edges & 4u) != 0;
    bool right = (edges & 8u) != 0;
    if ((top && bottom) || (left && right)) return -EINVAL;

    return brook::CompositorBeginWindowResize(static_cast<int>(wmId) - 1,
                                               left, right, top, bottom)
        ? 0 : -EINVAL;
}

// 518: WM_SET_CURSOR_VISIBLE(visible)
// Mechanism for userspace compositors to hide/show Brook's built-in cursor.
// This does not implement Wayland cursor surfaces or hotspot rendering.
int64_t sys_brook_wm_set_cursor_visible(uint64_t visible, uint64_t,
                                         uint64_t, uint64_t,
                                         uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    return brook::CompositorSetCursorVisible(proc, visible != 0) ? 0 : -EPERM;
}

// 519: WM_SET_CURSOR_IMAGE(pixels_ptr, w, h, hotX, hotY)
// Upload a custom ARGB8888 cursor image (max 64×64). Pass w=0 to reset.
int64_t sys_brook_wm_set_cursor_image(uint64_t pixelsAddr, uint64_t w,
                                       uint64_t h, uint64_t hotX,
                                       uint64_t hotY, uint64_t)
{
    if (w == 0) {
        brook::CompositorSetCursorImage(nullptr, 0, 0, 0, 0);
        return 0;
    }
    if (w > 64 || h > 64) return -EINVAL;
    if (h == 0) {
        brook::CompositorSetCursorImage(nullptr, 0, 0, 0, 0);
        return 0;
    }
    Process* proc = ProcessCurrent();
    if (!proc || !proc->addressSpace) return -ESRCH;
    KernelOperationGuard operation(proc);
    const uint64_t byteSize = w * h * 4;
    // Populate lazy pages before acquiring the AS lock. Validation alone is
    // not a lifetime pin: a sibling may unmap/protect the buffer immediately.
    if (!UserBufferReadable(pixelsAddr, byteSize)) return -EFAULT;
    auto* staged = static_cast<uint8_t*>(kmalloc(byteSize));
    if (!staged) return -ENOMEM;
    int64_t result = 0;
    {
        MappingTransaction lifetime(proc, proc->addressSpace);
        // Read-only resident copy: mutation serializes leaf retirement,
        // protection changes and file invalidation. No backing escapes this lock.
        KMutexLock(&proc->addressSpace->mutation);
        uint64_t offset = 0;
        while (!result && offset < byteSize) {
            const uint64_t address = pixelsAddr + offset;
            const auto* range = MappingRangeFind(proc->addressSpace->ranges, address);
            if (!range || !(range->protection & 1)) { result = -EFAULT; break; }
            const uint64_t leaf = VmmReadLeaf(proc->addressSpace->pageTable, VirtualAddress(address));
            if ((leaf & (VMM_PRESENT | VMM_USER)) != (VMM_PRESENT | VMM_USER)) { result = -EFAULT; break; }
            uint64_t chunk = 4096 - (address & 4095);
            if (chunk > byteSize - offset) chunk = byteSize - offset;
            if (chunk > range->end - address) chunk = range->end - address;
            memcpy(staged + offset, reinterpret_cast<const void*>(address), chunk);
            offset += chunk;
        }
        KMutexUnlock(&proc->addressSpace->mutation);
    }
    // Cursor publication reads kernel memory only, after releasing the AS lock.
    if (!result && !brook::CompositorSetCursorImage(reinterpret_cast<const uint32_t*>(staged),
            static_cast<uint32_t>(w), static_cast<uint32_t>(h),
            static_cast<int32_t>(hotX), static_cast<int32_t>(hotY))) result = -EINVAL;
    kfree(staged);
    return result;
}

// 520: WM_GET_SCREEN_INFO(out_ptr) — return screen width and height
// Writes two uint32_t values [width, height] to the user-supplied buffer.
int64_t sys_brook_wm_get_screen_info(uint64_t outAddr, uint64_t, uint64_t,
                                      uint64_t, uint64_t, uint64_t)
{
    if (!UserBufferWritable(outAddr, 8)) return -EFAULT;
    uint32_t w = 0, h = 0;
    brook::CompositorGetPhysDims(&w, &h);
    uint32_t* out = reinterpret_cast<uint32_t*>(outAddr);
    out[0] = w;
    out[1] = h;
    return 0;
}

// ---------------------------------------------------------------------------
// 521: WM_SCANOUT_MAP(out_ptr) — map compositor backbuffer into caller's space.
// Writes a ScanoutMapResult struct to out_ptr.
// ---------------------------------------------------------------------------
int64_t sys_brook_wm_scanout_map(uint64_t outAddr, uint64_t, uint64_t,
                                  uint64_t, uint64_t, uint64_t)
{
    if (!UserBufferWritable(outAddr, sizeof(brook::ScanoutMapResult)))
        return -EFAULT;

    Process* proc = ProcessCurrent();
    auto* out = reinterpret_cast<brook::ScanoutMapResult*>(outAddr);
    uint64_t addr = brook::CompositorScanoutMap(proc, out);
    return addr ? 0 : -ENOMEM;
}

// ---------------------------------------------------------------------------
// 522: WM_SCANOUT_FLIP(minY, maxY) — copy dirty rows to MMIO framebuffer.
// ---------------------------------------------------------------------------
int64_t sys_brook_wm_scanout_flip(uint64_t minY, uint64_t maxY, uint64_t,
                                   uint64_t, uint64_t, uint64_t)
{
    return brook::CompositorScanoutFlip(static_cast<uint32_t>(minY),
                                        static_cast<uint32_t>(maxY));
}

// ---------------------------------------------------------------------------
// 523: WM_GET_WINDOWS(out_ptr, max_count) — query window state for usermode compositor
// ---------------------------------------------------------------------------
int64_t sys_brook_wm_get_windows(uint64_t outAddr, uint64_t maxCount, uint64_t,
                                  uint64_t, uint64_t, uint64_t)
{
    if (maxCount == 0) return 0;
    uint64_t bufSize = maxCount * sizeof(brook::WmWindowDesc);
    if (!UserBufferWritable(outAddr, bufSize)) return -EFAULT;
    Process* proc = ProcessCurrent();
    auto* out = reinterpret_cast<brook::WmWindowDesc*>(outAddr);
    return static_cast<int64_t>(brook::CompositorGetWindows(proc, out, static_cast<uint32_t>(maxCount)));
}

// ---------------------------------------------------------------------------
// 524: WM_MAP_WINDOW_VFB(wmId) — map a window's VFB into compositor process
// ---------------------------------------------------------------------------
int64_t sys_brook_wm_map_window_vfb(uint64_t wmId, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    uint64_t addr = brook::CompositorMapWindowVfb(proc, static_cast<uint32_t>(wmId));
    return addr ? static_cast<int64_t>(addr) : -ENOENT;
}

} // namespace brook
