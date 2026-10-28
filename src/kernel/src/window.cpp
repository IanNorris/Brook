#include "chrome_material.h"
#include "fw_cfg.h"
// Brook window manager — z-ordered windows with chrome.
//
// Each process gets a Window that wraps its VFB with a title bar, border,
// and close/maximise buttons. The compositor calls WmRenderChrome() after
// blitting client areas to draw the window decorations.

#include "window.h"
#include "desktop_telemetry.h"
#include "device.h"
#include "virtio_blk.h"
#include "net.h"
#include "smp.h"
#include "build_info.h"
#include "kvmclock.h"
#include "shutdown.h"
#include "launcher_command.h"
#include "launch_feedback.h"
#include "shell.h"
#include "process.h"
#include "scheduler.h"
#include "apic.h"
#include "spinlock.h"
#include "compositor.h"
#include "font_atlas.h"
#include "input.h"
#include "mouse.h"
#include "serial.h"
#include "panic.h"
#include "rtc.h"
#include "terminal.h"
#include "display.h"
#include "vfs.h"
#include "memory/virtual_memory.h"
#include "memory/user_mapping.h"
#include "memory/physical_memory.h"
#include "memory/address.h"
#include "memory/heap.h"
#include "mem_tag.h"
#include "string.h"

namespace brook {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

// Serializes VFB source publication, source pinning, resize and destruction.
// Rendering continues to use the compositor epoch to retire kernel readers.
static KMutex g_vfbLock{};
struct VfbGuard {
    VfbGuard() { KMutexLock(&g_vfbLock); }
    ~VfbGuard() { KMutexUnlock(&g_vfbLock); }
};
static uint64_t NextPublication(Window& window) {
    if (window.publicationGeneration!=~uint64_t{0}) ++window.publicationGeneration;
    return window.publicationGeneration;
}
static const DesktopMetrics* g_desktopMetrics = &DesktopMetric100;
static uint32_t g_requestedScale = 100;
const DesktopMetrics& WmDesktopMetrics() { return *__atomic_load_n(&g_desktopMetrics,__ATOMIC_ACQUIRE); }
uint32_t WmMetric(uint32_t base) { return static_cast<uint32_t>(WmDesktopMetrics().pixels(static_cast<int>(base))); }
uint32_t WmGetRequestedScale() { return __atomic_load_n(&g_requestedScale,__ATOMIC_ACQUIRE); }
uint32_t WmGetAppliedScale() { return WmDesktopMetrics().percent; }
const FontAtlas& WmDesktopFont() {
    uint32_t scale=WmGetAppliedScale();
    return scale==200 ? g_desktopFont200 : scale==150 ? g_desktopFont150 : g_desktopFont100;
}
bool WmSetScale(uint32_t percent) {
    if (!DesktopMetricsFor(percent)) return false;
    __atomic_store_n(&g_requestedScale,percent,__ATOMIC_RELEASE);
    CompositorWake();return true;
}
static bool ResizeWindowLocked(int idx,uint16_t newClientW,uint16_t newClientH);
static void FocusWindowLocked(int idx);

static uint32_t g_themeId = BROOK_THEME_BROOK;
static bool g_themeSettingsLoaded = false;
static uint32_t g_appearance = BROOK_APPEARANCE_DEFAULT;
static uint32_t g_taskbarMetrics = BROOK_TASKBAR_DEFAULT;
uint32_t WmGetTaskbarMetrics(){return __atomic_load_n(&g_taskbarMetrics,__ATOMIC_ACQUIRE);}
bool WmSetTaskbarMetrics(uint32_t value){
    if(!brook_taskbar_metrics_valid(value))return false;
    __atomic_store_n(&g_taskbarMetrics,value,__ATOMIC_RELEASE);
    CompositorMarkDirty();CompositorWake();return true;
}
uint32_t WmGetAppearance(){return __atomic_load_n(&g_appearance,__ATOMIC_ACQUIRE);}
bool WmSetAppearance(uint32_t value){
    if(!brook_appearance_valid(value))return false;
    __atomic_store_n(&g_appearance,value,__ATOMIC_RELEASE);
    CompositorMarkDirty();CompositorWake();return true;
}
void WmSetPerformanceMode(bool enabled){
    if(enabled)__atomic_fetch_or(&g_appearance,uint32_t(BROOK_APPEARANCE_PERFORMANCE),__ATOMIC_ACQ_REL);
    else __atomic_fetch_and(&g_appearance,~uint32_t(BROOK_APPEARANCE_PERFORMANCE),__ATOMIC_ACQ_REL);
    CompositorMarkDirty();CompositorWake();
}
uint32_t WmGetTheme() { return __atomic_load_n(&g_themeId, __ATOMIC_ACQUIRE); }
const WmPalette& WmCurrentPalette() { return *brook_theme_palette(WmGetTheme()); }
bool WmSetTheme(uint32_t theme)
{
    if (!brook_theme_palette(theme)) return false;
    __atomic_store_n(&g_themeId, theme, __ATOMIC_RELEASE);
    CompositorMarkDirty();
    CompositorWake();
    return true;
}

// Startup I/O runs in the shell's activation path, never during rendering.
// Accept only a complete bounded file and an exact supported numeric ID.
static void WmRestoreTheme()
{
    if (g_themeSettingsLoaded) return;
    g_themeSettingsLoaded = true;
    uint32_t selected = BROOK_THEME_BROOK;
    uint32_t selectedScale = 100;
    uint32_t appearance=WmGetAppearance();
    uint32_t taskbarMetrics=BROOK_TASKBAR_DEFAULT;
    Vnode* vn = VfsOpen("/data/home/.config/brook/settings.ini", 0);
    if (vn)
    {
        char buf[513]; uint32_t done = 0; uint64_t offset = 0;
        bool failed = false;
        while (done < sizeof(buf))
        {
            int rd = VfsRead(vn, buf + done, sizeof(buf) - done, &offset);
            if (rd < 0) { failed = true; break; }
            if (!rd) break;
            done += static_cast<uint32_t>(rd);
        }
        VfsClose(vn);
        if (!failed && done < sizeof(buf))
        {
            buf[done] = 0;
            bool desktopSection = false;
            for (char* line = buf; *line;)
            {
                char* end = line; while (*end && *end != '\n') ++end;
                bool more = *end != 0; *end = 0;
                char* p = line; while (*p == ' ' || *p == '\t') ++p;
                if (*p == '[')
                {
                    desktopSection = strncmp(p, "[Desktop]", 9) == 0;
                    if (desktopSection) {
                        p += 9;
                        while (*p == ' ' || *p == '\t' || *p == '\r') ++p;
                        desktopSection = !*p;
                    }
                }
                else if (desktopSection && strncmp(p, "theme=", 6) == 0)
                {
                    p += 6;
                    if (*p >= '0' && *p < '0' + BROOK_THEME_COUNT)
                    {
                        uint32_t value = *p++ - '0';
                        while (*p == ' ' || *p == '\t' || *p == '\r') ++p;
                        selected = !*p ? value : BROOK_THEME_BROOK;
                    }
                    else selected = BROOK_THEME_BROOK;
                }
                else if (desktopSection && strncmp(p,"scale=",6)==0) {
                    p+=6;uint32_t value=0,digits=0;
                    while (*p>='0' && *p<='9' && digits<4) {value=value*10+(*p++-'0');++digits;}
                    while (*p==' ' || *p=='\t' || *p=='\r') ++p;
                    selectedScale=!*p && digits==3 && DesktopMetrics::Supported(value) ? value : 100;
                }
                else if(desktopSection){
                    appearance=brook_appearance_setting(appearance,p);
                    taskbarMetrics=brook_taskbar_metrics_setting(taskbarMetrics,p);
                }
                if (!more) break;
                line = end + 1;
            }
        }
    }
    char glass[8]{};
    if(FwCfgReadFile("opt/glass",glass,sizeof(glass)-1)) {
        if(glass[0]=='0'||(glass[0]=='o'&&glass[1]=='f'))appearance|=BROOK_APPEARANCE_PERFORMANCE;
        else if(glass[0]=='1'||(glass[0]=='o'&&glass[1]=='n'))appearance&=~BROOK_APPEARANCE_PERFORMANCE;
    }
    WmSetAppearance(appearance);
    WmSetTaskbarMetrics(taskbarMetrics);
    WmSetScale(selectedScale);
    WmSetTheme(selected);
}

static Window   g_windows[WM_MAX_WINDOWS] = {};
static bool     g_wmActive = false;
static int      g_focusedIdx = -1;
static uint64_t g_windowIdentity = 0;
static int g_taskbarPageTarget = -1;
static uint32_t g_nextZOrder = 1;  // 0 = backmost, higher = front

// Protected by g_vfbLock; only the published active bit is read lock-free by
// rendering. The lease is bound to the window creation identity, not its slot.
static int g_pointerCaptureIndex=-1;
static uint64_t g_pointerCaptureIdentity=0,g_pointerCaptureSequence=0,g_pointerCaptureDrops=0;
static uint64_t g_pointerReleaseSequence=0,g_pointerCaptureRevision=0;
static uint64_t g_pointerInteractionSerial=0;
static uint32_t g_pointerQuarantinedButtons=0;
// Physical-up recovery must retain ownership of a delayed or queued release.
static uint32_t g_pointerOrphanedReleases=0;
static uint32_t g_pointerCaptureToken=0,g_nextPointerCaptureToken=0,g_pointerCapturePublished=0;
static int32_t g_pointerAnchorX=0,g_pointerAnchorY=0;
static int16_t g_pointerLocalX=0,g_pointerLocalY=0;
static bool g_pointerEscapeHeld=false;

static void PointerCaptureReleaseLocked() {
    if(g_pointerCaptureIndex<0)return;
    g_pointerReleaseSequence=InputLatestSequence();
    __atomic_fetch_or(&g_pointerQuarantinedButtons,static_cast<uint32_t>(MouseGetButtons()),__ATOMIC_ACQ_REL);
    g_pointerCaptureIndex=-1;g_pointerCaptureIdentity=0;g_pointerCaptureToken=0;
    __atomic_store_n(&g_pointerCapturePublished,0u,__ATOMIC_RELEASE);
    __atomic_add_fetch(&g_pointerCaptureRevision,1,__ATOMIC_RELEASE);
    MouseSetPosition(g_pointerAnchorX,g_pointerAnchorY);
    CompositorMarkDirty();CompositorWake();
}
static void PointerCaptureValidateLocked() {
    if(g_pointerCaptureIndex<0)return;
    const auto& w=g_windows[g_pointerCaptureIndex];
    if(!g_wmActive || !w.proc || !w.vfb || !w.visible || w.minimized ||
       !w.focused || g_focusedIdx!=g_pointerCaptureIndex ||
       w.creationIdentity!=g_pointerCaptureIdentity ||
       InputDroppedEvents()!=g_pointerCaptureDrops || WmLauncherVisible())
        PointerCaptureReleaseLocked();
}
bool WmPointerCaptureActive() {
    return __atomic_load_n(&g_pointerCapturePublished,__ATOMIC_ACQUIRE)!=0;
}
uint64_t WmPointerCaptureRevision() { return __atomic_load_n(&g_pointerCaptureRevision,__ATOMIC_ACQUIRE); }
uint32_t WmPointerSuppressedButtons() { return __atomic_load_n(&g_pointerQuarantinedButtons,__ATOMIC_ACQUIRE); }
void WmPointerCaptureRefresh() {
    VfbGuard guard;PointerCaptureValidateLocked();
    // Recover a dropped Up without forwarding an already-queued (or not yet
    // published) Up to the next client. Fence published packets; retain a
    // release tombstone until Up or a new Down establishes a new button pair.
    const uint32_t recovered=WmPointerSuppressedButtons()&~static_cast<uint32_t>(MouseGetButtons());
    if(recovered) {
        g_pointerReleaseSequence=InputLatestSequence();
        g_pointerOrphanedReleases|=recovered;
        __atomic_fetch_and(&g_pointerQuarantinedButtons,~recovered,__ATOMIC_ACQ_REL);
    }
}
int64_t WmPointerCapture(Process* provider,uint32_t wmId,uint32_t operation,uint32_t token) {
    if(!provider || !wmId || wmId>WM_MAX_WINDOWS)return -22;
    VfbGuard guard;PointerCaptureValidateLocked();
    Window* w=WmFindWindowById(provider,wmId);
    if(!w)return -2;
    // Physical interaction proof for providers: queued WM events and new
    // protocol resources cannot manufacture a fresh post-release click.
    if(operation==3 && !token)return static_cast<int64_t>(g_pointerInteractionSerial);
    const bool owned=g_pointerCaptureIndex==static_cast<int>(wmId)-1 &&
                     g_pointerCaptureIdentity==w->creationIdentity;
    if(operation==0)return owned && (!token || token==g_pointerCaptureToken)?g_pointerCaptureToken:0;
    if(operation==2) {
        if(!owned || !token || token!=g_pointerCaptureToken)return -116;
        PointerCaptureReleaseLocked();return 0;
    }
    if(operation!=1 || token)return -22;
    if(owned)return g_pointerCaptureToken;
    if(g_pointerCaptureIndex>=0)return -16;
    if(!g_wmActive || !w->vfb || !w->visible || w->minimized || !w->focused ||
       g_focusedIdx!=static_cast<int>(wmId)-1 || WmLauncherVisible() || WmPointerSuppressedButtons())return -11;
    if(g_nextPointerCaptureToken==0xffffffffu)return -75;
    MouseGetPosition(&g_pointerAnchorX,&g_pointerAnchorY);
    g_pointerLocalX=static_cast<int16_t>(g_pointerAnchorX-w->clientX());
    g_pointerLocalY=static_cast<int16_t>(g_pointerAnchorY-w->clientY());
    g_pointerCaptureIndex=static_cast<int>(wmId)-1;
    g_pointerCaptureIdentity=w->creationIdentity;
    g_pointerCaptureSequence=InputLatestSequence();g_pointerCaptureDrops=InputDroppedEvents();
    g_pointerCaptureToken=++g_nextPointerCaptureToken;
    __atomic_store_n(&g_pointerCapturePublished,1u,__ATOMIC_RELEASE);
    __atomic_add_fetch(&g_pointerCaptureRevision,1,__ATOMIC_RELEASE);
    CompositorMarkDirty();CompositorWake();return g_pointerCaptureToken;
}

static bool PushRelativeLocked(Window* w,const InputEvent& event,uint32_t token) {
    int64_t dx=event.deltaX,dy=event.deltaY;
    while(dx || dy) {
        const int16_t x=static_cast<int16_t>(dx>32767?32767:dx<(-32768)?(-32768):dx);
        const int16_t y=static_cast<int16_t>(dy>32767?32767:dy<(-32768)?(-32768):dy);
        if(!WmInputPush(w,event,x,y,token))return false;
        dx-=x;dy-=y;
    }
    return true;
}
bool WmInputPushRelative(Window* window,const InputEvent& event) {
    VfbGuard guard;
    if(!window || !window->proc || g_pointerCaptureIndex>=0)return false;
    return PushRelativeLocked(window,event,0);
}
bool WmRouteCapturedPointer(const InputEvent& event) {
    VfbGuard guard;PointerCaptureValidateLocked();
    const bool pointer=event.type==InputEventType::MouseMove || event.type==InputEventType::MouseRelativeMove ||
        event.type==InputEventType::MouseButtonDown || event.type==InputEventType::MouseButtonUp ||
        event.type==InputEventType::MouseScroll;
    if(pointer && event.sequence<=g_pointerReleaseSequence)return true;
    if((event.type==InputEventType::MouseButtonDown || event.type==InputEventType::MouseButtonUp) && event.scanCode<32) {
        const uint32_t bit=uint32_t(1)<<event.scanCode;
        if(g_pointerOrphanedReleases&bit) {
            g_pointerOrphanedReleases&=~bit;
            if(event.type==InputEventType::MouseButtonUp)return true;
            // A new press after the recovery fence starts an ordinary pair.
        }
        if(WmPointerSuppressedButtons()&bit) {
            if(event.type==InputEventType::MouseButtonUp)
                __atomic_fetch_and(&g_pointerQuarantinedButtons,~bit,__ATOMIC_ACQ_REL);
            return true;
        }
    }
    if(g_pointerEscapeHeld && event.scanCode==1 &&
       (event.type==InputEventType::KeyPress || event.type==InputEventType::KeyRelease)) {
        if(event.type==InputEventType::KeyRelease)g_pointerEscapeHeld=false;
        return true;
    }
    if(g_pointerCaptureIndex<0) {
        if(event.type==InputEventType::MouseButtonDown && event.scanCode==0 &&
           g_pointerInteractionSerial<0x7fffffffffffffffull)++g_pointerInteractionSerial;
        return false;
    }
    if(event.type==InputEventType::KeyPress && event.scanCode==1 &&
       (event.modifiers&(INPUT_MOD_CTRL|INPUT_MOD_ALT))==(INPUT_MOD_CTRL|INPUT_MOD_ALT)) {
        g_pointerEscapeHeld=true;PointerCaptureReleaseLocked();return true;
    }
    if(!pointer)return false;
    // Never assign already-queued input to a newly granted lease.
    if(event.sequence<=g_pointerCaptureSequence)return true;
    Window* w=&g_windows[g_pointerCaptureIndex];
    if(event.type==InputEventType::MouseMove)return true; // absolute tablet has no raw motion
    const bool queued=event.type==InputEventType::MouseRelativeMove
        ?PushRelativeLocked(w,event,g_pointerCaptureToken)
        :WmInputPush(w,event,g_pointerLocalX,g_pointerLocalY,g_pointerCaptureToken);
    if(!queued)PointerCaptureReleaseLocked(); // loss must not silently corrupt a locked stream
    return true;
}

// Immutable launcher payloads. Only refs change after publication. The single
// maintenance worker owns building/pending/retired lifetimes; no renderer does
// VFS I/O or frees icon memory. The compositor alone swaps an active snapshot.
struct LauncherSnapshot {
    LauncherItem items[WM_LAUNCHER_MAX_ITEMS];
    uint32_t count;
    uint32_t generation;
    uint64_t builtTick;
    uint32_t refs;
    uint32_t* applicationIcon;
    uint32_t* terminalIcon;
    uint32_t* appsIcon;
};
static LauncherSnapshot g_emptyLauncher = {};
static LauncherSnapshot* g_launcherActive = &g_emptyLauncher;
static LauncherSnapshot* g_launcherPending = nullptr;
static LauncherSnapshot* g_launcherRetired = nullptr;
static SpinLock g_launcherLock;
static uint32_t g_launcherRequested = 1;
static bool g_launcherWorkerStarted = false;
static bool g_launcherLoaded = false;
static bool g_launcherOpen = false;
static int g_launcherError = 0;
static uint32_t g_launcherScroll = 0;
static void LauncherStartWorker();

// Requests own all metadata/icon pixels, never launcher snapshot pointers.
struct LauncherRequest {
    char command[128];
    bool desktop, hasIcon;
    uint32_t icon[LAUNCHER_ICON_PX * LAUNCHER_ICON_PX];
};
static SpinLock g_launchLock;
static LaunchFeedbackTable g_launches;
static LaunchHistory g_launchHistory;
static LauncherRequest g_launchRequests[LaunchFeedbackTable::Capacity];
static bool g_launchBusy = false;

bool WmLaunchBusy() { return __atomic_load_n(&g_launchBusy, __ATOMIC_ACQUIRE); }

void WmLaunchFeedbackPoll()
{
    static uint64_t lastPoll = 0;
    const uint64_t now = ApicTickCount();
    if (now - lastPoll < 100) return;
    lastPoll = now;
    // Window/process lookups may take sleeping/scheduler locks. Never hold the
    // feedback spinlock while acquiring them. Completion is versioned by token.
    bool hadEntries = false,needsObservation = false;
    SpinLockAcquire(&g_launchLock);
    for (const auto& e : g_launches.entries) hadEntries |= e.phase != LaunchPhase::Empty;
    for (const auto& r : g_launchHistory.records) needsObservation |= LaunchHistory::NeedsObservation(r,now);
    SpinLockRelease(&g_launchLock);
    if (!hadEntries && !needsObservation) {
        if (__atomic_exchange_n(&g_launchBusy, false, __ATOMIC_ACQ_REL)) CompositorMarkDirty();
        return;
    }
    {
        VfbGuard windows;
        for (const auto& w : g_windows) {
            if (!w.proc || !w.visible || !w.focusable) continue;
            SpinLockAcquire(&g_launchLock);
            g_launchHistory.Observe(w.clientLaunchId,w.clientOrigin==WmClientOrigin::RegisteredUnixPeer,
                w.creationIdentity,w.appId,w.wmId,w.creationIdentity,now);
            uint64_t token=w.clientLaunchId;
            if(w.clientOrigin==WmClientOrigin::RegisteredUnixPeer&&g_launches.Find(token)<0)token=0;
            g_launches.Window(token, w.creationIdentity, w.appId,nullptr,w.wmId,w.creationIdentity,now);
            SpinLockRelease(&g_launchLock);
        }
    }
    for (unsigned i = 0; i < LaunchFeedbackTable::Capacity; ++i) {
        LaunchFeedback entry;
        SpinLockAcquire(&g_launchLock);
        entry = g_launches.entries[i];
        SpinLockRelease(&g_launchLock);
        if (entry.phase == LaunchPhase::Empty) continue;
        SpinLockAcquire(&g_launchLock);
        // Absence of tagged processes is not a failed single-instance handoff.
        g_launches.Advance(entry.token, now, true);
        SpinLockRelease(&g_launchLock);
    }
    bool busy = false;
    SpinLockAcquire(&g_launchLock);
    for (const auto& e : g_launches.entries) busy |= e.busy()&&!e.windowObserved;
    SpinLockRelease(&g_launchLock);
    const bool busyChanged=__atomic_exchange_n(&g_launchBusy,busy,__ATOMIC_ACQ_REL)!=busy;
    if(hadEntries||busyChanged)CompositorMarkDirty(); // Also erase the final expired notice.
}

void WmTaskbarPendingClick(int target)
{
    const bool navigation = target <= -100;
    if (navigation) target += 90;
    if (target > -10 || target <= -10 - static_cast<int>(LaunchFeedbackTable::Capacity)) return;
    const unsigned slot = static_cast<unsigned>(-10 - target);
    if (!navigation) {
        SpinLockAcquire(&g_launchLock);
        g_launches.Dismiss(slot); // Active launch is not cancellation.
        SpinLockRelease(&g_launchLock);
    }
    g_taskbarPageTarget = target;
    CompositorMarkDirty();
}

class LauncherReadGuard {
    LauncherSnapshot* snapshot;
public:
    LauncherReadGuard() {
        SpinLockAcquire(&g_launcherLock);
        snapshot = g_launcherActive;
        __atomic_fetch_add(&snapshot->refs, 1u, __ATOMIC_RELAXED);
        SpinLockRelease(&g_launcherLock);
    }
    ~LauncherReadGuard() { __atomic_fetch_sub(&snapshot->refs, 1u, __ATOMIC_RELEASE); }
    const LauncherSnapshot& get() const { return *snapshot; }
    LauncherReadGuard(const LauncherReadGuard&) = delete;
    LauncherReadGuard& operator=(const LauncherReadGuard&) = delete;
};

uint32_t WmSnapshotDebugApps(WmDebugApp* out,uint32_t capacity,
                             uint32_t* generation,uint64_t* builtTick)
{
    LauncherReadGuard guard;const auto& catalog=guard.get();
    *generation=catalog.generation;*builtTick=catalog.builtTick;
    uint32_t count=0;
    for(uint32_t i=0;i<catalog.count&&count<capacity;++i) {
        const auto& item=catalog.items[i];auto& app=out[count++];app={};
        // Fixed arrays have identical extents; no live catalog pointers escape.
        memcpy(app.id,item.catalogId,sizeof(app.id));memcpy(app.title,item.title,sizeof(app.title));
        memcpy(app.desktopAppId,item.appId,sizeof(app.desktopAppId));
        memcpy(app.executable,item.executable,sizeof(app.executable));
        app.desktop=item.isDesktopEntry;app.eligible=item.valid;
    }
    return count;
}

void WmRequestLauncherRefresh()
{
    __atomic_fetch_add(&g_launcherRequested, 1u, __ATOMIC_RELEASE);
}

bool WmLauncherApplyPending()
{
    SpinLockAcquire(&g_launcherLock);
    if (!g_launcherPending || g_launcherRetired) {
        SpinLockRelease(&g_launcherLock);
        return false;
    }
    LauncherSnapshot* previous = g_launcherActive;
    g_launcherActive = g_launcherPending;
    g_launcherPending = nullptr;
    if (previous != &g_emptyLauncher) g_launcherRetired = previous;
    const uint32_t count = g_launcherActive->count;
    const uint32_t generation = g_launcherActive->generation;
    SpinLockRelease(&g_launcherLock);
    g_launcherScroll = 0;
    SerialPrintf("WM: launcher published generation=%u items=%u\n", generation, count);
    return true;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------



// Index 0 is close, 1 maximize/restore, 2 minimize. Rendering and input
// intentionally use the same full-height rectangular control cells.
static WmRect CaptionRect(const Window& w, int index)
{
    return DesktopCaptionRect({w.x,w.y,w.clientW,w.clientH,w.noChrome},w.desktopMetrics(),index);
}

static int WmTextWidth(const char* text)
{
    int width = 0;
    for (; *text; ++text)
    {
        uint32_t code = static_cast<uint8_t>(*text);
        if (code >= WmDesktopFont().firstChar && code < WmDesktopFont().firstChar + WmDesktopFont().glyphCount)
            width += WmDesktopFont().glyphs[code - WmDesktopFont().firstChar].advance;
    }
    return width;
}

static void WmStrCopy(char* dst, const char* src, uint32_t maxLen)
{
    uint32_t i = 0;
    while (src[i] && i < maxLen - 1) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

// Pixel write into buffer with bounds check
static inline void WmPutPixel(uint32_t* buf, uint32_t stride,
                               uint32_t screenW, uint32_t screenH,
                               int x, int y, uint32_t color)
{
    if (x < 0 || y < 0) return;
    if (static_cast<uint32_t>(x) >= screenW) return;
    if (static_cast<uint32_t>(y) >= screenH) return;
    buf[y * stride + x] = color;
}

// Fill a rectangle
static void WmFillRect(uint32_t* buf, uint32_t stride,
                        uint32_t screenW, uint32_t screenH,
                        int x0, int y0, int w, int h, uint32_t color)
{
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x)
            WmPutPixel(buf, stride, screenW, screenH, x, y, color);
}

// Render a single glyph from g_fontAtlas at (penX, penY) into buffer.
// Returns advance width. Clips pixels to [0, clipRight) horizontally.
static int WmRenderGlyph(uint32_t* buf, uint32_t stride,
                          uint32_t screenW, uint32_t screenH,
                          int penX, int penY, int code,
                          uint32_t fg, uint32_t bg,
                          int clipRight = 0, const WmRect* clip = nullptr)
{
    const FontAtlas& fa = WmDesktopFont();
    if (code < static_cast<int>(fa.firstChar) ||
        code >= static_cast<int>(fa.firstChar + fa.glyphCount))
        return 0;

    const GlyphInfo& gi = fa.glyphs[code - static_cast<int>(fa.firstChar)];
    int gw = gi.atlasX1 - gi.atlasX0;
    int gh = gi.atlasY1 - gi.atlasY0;

    int drawX = penX + gi.bearingX;
    int drawY = penY + fa.ascent - gi.bearingY;

    int maxX = (clipRight > 0) ? clipRight : static_cast<int>(screenW);

    for (int row = 0; row < gh; ++row)
    {
        for (int col = 0; col < gw; ++col)
        {
            uint8_t cov = fa.pixels[(gi.atlasY0 + row) * static_cast<int>(fa.atlasWidth)
                                     + (gi.atlasX0 + col)];
            if (cov == 0) continue;

            int px = drawX + col;
            int py = drawY + row;
            if (px < 0 || py < 0) continue;
            if (clip && !clip->contains(px,py)) continue;
            if (px >= maxX) continue;
            if (static_cast<uint32_t>(px) >= screenW) continue;
            if (static_cast<uint32_t>(py) >= screenH) continue;

            // Preserve foreground coverage over a translucent material fill.
            // Legacy RGB buffers have alpha zero and remain fully opaque.
            (void)bg;
            auto& destination=buf[py*stride+px];
            destination=ChromePixelOver(destination,(uint32_t(cov)<<24)|(fg&0xffffff));
        }
    }

    return gi.advance;
}

// Render a string, optionally clipped to maxWidth pixels from starting x.
// maxWidth <= 0 means no clipping.
static void WmRenderString(uint32_t* buf, uint32_t stride,
                            uint32_t screenW, uint32_t screenH,
                            int x, int y, const char* str,
                            uint32_t fg, uint32_t bg,
                            int maxWidth = 0, const WmRect* clip = nullptr)
{
    int penX = x;
    int clipRight = (maxWidth > 0) ? (x + maxWidth) : 0;
    while (*str)
    {
        if (maxWidth > 0 && (penX - x) >= maxWidth) break;
        penX += WmRenderGlyph(buf, stride, screenW, screenH,
                               penX, y, static_cast<uint8_t>(*str), fg, bg,
                               clipRight, clip);
        ++str;
    }
}

// Keep pale launcher labels legible when a low-opacity panel crosses bright
// client content. The narrow outline follows glyph coverage, not a solid box.
static void WmRenderMaterialString(uint32_t* buf,uint32_t stride,uint32_t sw,uint32_t sh,
                                  int x,int y,const char* text,uint32_t fg,uint32_t bg,int width=0)
{
    const unsigned brightness=(((fg>>16)&255)+((fg>>8)&255)+(fg&255))/3;
    if(CompositorGlassActive() && CompositorMaterialOpacity()<180 && brightness>128){
        const int offset=WmMetric(1);
        WmRenderString(buf,stride,sw,sh,x-offset,y,text,0x071526,bg,width);
        WmRenderString(buf,stride,sw,sh,x+offset,y,text,0x071526,bg,width);
        WmRenderString(buf,stride,sw,sh,x,y-offset,text,0x071526,bg,width);
        WmRenderString(buf,stride,sw,sh,x,y+offset,text,0x071526,bg,width);
    }
    WmRenderString(buf,stride,sw,sh,x,y,text,fg,bg,width);
}

static bool WmEqualI(const char* a, const char* b)
{
    if (!a || !b) return false;
    while (*a && *b)
    {
        char ac = *a++, bc = *b++;
        if (ac >= 'A' && ac <= 'Z') ac += 'a' - 'A';
        if (bc >= 'A' && bc <= 'Z') bc += 'a' - 'A';
        if (ac != bc) return false;
    }
    return !*a && !*b;
}

static const char* WmBaseName(const char* path)
{
    const char* name = path;
    for (; *path; ++path) if (*path == '/') name = path + 1;
    return name;
}

static bool WmTitleHasAppName(const char* title, const char* name)
{
    if (!name[0]) return false;
    for (const char* start = title; *start; ++start)
    {
        if (start != title && start[-1] != ' ' && start[-1] != '-') continue;
        const char *a = start, *b = name;
        while (*a && *b)
        {
            char ac = *a, bc = *b;
            if (ac >= 'A' && ac <= 'Z') ac += 'a' - 'A';
            if (bc >= 'A' && bc <= 'Z') bc += 'a' - 'A';
            if (ac != bc) break;
            ++a; ++b;
        }
        if (!*b && (!*a || *a == ' ' || *a == '-')) return true;
    }
    return false;
}

static const uint32_t* WindowIcon(const Window& w, const LauncherSnapshot& launcher)
{
    if (WmEqualI(w.appId, "terminal")) return launcher.terminalIcon ? launcher.terminalIcon : launcher.applicationIcon;
    // Explicit desktop identity takes precedence over executable/name guesses.
    for (uint32_t i = 0; i < launcher.count; ++i)
    {
        const LauncherItem& item = launcher.items[i];
        if (item.valid && item.iconPixels && w.appId[0] && item.appId[0] && WmEqualI(w.appId, item.appId))
            return item.iconPixels;
    }
    const char* executable = w.proc ? WmBaseName(w.proc->exePath) : "";
    for (uint32_t i = 0; i < launcher.count; ++i)
    {
        const LauncherItem& item = launcher.items[i];
        if (!item.valid || !item.iconPixels || !item.executable[0]) continue;
        if ((w.appId[0] && WmEqualI(w.appId, item.executable)) ||
            (executable[0] && WmEqualI(executable, item.executable))) return item.iconPixels;
    }
    // Legacy native windows have no app_id. Their title is only a last resort;
    // an unknown explicit ID must not acquire another app's icon by title.
    if (!w.appId[0])
    {
        if (w.proc && TerminalFindByProcess(w.proc)) return launcher.terminalIcon ? launcher.terminalIcon : launcher.applicationIcon;
        for (uint32_t i = 0; i < launcher.count; ++i)
            if (launcher.items[i].valid && launcher.items[i].iconPixels &&
                WmTitleHasAppName(w.title, launcher.items[i].title)) return launcher.items[i].iconPixels;
    }
    return launcher.applicationIcon;
}

// Shared straight-alpha ARGB renderer. No VFS access or ownership transfer in
// the compositor path; icons are immutable for the launcher's lifetime.
static void WmDrawIcon(uint32_t* buf, uint32_t stride, uint32_t screenW, uint32_t screenH,
                       int x, int y, int size, const uint32_t* pixels)
{
    if (pixels)
    {
        for (int row = 0; row < size; ++row)
            for (int col = 0; col < size; ++col)
            {
                const int px = x + col, py = y + row;
                if (px < 0 || py < 0 || px >= static_cast<int>(screenW) || py >= static_cast<int>(screenH)) continue;
                const uint32_t src = pixels[(row * LAUNCHER_ICON_PX / size) * LAUNCHER_ICON_PX + col * LAUNCHER_ICON_PX / size];
                if(!(src>>24))continue;
                auto& destination=buf[py*stride+px];
                destination=ChromePixelOver(destination,src);
            }
        return;
    }
    // Missing asset: a neutral application-window glyph, never a misleading
    // letter tile or a guessed application logo.
    WmFillRect(buf, stride, screenW, screenH, x + 2, y + 3, size - 4, size - 6, WmCurrentPalette().mutedText);
    WmFillRect(buf, stride, screenW, screenH, x + 3, y + 7, size - 6, size - 11, WmCurrentPalette().itemHover);
    WmFillRect(buf, stride, screenW, screenH, x + 5, y + 5, 2, 1, WmCurrentPalette().titleInactive);
}

// ---------------------------------------------------------------------------
// API implementation
// ---------------------------------------------------------------------------

void WmInit()
{
    if (g_wmActive)
    {
        // Idempotent: launcher shortcuts that include `set wm` for first-run
        // boot scripts must not nuke the existing window table when invoked
        // a second time. Without this guard, every subsequent app launch
        // would wipe every existing window's `proc` pointer (leaving the
        // owning processes orphaned with no chrome and never reaped).
        SerialPuts("WM: WmInit() called again — already initialised, skipping\n");
        return;
    }
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
        g_windows[i].proc = nullptr;
    g_focusedIdx = -1;
    g_nextZOrder = 1;
    SerialPuts("WM: initialised\n");
}

bool WmIsActive()
{
    return g_wmActive;
}

static void SetWmActiveLocked(bool active) {
    g_wmActive=active;
    if(!active)PointerCaptureReleaseLocked();
}
void WmSetActive(bool active)
{
    if (active) WmRestoreTheme();
    if (active && !g_launcherLoaded) WmLauncherLoad();
    if (active) LauncherStartWorker();
    { VfbGuard guard;SetWmActiveLocked(active); }
    if (active)
        SerialPuts("WM: window manager mode enabled\n");
}

static int CreateWindowLocked(Process* proc, int16_t x, int16_t y,
                   uint16_t clientW, uint16_t clientH,
                   const char* title, uint8_t upscale,
                   bool focusable)
{
    // Find free slot
    int idx = -1;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
    {
        if (g_windows[i].proc == nullptr)
        {
            idx = static_cast<int>(i);
            break;
        }
    }
    if (idx < 0) return -1;

    Window& w = g_windows[idx];
    w.proc = proc;
    w.metrics = &WmDesktopMetrics();
    w.creationIdentity = __atomic_add_fetch(&g_windowIdentity, 1, __ATOMIC_ACQ_REL);
    w.publicationGeneration=w.publishedGeneration=1;
    w.clientPid=proc?(proc->tgid?proc->tgid:proc->pid):0;
    w.clientIncarnation=proc?(proc->isThread?proc->leaderIncarnation:proc->incarnation):0;
    w.clientLaunchId=proc?proc->launchId:0;
    w.clientOrigin=proc?WmClientOrigin::NativeProcess:WmClientOrigin::Unknown;
    w.appId[0] = 0;
    w.x = x;
    w.y = y;
    w.clientW = clientW;
    w.clientH = clientH;
    w.zOrder = g_nextZOrder++;
    w.upscale = (upscale >= 1) ? upscale : 1;
    w.opacity = 255;
    w.blurRadius = 0;
    w.state = WindowState::Normal;
    w.focused = false;
    w.visible = true;
    w.minimized = false;
    w.noChrome = false;
    w.focusable = focusable;
    w.savedX = x;
    w.savedY = y;
    w.savedW = clientW;
    w.savedH = clientH;
    w.vfb       = nullptr;
    w.vfbStride = 0;
    w.vfbWidth=w.vfbHeight=0;
    w.vfbBytes  = 0;
    w.vfbUser   = nullptr;
    w.vfbAddressSpace = nullptr;
    w.vfbBacking = nullptr;
    w.vfbRetirement = nullptr;
    w.vfbDirty  = 0;
    // A reused slot must not keep the previous client's dmabuf. Otherwise a
    // new SHM window keeps displaying the dead GL client's last frame.
    w.externalCaptured = false;
    w.externalW = w.externalH = 0;
    w.wmId      = static_cast<uint16_t>(idx + 1);
    if (proc) {proc->fbDestX=w.clientX();proc->fbDestY=w.clientY();}
    w.inputHead = 0;
    w.inputTail = 0;
    w.inputDropCount = 0;
    WmStrCopy(w.title, title ? title : "Window", sizeof(w.title));

    SerialPrintf("WM: created window %d '%s' at (%d,%d) %ux%u scale=%u for pid %u\n",
                 idx, w.title, x, y, clientW, clientH, upscale, proc ? proc->pid : 0);

    // Auto-focus normal windows. Popup/tooltips are raised by z-order but keep
    // keyboard focus on the invoking toplevel. Non-focusable windows (popups,
    // menus) already received the highest z-order above, so they'll render
    // on top of existing windows without stealing keyboard focus.
    if (w.focusable)
        FocusWindowLocked(idx);

    return idx;
}

int WmCreateWindow(Process* proc,int16_t x,int16_t y,uint16_t clientW,uint16_t clientH,
                   const char* title,uint8_t upscale,bool focusable) {
    VfbGuard guard;
    return CreateWindowLocked(proc,x,y,clientW,clientH,title,upscale,focusable);
}

static void DestroyWindowLocked(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    Window& w = g_windows[idx];
    if (!w.proc) return;

    if(g_pointerCaptureIndex==idx)PointerCaptureReleaseLocked();
    SerialPrintf("WM: destroyed window %d '%s'\n", idx, w.title);

    // Detach the per-window VFB (if any).  The compositor may already have
    // snapshotted w.vfb for the current frame, so kernel pages are freed only
    // after a later compositor epoch.
    if (w.vfb)
    {
        auto* retirement=w.vfbRetirement;
        w.vfbRetirement=nullptr;
        AddressSpaceState* space = w.vfbAddressSpace;
        auto* backing=w.vfbBacking;
        w.vfbBacking = nullptr;
        w.vfbAddressSpace = nullptr;
        w.vfb       = nullptr;
        w.vfbStride = 0;
        w.vfbWidth=w.vfbHeight=0;
        w.vfbBytes  = 0;
        w.vfbUser   = nullptr;
        w.vfbDirty  = 0;
        // Buffer-specific removal uses preallocated retirement counters and
        // cannot fail allocation. Complete AS cleanup before renderer retirement.
        if (space && backing && UserMappingRemoveBacking(ProcessCurrent(),space,backing)<0)
            KernelPanic("WM: invalid framebuffer mappings during destruction");
        MappingBackingUnref(backing);AddressSpaceUnref(space);
        CompositorRetireVfb(retirement);
    }
    w.externalCaptured = false;
    w.externalW = w.externalH = 0;
    w.wmId = 0;
    w.proc = nullptr;
    w.visible = false;

    if (g_focusedIdx == idx)
    {
        g_focusedIdx = -1;
        // Focus the next highest z-order window
        int bestIdx = -1;
        uint8_t bestZ = 0;
        for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
        {
            if (g_windows[i].proc && g_windows[i].visible && g_windows[i].focusable
                && g_windows[i].zOrder >= bestZ)
            {
                bestZ = g_windows[i].zOrder;
                bestIdx = static_cast<int>(i);
            }
        }
        if (bestIdx >= 0) FocusWindowLocked(bestIdx);
    }
    extern void CompositorMarkDirty();
    CompositorMarkDirty();
}

void WmDestroyWindow(int idx) {
    VfbGuard guard;DestroyWindowLocked(idx);
}

void WmDestroyWindowForProcess(Process* proc)
{
    if (!proc) return;
    VfbGuard guard;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
    {
        if (g_windows[i].proc == proc)
        {
            DestroyWindowLocked(static_cast<int>(i));
            // Don't return — a single process may own multiple windows
            // (e.g. waylandd hosting several xdg_toplevels).
        }
    }
}

int WmFindWindowForProcess(Process* proc)
{
    if (!proc) return -1;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
    {
        if (g_windows[i].proc == proc)
            return static_cast<int>(i);
    }
    return -1;
}

WmHitResult WmHitTest(int32_t mx, int32_t my)
{
    WmHitResult result = { -1, WmHitZone::None };

    // Collect sorted indices
    int sorted[WM_MAX_WINDOWS];
    uint32_t count = WmGetZOrder(sorted, WM_MAX_WINDOWS);

    // Walk front to back (last in z-order array = frontmost)
    for (int i = static_cast<int>(count) - 1; i >= 0; --i)
    {
        int idx = sorted[i];
        const Window& w = g_windows[idx];
        if (!w.proc || !w.visible || w.minimized) continue;

        int wx = w.x;
        int wy = w.y;
        int ww = w.outerWidth();
        int wh = w.outerHeight();

        // Check if mouse is within outer bounds (extended by grab zone for resize).
        // CSD windows have no kernel-managed chrome or resize zone; treat them
        // strictly by their outer (== client) bounds.
        int grab = w.noChrome ? 0 : static_cast<int>(WmMetric(WM_RESIZE_EDGE));
        if (mx < wx - grab || my < wy || mx >= wx + ww + grab || my >= wy + wh + grab)
            continue;

        // CSD: no chrome zones, no edge resize.  The whole window is client.
        if (w.noChrome)
        {
            result.windowIndex = idx;
            result.zone = WmHitZone::ClientArea;
            return result;
        }

        // If click is outside the visual window but inside grab zone,
        // treat as edge resize
        bool outsideRight  = (mx >= wx + ww);
        bool outsideBottom = (my >= wy + wh);
        if (outsideRight || outsideBottom)
        {
            result.windowIndex = idx;
            if (outsideRight && outsideBottom)
                result.zone = WmHitZone::ResizeCorner;
            else if (outsideRight)
                result.zone = WmHitZone::ResizeRight;
            else
                result.zone = WmHitZone::ResizeBottom;
            return result;
        }

        // We hit this window
        result.windowIndex = idx;

        int relX = mx - wx;
        int relY = my - wy;

        const WmHitZone captionZones[] = {WmHitZone::CloseButton,
            WmHitZone::MaximizeButton, WmHitZone::MinimizeButton};
        for (int button = 0; button < 3; ++button)
        {
            if (CaptionRect(w, button).contains(mx, my))
            {
                result.zone = captionZones[button];
                return result;
            }
        }
        if (relY < static_cast<int>(WmMetric(WM_BORDER_WIDTH) + WmMetric(WM_TITLE_BAR_HEIGHT)))
        {
            result.zone = WmHitZone::TitleBar;
            return result;
        }

        // Resize corner (bottom-right grab zone)
        if (relX >= ww - static_cast<int>(WmMetric(WM_RESIZE_GRAB)) &&
            relY >= wh - static_cast<int>(WmMetric(WM_RESIZE_GRAB)))
        {
            result.zone = WmHitZone::ResizeCorner;
            return result;
        }

        // Resize edges (bottom and right, wider than visual border)
        if (relY >= wh - static_cast<int>(WmMetric(WM_RESIZE_EDGE)))
        {
            result.zone = WmHitZone::ResizeBottom;
            return result;
        }
        if (relX >= ww - static_cast<int>(WmMetric(WM_RESIZE_EDGE)))
        {
            result.zone = WmHitZone::ResizeRight;
            return result;
        }

        // Border
        if (relX < static_cast<int>(WmMetric(WM_BORDER_WIDTH)) ||
            relX >= ww - static_cast<int>(WmMetric(WM_BORDER_WIDTH)) ||
            relY >= wh - static_cast<int>(WmMetric(WM_BORDER_WIDTH)))
        {
            result.zone = WmHitZone::Border;
            return result;
        }

        // Client area
        result.zone = WmHitZone::ClientArea;
        return result;
    }

    return result;
}

static void FocusWindowLocked(int idx)
{
    g_taskbarPageTarget = -1;
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    if (!g_windows[idx].proc) return;
    if (!g_windows[idx].focusable) return;
    if (g_focusedIdx == idx) return;

    PointerCaptureReleaseLocked();
    // Unfocus old
    if (g_focusedIdx >= 0 && g_focusedIdx < static_cast<int>(WM_MAX_WINDOWS))
    {
        Window& old = g_windows[g_focusedIdx];
        old.focused = false;
        if (old.proc)
            WmPushWmEvent(&old, WM_EVT_FOCUS_LOST, 0, 0);
    }

    // Focus and raise new.  Push FOCUS_GAINED unconditionally, even if
    // the VFB isn't attached yet -- a freshly-created Wayland window
    // attaches its buffer some time after WM_CREATE_WINDOW returns, and
    // waylandd needs the focus event to drive wl_keyboard.enter so the
    // client believes it has the keyboard.  Without this, no keystroke
    // ever reaches a newly-opened window.
    g_windows[idx].focused = true;
    g_windows[idx].zOrder = g_nextZOrder++;
    g_focusedIdx = idx;

    // Re-raise any non-focusable windows (popups, menus) belonging to the
    // same process so they stay above the newly-raised parent.
    Process* focusProc = g_windows[idx].proc;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
    {
        if (static_cast<int>(i) == idx) continue;
        Window& sib = g_windows[i];
        if (sib.proc == focusProc && sib.visible && !sib.focusable)
            sib.zOrder = g_nextZOrder++;
    }

    WmPushWmEvent(&g_windows[idx], WM_EVT_FOCUS_GAINED, 0, 0);
    CompositorMarkDirty();
    CompositorWake();
}

void WmSetFocus(int idx) { VfbGuard guard;FocusWindowLocked(idx); }

void WmSendToBack(int idx)
{
    VfbGuard guard;
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    if (!g_windows[idx].proc) return;

    // Renumber all z-orders to avoid collision: get sorted order, then
    // assign sequential values starting from 1, placing idx at 0.
    int sorted[WM_MAX_WINDOWS];
    uint32_t count = 0;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i) {
        if (g_windows[i].proc && (int)i != idx)
            sorted[count++] = (int)i;
    }
    // Sort by current z-order (ascending)
    for (uint32_t i = 1; i < count; ++i) {
        int key = sorted[i];
        uint8_t keyZ = g_windows[key].zOrder;
        int j = (int)i - 1;
        while (j >= 0 && g_windows[sorted[j]].zOrder > keyZ) {
            sorted[j + 1] = sorted[j];
            --j;
        }
        sorted[j + 1] = key;
    }
    // Assign: target gets 0, others get 1..count
    g_windows[idx].zOrder = 0;
    for (uint32_t i = 0; i < count; ++i)
        g_windows[sorted[i]].zOrder = (uint8_t)(i + 1);
    g_nextZOrder = (uint8_t)(count + 1);
    CompositorMarkDirty();
    CompositorWake();
}

int WmGetFocusedWindow()
{
    return g_focusedIdx;
}

Window* WmGetWindow(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return nullptr;
    if (!g_windows[idx].proc) return nullptr;
    return &g_windows[idx];
}

void WmSetClientSideDecoration(int idx, bool enable)
{
    VfbGuard guard;
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    Window& w = g_windows[idx];
    if (!w.proc) return;
    if (w.noChrome == enable) return;
    w.noChrome = enable;
    // Mark compositor dirty so the chrome region repaints/clears next frame.
    extern void CompositorMarkDirty();
    CompositorMarkDirty();
    SerialPrintf("WM: window %d CSD=%d\n", idx, (int)enable);
}

// Set per-window display properties (opacity and/or backdrop blur radius). `mask`
// selects which fields to apply: bit 0 = opacity, bit 1 = blurRadius. Marks the
// compositor dirty so the change is composited next frame. The GPU DRAW path
// reads w.opacity (and, for frosted glass, w.blurRadius).
void WmSetWindowProperties(int idx, uint32_t mask, uint8_t opacity, uint8_t blurRadius)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    Window& w = g_windows[idx];
    if (!w.proc) return;
    bool changed = false;
    if ((mask & WM_PROP_OPACITY) && w.opacity != opacity)    { w.opacity = opacity; changed = true; }
    if ((mask & WM_PROP_BLUR)    && w.blurRadius != blurRadius) { w.blurRadius = blurRadius; changed = true; }
    if (!changed) return;
    extern void CompositorMarkDirty();
    CompositorMarkDirty();
}

uint32_t WmWindowCount()
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
        if (g_windows[i].proc) ++n;
    return n;
}

static void SetMaximizedLocked(int idx, bool enable)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    Window& w = g_windows[idx];
    if (!w.proc) return;
    if ((w.state == WindowState::Maximized) == enable) return;

    uint32_t screenW, screenH;
    CompositorGetPhysDims(&screenW, &screenH);

    uint16_t newW, newH;
    const auto oldState=w.state;
    const auto oldX=w.x,oldY=w.y;
    const auto oldW=w.clientW,oldH=w.clientH;
    const auto savedX=w.savedX,savedY=w.savedY;
    const auto savedW=w.savedW,savedH=w.savedH;

    if (w.state == WindowState::Normal)
    {
        const int64_t width = static_cast<int64_t>(screenW) - (w.noChrome ? 0 : 2 * w.metric(WM_BORDER_WIDTH));
        const int64_t height = static_cast<int64_t>(WmDesktopHeight(screenH)) -
            (w.noChrome ? 0 : w.metric(WM_TITLE_BAR_HEIGHT) + 2 * w.metric(WM_BORDER_WIDTH));
        if (width < WM_MIN_WIDTH || height < WM_MIN_HEIGHT || width > 65535 || height > 65535) return;
        // Save current geometry
        w.savedX = w.x;
        w.savedY = w.y;
        w.savedW = w.clientW;
        w.savedH = w.clientH;

        // Maximize: fill desktop area (screen minus taskbar) minus chrome.
        // CSD windows have no chrome, so the client area takes the full space.
        w.x = 0;
        w.y = 0;
        newW = static_cast<uint16_t>(width);
        newH = static_cast<uint16_t>(height);
        w.state = WindowState::Maximized;
    }
    else
    {
        // Restore
        const auto restored = ReframeDesktopWindow(
            {w.savedX,w.savedY,w.savedW,w.savedH,w.noChrome},
            w.desktopMetrics(),w.desktopMetrics(),screenW,WmDesktopHeight(screenH),
            !w.noChrome && w.focusable);
        w.x = static_cast<int16_t>(restored.x);
        w.y = static_cast<int16_t>(restored.y);
        newW = w.savedW;
        newH = w.savedH;
        w.state = WindowState::Normal;
    }

    // Route through WmResizeWindow so terminal VFBs / SIGWINCH paths fire.
    // Without this, chrome grows but the inner VFB stays the old size.
    if (!ResizeWindowLocked(idx,newW,newH)) {
        w.x=oldX;w.y=oldY;w.clientW=oldW;w.clientH=oldH;w.state=oldState;
        w.savedX=savedX;w.savedY=savedY;w.savedW=savedW;w.savedH=savedH;
    }
    w.proc->fbDestX=w.clientX();w.proc->fbDestY=w.clientY();
}

void WmSetMaximized(int idx,bool enable) { VfbGuard guard;SetMaximizedLocked(idx,enable); }

void WmToggleMaximize(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    Window& w = g_windows[idx];
    if (!w.proc) return;
    WmSetMaximized(idx, w.state != WindowState::Maximized);
}

static void MoveWindowLocked(int idx, int16_t newX, int16_t newY)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    Window& w = g_windows[idx];
    if (!w.proc) return;
    // CSD can maximize during a title drag; its old offset must not move the
    // new desktop-sized surface away from the origin.
    if (w.state == WindowState::Maximized) return;
    w.x = newX;
    w.y = newY;
    w.proc->fbDestX=w.clientX();w.proc->fbDestY=w.clientY();
    extern void CompositorMarkDirty();
    CompositorMarkDirty();
}

void WmMoveWindow(int idx, int16_t newX, int16_t newY)
{
    VfbGuard guard;
    MoveWindowLocked(idx,newX,newY);
}

bool WmMoveWindowRelativeToParent(Process* proc, uint32_t wmId,
                                  uint32_t parentWmId,
                                  int32_t relX, int32_t relY)
{
    VfbGuard guard;
    Window* child = WmFindWindowById(proc, wmId);
    Window* parent = WmFindWindowById(proc, parentWmId);
    if (!child || !parent) return false;

    int32_t clientX = parent->clientX();
    int32_t clientY = parent->clientY();
    int32_t x = clientX + relX;
    int32_t y = clientY + relY;
    static constexpr int32_t kMinI16 = -32768;
    static constexpr int32_t kMaxI16 = 32767;
    if (x < kMinI16) x = kMinI16;
    if (x > kMaxI16) x = kMaxI16;
    if (y < kMinI16) y = kMinI16;
    if (y > kMaxI16) y = kMaxI16;

    MoveWindowLocked(static_cast<int>(wmId) - 1, static_cast<int16_t>(x), static_cast<int16_t>(y));
    return true;
}

int WmPlaceOwnedWindow(Process* proc, uint32_t wmId, uint64_t incarnation,
                       int32_t clientX, int32_t clientY)
{
    if (!proc || !incarnation) return -22;
    VfbGuard guard;
    Window* w = WmFindWindowById(proc, wmId);
    if (!w) return -2;
    if (w->creationIdentity != incarnation) return -116;
    const int64_t x = static_cast<int64_t>(clientX) - (w->noChrome ? 0 : w->metric(WM_BORDER_WIDTH));
    const int64_t y = static_cast<int64_t>(clientY) - (w->noChrome ? 0 : w->metric(WM_TITLE_BAR_HEIGHT)+w->metric(WM_BORDER_WIDTH));
    if (x < -32768 || x > 32767 || y < -32768 || y > 32767) return -22;
    if (w->x != x || w->y != y) {
        MoveWindowLocked(static_cast<int>(wmId)-1, static_cast<int16_t>(x), static_cast<int16_t>(y));
        CompositorWake();
    }
    return 0;
}

static bool ResizeWindowLocked(int idx, uint16_t newClientW, uint16_t newClientH)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return false;
    Window& w = g_windows[idx];
    if (!w.proc) return false;
    if (w.vfb) {
        const uint32_t head=__atomic_load_n(&w.inputHead,__ATOMIC_ACQUIRE);
        const uint32_t tail=__atomic_load_n(&w.inputTail,__ATOMIC_ACQUIRE);
        if ((head+1)%Window::WM_INPUT_QUEUE==tail) return false;
    }

    // Enforce minimum size
    if (newClientW < WM_MIN_WIDTH)  newClientW = WM_MIN_WIDTH;
    if (newClientH < WM_MIN_HEIGHT) newClientH = WM_MIN_HEIGHT;

    // Keep exact ownership and geometry stable across terminal preparation.
    // Both locks sleep; terminal state never acquires the window guard.
    auto terminal = TerminalResizeForProcess(w.proc->pid,w.proc->incarnation,newClientW,newClientH);
    if (terminal == TerminalResizeResult::Failed) return false;
    NextPublication(w);
    w.clientW = newClientW;
    w.clientH = newClientH;
    if (terminal == TerminalResizeResult::Resized)
    {
        // Backing pixels and SIGWINCH were committed by the exact owner.
    }
    else if (w.vfb)
    {
        // WM-API window (waylandd-hosted toplevel etc): notify the client
        // so it can reallocate a buffer at the new size and re-commit.
        // The compositor blit clamps to the existing VFB until the new
        // buffer arrives, so the visible chrome grows immediately and
        // content "fills in" once the client acknowledges the configure.
        SerialPrintf("WM: resize wm_id=%u '%s' -> %ux%u (vfb %ux%u stride=%u)"
                     " — pushing WM_EVT_RESIZED\n",
                     w.wmId, w.title, newClientW, newClientH,
                     w.clientW, w.clientH, w.vfbStride);
        WmPushWmEvent(&w,
                      WM_EVT_RESIZED,
                      static_cast<int16_t>(newClientW),
                      static_cast<int16_t>(newClientH));
    }
    else
    {
        // Legacy per-process VFB (not WM-API): stays the same size,
        // upscale handles it.
    }
    extern void CompositorMarkDirty();
    CompositorMarkDirty();
    return true;
}

bool WmResizeWindow(int idx,uint16_t newClientW,uint16_t newClientH)
{
    VfbGuard guard;
    return ResizeWindowLocked(idx,newClientW,newClientH);
}

bool WmApplyScalePending()
{
    const auto* next=DesktopMetricsFor(WmGetRequestedScale());
    if (!next || next==&WmDesktopMetrics()) return false;
    uint32_t sw,sh;CompositorGetPhysDims(&sw,&sh);
    VfbGuard guard;
    const int desktopH=sh>static_cast<uint32_t>(next->taskbar()) ? sh-next->taskbar() : 0;
    for (auto& w:g_windows) {
        if (!w.proc) continue;
        const auto& before=w.desktopMetrics();
        auto rect=ReframeDesktopWindow({w.x,w.y,w.clientW,w.clientH,w.noChrome},before,*next,sw,desktopH,!w.noChrome && w.focusable);
        auto saved=ReframeDesktopWindow({w.savedX,w.savedY,w.savedW,w.savedH,w.noChrome},before,*next,sw,desktopH,false);
        w.metrics=next;w.x=static_cast<int16_t>(rect.x);w.y=static_cast<int16_t>(rect.y);
        w.savedX=static_cast<int16_t>(saved.x);w.savedY=static_cast<int16_t>(saved.y);
        if (w.state==WindowState::Maximized) {
            const int border=w.noChrome ? 0 : next->border();
            const int title=w.noChrome ? 0 : next->title();
            const int width=static_cast<int>(sw)-2*border,height=desktopH-title-2*border;
            if (width>=static_cast<int>(WM_MIN_WIDTH) && height>=static_cast<int>(WM_MIN_HEIGHT) && width<=65535 && height<=65535 && ResizeWindowLocked(static_cast<int>(&w-g_windows),width,height)) {
                w.x=w.y=0;
            } else {
                // Keep the existing owned pixels and make the window movable
                // if memory cannot satisfy a new maximized backing allocation.
                w.state=WindowState::Normal;
                SerialPrintf("WM: scale resize failed for window %u; preserving client buffer\n",w.wmId);
            }
        }
        w.proc->fbDestX=w.clientX();w.proc->fbDestY=w.clientY();
    }
    __atomic_store_n(&g_desktopMetrics,next,__ATOMIC_RELEASE);
    g_launcherScroll=0;
    CompositorMarkDirty();
    SerialPrintf("WM: desktop scale applied %u%%\n",next->percent);
    return true;
}

static int NextFocusCandidateLocked(int excluded)
{
    int best=-1;uint8_t bestZ=0;
    for(uint32_t i=0;i<WM_MAX_WINDOWS;++i) {
        const auto& w=g_windows[i];
        if(static_cast<int>(i)!=excluded && w.proc && w.visible && !w.minimized && w.focusable && w.zOrder>=bestZ) {
            bestZ=w.zOrder;best=static_cast<int>(i);
        }
    }
    return best;
}

static void MinimizeWindowLocked(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    Window& w = g_windows[idx];
    if (!w.proc || w.minimized) return;

    if(g_pointerCaptureIndex==idx)PointerCaptureReleaseLocked();
    w.minimized = true;
    SerialPrintf("WM: minimized window %d '%s'\n", idx, w.title);

    // If this was focused, focus the next visible window
    if (g_focusedIdx == idx)
    {
        g_focusedIdx = -1;
        w.focused = false;
        WmPushWmEvent(&w,WM_EVT_FOCUS_LOST,0,0);
        const int bestIdx=NextFocusCandidateLocked(idx);
        if (bestIdx >= 0) FocusWindowLocked(bestIdx);
    }
    extern void CompositorMarkDirty();
    CompositorMarkDirty();
}

static void RestoreWindowLocked(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    Window& w = g_windows[idx];
    if (!w.proc || !w.minimized) return;

    w.minimized = false;
    SerialPrintf("WM: restored window %d '%s'\n", idx, w.title);
    FocusWindowLocked(idx);
    extern void CompositorMarkDirty();
    CompositorMarkDirty();
}

void WmMinimizeWindow(int idx) { VfbGuard guard;MinimizeWindowLocked(idx); }
void WmRestoreWindow(int idx) { VfbGuard guard;RestoreWindowLocked(idx); }

static bool RequestCloseLocked(int idx,uint32_t* method)
{
    if (idx<0 || idx>=static_cast<int>(WM_MAX_WINDOWS)) return false;
    auto& w=g_windows[idx];
    if (!w.proc || !w.focusable) return false;
    if (w.vfb) {
        *method=1;
        return WmPushWmEvent(&w,WM_EVT_CLOSE_REQUESTED,0,0);
    }
    if(w.proc->state==ProcessState::Terminated)return false;
    TerminalGeometry terminal{};
    const bool isTerminal=TerminalSnapshotGeometry(w.proc,&terminal);
    *method=isTerminal?2:3;
    // Terminal hangup is cooperative too: no hidden SIGKILL on close.
    ProcessSendSignal(w.proc,isTerminal?1:15);
    return true;
}

bool WmRequestCloseWindow(int idx) {
    VfbGuard guard;uint32_t method=0;return RequestCloseLocked(idx,&method);
}

static void SnapshotDebugWindowLocked(const Window& w,WmDebugWindow* out)
{
    *out={};out->id=w.wmId;out->incarnation=w.creationIdentity;
    out->providerPid=w.proc->tgid?w.proc->tgid:w.proc->pid;
    out->providerIncarnation=w.proc->isThread?w.proc->leaderIncarnation:w.proc->incarnation;
    out->clientPid=w.clientPid;out->clientIncarnation=w.clientIncarnation;
    out->clientOrigin=w.clientOrigin;out->launchId=w.clientLaunchId;
    out->x=w.x;out->y=w.y;out->width=w.clientW;out->height=w.clientH;
    out->outerWidth=w.outerWidth();out->outerHeight=w.outerHeight();
    out->bufferWidth=w.vfbWidth;out->bufferHeight=w.vfbHeight;
    out->visible=w.visible;out->minimized=w.minimized;out->maximized=w.state==WindowState::Maximized;
    out->focused=w.focused;out->focusable=w.focusable;out->apiWindow=w.vfb!=nullptr;
    out->providerKernel=w.proc->isKernelThread;
    WmStrCopy(out->title,w.title,sizeof(out->title));WmStrCopy(out->appId,w.appId,sizeof(out->appId));
}

Process* WmReferenceTerminatedOwner()
{
    VfbGuard guard;
    for(const auto& w:g_windows) {
        if(!w.proc || w.proc->state!=ProcessState::Terminated)continue;
        Process* pinned=SchedulerFindProcessReference(w.proc->pid);
        if(pinned && pinned==w.proc && pinned->incarnation==w.proc->incarnation)return pinned;
        if(pinned)ProcessUnref(pinned);
    }
    return nullptr;
}

uint32_t WmSnapshotDebugWindows(WmDebugWindow* out,uint32_t capacity,uint64_t* tick)
{
    VfbGuard guard;uint32_t count=0;
    for (const auto& w:g_windows) if(w.proc && count<capacity) SnapshotDebugWindowLocked(w,&out[count++]);
    *tick=ApicTickCount();return count;
}

int WmApplyDebugAction(uint32_t id,uint64_t incarnation,WmDebugAction action,
                      uint32_t restoreTarget,WmDebugWindow* result,uint32_t* closeMethod)
{
    VfbGuard guard;
    if (!id || id>WM_MAX_WINDOWS) return -2;
    const int idx=static_cast<int>(id)-1;auto& w=g_windows[idx];
    if (!w.proc || w.creationIdentity!=incarnation) return -2;
    if (!w.focusable) return -95;
    const bool needsResize=(action==WmDebugAction::Maximize && w.state!=WindowState::Maximized) ||
        (action==WmDebugAction::Restore && restoreTarget &&
        (w.state==WindowState::Maximized)!=(restoreTarget==2));
    uint32_t notifications[WM_MAX_WINDOWS]{};
    if(needsResize)++notifications[idx];
    int focusTarget=-1;
    if(action==WmDebugAction::Focus || (action==WmDebugAction::Restore && w.minimized))focusTarget=idx;
    if(action==WmDebugAction::Minimize && !w.minimized && g_focusedIdx==idx) {
        ++notifications[idx];focusTarget=NextFocusCandidateLocked(idx);
        if(focusTarget>=0)++notifications[focusTarget];
    }else if(focusTarget>=0 && focusTarget!=g_focusedIdx) {
        ++notifications[focusTarget];
        if(g_focusedIdx>=0)++notifications[g_focusedIdx];
    }
    // Check every notification before applying any part of the transition.
    // Consumers can only free slots while this WM lock excludes producers.
    for(uint32_t i=0;i<WM_MAX_WINDOWS;++i)if(notifications[i] && g_windows[i].vfb) {
        const auto& target=g_windows[i];
        const uint32_t head=__atomic_load_n(&target.inputHead,__ATOMIC_ACQUIRE);
        const uint32_t tail=__atomic_load_n(&target.inputTail,__ATOMIC_ACQUIRE);
        const uint32_t free=(tail+Window::WM_INPUT_QUEUE-head-1)%Window::WM_INPUT_QUEUE;
        if(free<notifications[i])return -11;
    }
    switch(action) {
    case WmDebugAction::Focus:RestoreWindowLocked(idx);FocusWindowLocked(idx);break;
    case WmDebugAction::Minimize:MinimizeWindowLocked(idx);break;
    case WmDebugAction::Maximize:SetMaximizedLocked(idx,true);if(w.state!=WindowState::Maximized)return -12;break;
    case WmDebugAction::Restore:
        if(restoreTarget>2)return -22;
        if(restoreTarget) SetMaximizedLocked(idx,restoreTarget==2);
        if(restoreTarget && (w.state==WindowState::Maximized)!=(restoreTarget==2))return -12;
        RestoreWindowLocked(idx);break;
    case WmDebugAction::Close:
        if(w.clientOrigin==WmClientOrigin::Unknown || w.proc->isKernelThread)return -95;
        if(!RequestCloseLocked(idx,closeMethod))return -11;
        break;
    }
    SnapshotDebugWindowLocked(w,result);
    CompositorMarkDirty();CompositorWake();return 0;
}

uint32_t WmGetZOrder(int* outIndices, uint32_t maxOut)
{
    // Collect visible, non-minimized windows
    uint32_t count = 0;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS && count < maxOut; ++i)
    {
        if (g_windows[i].proc && g_windows[i].visible && !g_windows[i].minimized)
            outIndices[count++] = static_cast<int>(i);
    }

    // Simple insertion sort by z-order (ascending = back to front)
    for (uint32_t i = 1; i < count; ++i)
    {
        int key = outIndices[i];
        uint32_t keyZ = g_windows[key].zOrder;
        int j = static_cast<int>(i) - 1;
        while (j >= 0 && g_windows[outIndices[j]].zOrder > keyZ)
        {
            outIndices[j + 1] = outIndices[j];
            --j;
        }
        outIndices[j + 1] = key;
    }

    return count;
}

// ---------------------------------------------------------------------------
// Chrome rendering
// ---------------------------------------------------------------------------

static uint32_t WmMaterialColor(uint32_t color){
    return (color&0xffffff)|(CompositorGlassActive()?CompositorMaterialOpacity()<<24:0xff000000u);
}

static ChromeMaterialCache g_chromeMaterial;
static uint64_t g_materialRebuilds=0;
const ChromeMaterialCache& WmChromeMaterial(){
    const auto& p=WmCurrentPalette();
    if(g_chromeMaterial.Ensure(p.titleActive,p.titleInactive,CompositorMaterialOpacity(),
                              WmMetric(WM_TITLE_BAR_HEIGHT),WmMetric(7),WmMetric(WM_BORDER_WIDTH)))
        __atomic_add_fetch(&g_materialRebuilds,1,__ATOMIC_RELAXED);
    return g_chromeMaterial;
}
uint64_t WmMaterialCacheRebuilds(){return __atomic_load_n(&g_materialRebuilds,__ATOMIC_RELAXED);}

static void RenderWindowChrome(uint32_t* buf, uint32_t stride,
                                uint32_t screenW, uint32_t screenH,
                                const Window& w,
                                int32_t mouseX = -1, int32_t mouseY = -1)
{
    if (!buf || !stride || !screenW || !screenH || w.noChrome || !w.proc || !w.visible) return;
    const int border = WmMetric(WM_BORDER_WIDTH);
    const int ow = w.outerWidth(), oh = w.outerHeight();
    const uint32_t edge = w.focused ? WmCurrentPalette().borderActive : WmCurrentPalette().titleInactive;
    const uint32_t bg = WmMaterialColor(w.focused ? WmCurrentPalette().titleActive : WmCurrentPalette().titleInactive);
    const uint32_t fg = w.focused ? WmCurrentPalette().text : WmCurrentPalette().mutedText;
    WmFillRect(buf, stride, screenW, screenH, w.x, w.y, ow, border, edge);
    WmFillRect(buf, stride, screenW, screenH, w.x, w.y, border, oh, edge);
    WmFillRect(buf, stride, screenW, screenH, w.x + ow - border, w.y, border, oh, edge);
    WmFillRect(buf, stride, screenW, screenH, w.x, w.y + oh - border, ow, border, edge);
    const int tx = w.x + border, ty = w.y + border;
    const bool material=CompositorGlassActive();
    const auto* cached=material?&WmChromeMaterial():nullptr;
    const int titleH=WmMetric(WM_TITLE_BAR_HEIGHT);
    for(int row=0;row<titleH;++row)
        WmFillRect(buf,stride,screenW,screenH,tx,ty+row,w.clientW,1,
            material?cached->rows[w.focused][row]:bg);
    // A restrained one-pixel edge replaces the old heavy double outline.
    if(border>1){
        WmFillRect(buf,stride,screenW,screenH,w.x+1,w.y+1,ow-2,border-1,bg);
        WmFillRect(buf,stride,screenW,screenH,w.x+1,w.y+border,border-1,oh-2*border,bg);
        WmFillRect(buf,stride,screenW,screenH,w.x+ow-border,w.y+border,border-1,oh-2*border,bg);
        WmFillRect(buf,stride,screenW,screenH,w.x+1,w.y+oh-border,ow-2,border-1,bg);
    }
    if(material){
        WmFillRect(buf,stride,screenW,screenH,w.x,w.y,ow,1,cached->rims[w.focused][0]);
        WmFillRect(buf,stride,screenW,screenH,w.x,w.y+1,1,oh-2,cached->rims[w.focused][1]);
        WmFillRect(buf,stride,screenW,screenH,w.x+ow-1,w.y+1,1,oh-2,cached->rims[w.focused][1]);
        WmFillRect(buf,stride,screenW,screenH,w.x,w.y+oh-1,ow,1,cached->rims[w.focused][2]);
    }
    WmFillRect(buf,stride,screenW,screenH,tx,ty+titleH-1,w.clientW,1,
               material?cached->rims[w.focused][2]:ChromeMix(bg,0xffffff,32));
    // Active identity stroke: keep the hit-test geometry and client opacity.
    if (w.focused) WmFillRect(buf,stride,screenW,screenH,tx+WmMetric(12),ty,
        w.clientW>WmMetric(184)?WmMetric(160):w.clientW/2,WmMetric(4),0x0068ACFF);
    LauncherReadGuard icons;
    const int iconX = tx + WmMetric(10);
    const int textX = iconX + WmMetric(20) + WmMetric(8);
    if (CaptionRect(w, 2).x - iconX >= static_cast<int>(WmMetric(24)))
        WmDrawIcon(buf, stride, screenW, screenH, iconX,
                   ty + (static_cast<int>(WmMetric(WM_TITLE_BAR_HEIGHT)) - static_cast<int>(WmMetric(20))) / 2, WmMetric(20), WindowIcon(w, icons.get()));
    const int textW = CaptionRect(w, 2).x - textX - WmMetric(8);
    if (textW > 0)
        WmRenderString(buf, stride, screenW, screenH, textX,
            ty + (static_cast<int>(WmMetric(WM_TITLE_BAR_HEIGHT)) - WmDesktopFont().lineHeight) / 2,
            w.title, fg, bg, textW);
    for (int button = 0; button < 3; ++button)
    {
        const WmRect r = CaptionRect(w, button);
        const bool hover = r.contains(mouseX, mouseY);
        if(material)WmFillRect(buf,stride,screenW,screenH,r.x,r.y,1,r.h,ChromeMix(bg,0xffffff,20));
        if (hover) WmFillRect(buf, stride, screenW, screenH, r.x, r.y, r.w, r.h,
                             button == 0 ? WmCurrentPalette().closeHover : WmCurrentPalette().captionHover);
        if (r.w < static_cast<int>(WmMetric(14))) continue;
        const uint32_t ink = hover && button == 0 ? WmCurrentPalette().closeText :
                             hover ? WmCurrentPalette().text : fg;
        const int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        if (button == 0)
        {
            for (int d = -static_cast<int>(WmMetric(4)); d <= static_cast<int>(WmMetric(4)); ++d)
            {
                WmFillRect(buf, stride, screenW, screenH, cx+d,cy+d,WmMetric(1),WmMetric(1),ink);
                WmFillRect(buf, stride, screenW, screenH, cx+d,cy-d,WmMetric(1),WmMetric(1),ink);
            }
        }
        else if (button == 1)
        {
            auto box = [&](int x, int y, int width, int height) {
                WmFillRect(buf, stride, screenW, screenH, x, y, width, WmMetric(1), ink);
                WmFillRect(buf, stride, screenW, screenH, x, y + height - 1, width, WmMetric(1), ink);
                WmFillRect(buf, stride, screenW, screenH, x, y, WmMetric(1), height, ink);
                WmFillRect(buf, stride, screenW, screenH, x + width - 1, y, WmMetric(1), height, ink);
            };
            if (w.state == WindowState::Maximized)
            {
                box(cx - WmMetric(2), cy - WmMetric(5), WmMetric(8), WmMetric(8));
                WmFillRect(buf, stride, screenW, screenH, cx - WmMetric(5), cy - WmMetric(2), WmMetric(8), WmMetric(8),
                           hover ? WmCurrentPalette().captionHover : bg);
                box(cx - WmMetric(5), cy - WmMetric(2), WmMetric(8), WmMetric(8));
            }
            else box(cx - WmMetric(5), cy - WmMetric(4), WmMetric(11), WmMetric(9));
        }
        else WmFillRect(buf, stride, screenW, screenH, cx - WmMetric(5), cy + WmMetric(3), WmMetric(11), WmMetric(1), ink);
    }
}

void WmRenderChrome(uint32_t* backBuffer, uint32_t stride,
                    uint32_t screenW, uint32_t screenH)
{
    if (!g_wmActive) return;

    int sorted[WM_MAX_WINDOWS];
    uint32_t count = WmGetZOrder(sorted, WM_MAX_WINDOWS);

    for (uint32_t i = 0; i < count; ++i)
    {
        const Window& w = g_windows[sorted[i]];
        if (!w.proc || !w.visible) continue;
        RenderWindowChrome(backBuffer, stride, screenW, screenH, w);
    }
}

void WmRenderChromeForWindow(uint32_t* backBuffer, uint32_t stride,
                              uint32_t screenW, uint32_t screenH, int idx,
                              int32_t mouseX, int32_t mouseY)
{
    if (!g_wmActive || idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    const Window& w = g_windows[idx];
    if (!w.proc || !w.visible) return;
    RenderWindowChrome(backBuffer, stride, screenW, screenH, w, mouseX, mouseY);
}

// ---------------------------------------------------------------------------
// Taskbar
// ---------------------------------------------------------------------------

#include "desktop_telemetry_runtime.inc"

struct TaskbarLayout
{
    int y, textY;
    WmRect apps, terminal, next, clock;
    WmRect metrics[6];
    WmRect buttons[WM_MAX_WINDOWS + LaunchFeedbackTable::Capacity];
    int indices[WM_MAX_WINDOWS + LaunchFeedbackTable::Capacity];
    int count, nextIndex;
    char clockText[32];
};

static TaskbarLayout GetTaskbarLayout(uint32_t screenW, uint32_t screenH)
{
    TaskbarLayout l = {};
    l.nextIndex = -1;
    const int sw = static_cast<int>(screenW), sh = static_cast<int>(screenH);
    l.y = sh > static_cast<int>(WmMetric(WM_TASKBAR_HEIGHT)) ? sh - WmMetric(WM_TASKBAR_HEIGHT) : 0;
    const int height = sh - l.y;
    const int bh = height < static_cast<int>(WmMetric(WM_TASKBAR_BTN_HEIGHT)) ? height : WmMetric(WM_TASKBAR_BTN_HEIGHT);
    const int by = l.y + (height - bh) / 2;
    l.textY = by + (bh - WmDesktopFont().lineHeight) / 2;
    int x = WmMetric(WM_TASKBAR_PADDING);
    const int appsW = WmMetric(40);
    l.apps = {x, by, appsW, bh}; x += appsW + WmMetric(WM_TASKBAR_PADDING);
    l.terminal = {x, by, static_cast<int>(WmMetric(40)), bh}; x += WmMetric(40) + WmMetric(WM_TASKBAR_PADDING);
    RtcFormatTaskbar(l.clockText, RtcNow(), false);
    int right = sw - WmMetric(8);
    // Reserve measured widgets from right to left. Keep one useful window
    // button on small displays; diagnostics yield before the clock does.
    auto reserve = [&](WmRect& r, int width) {
        if (right - width - static_cast<int>(WmMetric(12)) < x + static_cast<int>(WmMetric(80))) return;
        right -= width;
        r = {right, by, width, bh};
        right -= WmMetric(12);
    };
    reserve(l.clock, WmTextWidth(l.clockText));
    const int widths[6]={64,64,112,112,240,240};
    const uint32_t enabledMetrics=WmGetTaskbarMetrics();
    for(int i=5;i>=0;--i)if(enabledMetrics&(1u<<i)){
        reserve(l.metrics[i],WmMetric(widths[i]));
        if(l.metrics[i].w){
            // Two 19-pixel text lines do not fit the 32-pixel task buttons.
            // Use the full tray height, retaining a scaled top/bottom inset.
            l.metrics[i].y=l.y+WmMetric(2);
            l.metrics[i].h=height-2*WmMetric(2);
        }
    }
    int ids[WM_MAX_WINDOWS + LaunchFeedbackTable::Capacity], total = 0, focused = 0;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
    {
        const Window& w = g_windows[i];
        if (!w.proc || !w.visible || !w.focusable) continue;
        if (w.focused) focused = total;
        ids[total++] = static_cast<int>(i);
    }
    SpinLockAcquire(&g_launchLock);
    for (unsigned i = 0; i < LaunchFeedbackTable::Capacity; ++i) {
        if (!g_launches.entries[i].present()) continue;
        if (g_taskbarPageTarget == -10 - static_cast<int>(i)) focused = total;
        ids[total++] = -10 - static_cast<int>(i);
    }
    SpinLockRelease(&g_launchLock);
    int available = right - x;
    if (!total || available <= 0) return l;
    int capacity = (available + WmMetric(WM_TASKBAR_PADDING)) / (WmMetric(WM_TASKBAR_BTN_WIDTH) + WmMetric(WM_TASKBAR_PADDING));
    bool overflow = capacity < total;
    if (overflow)
    {
        // The arrow focuses the first window on the next page. The focused
        // window selects the page, so every hidden/minimized window remains
        // reachable through the existing focus/restore input path.
        int arrowW = available < static_cast<int>(WmMetric(32)) ? available : static_cast<int>(WmMetric(32));
        l.next = {right - arrowW, by, arrowW, bh};
        available -= arrowW + WmMetric(WM_TASKBAR_PADDING);
        capacity = (available + WmMetric(WM_TASKBAR_PADDING)) / (WmMetric(WM_TASKBAR_BTN_WIDTH) + WmMetric(WM_TASKBAR_PADDING));
        if (capacity < 1 && available > 0) capacity = 1;
        if (capacity < 1) { l.nextIndex = ids[(focused + 1) % total]; return l; }
    }
    if (capacity > total) capacity = total;
    const int first = (focused / capacity) * capacity;
    l.count = total - first < capacity ? total - first : capacity;
    int width = (available - (capacity - 1) * WmMetric(WM_TASKBAR_PADDING)) / capacity;
    if (width > static_cast<int>(WmMetric(WM_TASKBAR_BTN_WIDTH))) width = WmMetric(WM_TASKBAR_BTN_WIDTH);
    for (int i = 0; i < l.count; ++i)
    {
        l.indices[i] = ids[first + i];
        l.buttons[i] = {x, by, width, bh};
        x += width + WmMetric(WM_TASKBAR_PADDING);
    }
    if (overflow) l.nextIndex = ids[(first + capacity) % total];
    return l;
}

static void WmMaterialRect(uint32_t* pixels,uint32_t stride,uint32_t sw,uint32_t sh,const WmRect& r,uint32_t color){
    const unsigned radius=CompositorGlassActive()?WmMetric(4):0;
    for(int y=0;y<r.h;++y){const unsigned row=y<r.h/2?y:r.h-1-y;
        const int inset=ChromeCornerInset(radius,row);
        WmFillRect(pixels,stride,sw,sh,r.x+inset,r.y+y,r.w-2*inset,1,WmMaterialColor(color));
    }
}

void WmRenderTaskbar(uint32_t* backBuffer, uint32_t stride,
                     uint32_t screenW, uint32_t screenH,
                     uint64_t uptimeMs, int32_t mouseX, int32_t mouseY)
{
    if (!g_wmActive || !backBuffer) return;
    LauncherReadGuard icons;
    const TaskbarLayout l = GetTaskbarLayout(screenW, screenH);
    WmFillRect(backBuffer, stride, screenW, screenH, 0, l.y, screenW,
               screenH - l.y, WmMaterialColor(WmCurrentPalette().taskbar));
    WmFillRect(backBuffer, stride, screenW, screenH, 0, l.y, screenW, 1, WmCurrentPalette().separator);
    auto button = [&](const WmRect& r, const char* title, bool active, bool minimized) {
        uint32_t bg = active ? WmCurrentPalette().taskActive :
            r.contains(mouseX, mouseY) ? WmCurrentPalette().taskHover : WmCurrentPalette().taskButton;
        WmMaterialRect(backBuffer,stride,screenW,screenH,r,bg);
        if (active || minimized)
            WmFillRect(backBuffer, stride, screenW, screenH, r.x + WmMetric(6), r.y + r.h - WmMetric(2),
                       r.w - WmMetric(12), WmMetric(2), active ? 0x68acff : WmCurrentPalette().minimized);
        if (r.w > static_cast<int>(WmMetric(12))) WmRenderString(backBuffer, stride, screenW, screenH,
            r.x + WmMetric(6), l.textY, title, WmCurrentPalette().taskText, bg, r.w - WmMetric(12));
    };
    button(l.apps, "", g_launcherOpen, false);
    button(l.terminal, "", false, false);
    WmDrawIcon(backBuffer, stride, screenW, screenH, l.apps.x + (l.apps.w - static_cast<int>(WmMetric(24))) / 2,
               l.apps.y + (l.apps.h - static_cast<int>(WmMetric(24))) / 2, WmMetric(24), icons.get().appsIcon);
    WmDrawIcon(backBuffer, stride, screenW, screenH, l.terminal.x + (l.terminal.w - static_cast<int>(WmMetric(24))) / 2,
               l.terminal.y + (l.terminal.h - static_cast<int>(WmMetric(24))) / 2, WmMetric(24), icons.get().terminalIcon);
    char launchTitle[80]{};
    const char* hoverTitle = l.apps.contains(mouseX, mouseY) ? "Applications" :
        l.terminal.contains(mouseX, mouseY) ? "New terminal" : nullptr;
    for (int i = 0; i < l.count; ++i)
    {
        const WmRect& r = l.buttons[i];
        if (l.indices[i] <= -10) {
            const unsigned slot = static_cast<unsigned>(-10 - l.indices[i]);
            LaunchFeedback e;
            uint32_t icon[LAUNCHER_ICON_PX * LAUNCHER_ICON_PX];
            bool hasIcon;
            SpinLockAcquire(&g_launchLock);
            e = g_launches.entries[slot];
            hasIcon = g_launchRequests[slot].hasIcon;
            if (hasIcon) memcpy(icon, g_launchRequests[slot].icon, sizeof(icon));
            SpinLockRelease(&g_launchLock);
            button(r, "", false, false);
            if (r.w >= static_cast<int>(WmMetric(24)) && r.h >= static_cast<int>(WmMetric(24)))
                WmDrawIcon(backBuffer, stride, screenW, screenH, r.x + (r.w - static_cast<int>(WmMetric(24))) / 2,
                           r.y + (r.h - static_cast<int>(WmMetric(24))) / 2, WmMetric(24), hasIcon ? icon : icons.get().applicationIcon);
            if (e.busy()) {
                // Eight positions form a bounded animated progress ring.
                static const int dx[8] = {0, 3, 4, 3, 0, -3, -4, -3};
                static const int dy[8] = {-4, -3, 0, 3, 4, 3, 0, -3};
                unsigned step = (uptimeMs / 100) % 8;
                for (unsigned dot = 0; dot < 8; ++dot)
                    WmFillRect(backBuffer, stride, screenW, screenH, r.x + r.w - 8 + dx[dot],
                               r.y + r.h - 8 + dy[dot], 2, 2,
                               dot == step ? WmCurrentPalette().taskText : WmCurrentPalette().minimized);
            } else {
                WmRenderString(backBuffer, stride, screenW, screenH, r.x + r.w - WmMetric(10),
                               l.textY, "!", WmCurrentPalette().taskText, WmCurrentPalette().taskButton, WmMetric(10));
            }
            if (r.contains(mouseX, mouseY)) {
                WmStrCopy(launchTitle, e.busy() ? "Starting: " :
                    e.phase == LaunchPhase::TimedOut ? "Still waiting: " : "Launch failed: ", sizeof(launchTitle));
                size_t n = strlen(launchTitle);
                WmStrCopy(launchTitle + n, e.title, sizeof(launchTitle) - n);
                hoverTitle = launchTitle;
            }
            continue;
        }
        const Window& w = g_windows[l.indices[i]];
        button(r, "", w.focused && !w.minimized, w.minimized);
        if (r.w >= static_cast<int>(WmMetric(24)) && r.h >= static_cast<int>(WmMetric(24)))
            WmDrawIcon(backBuffer, stride, screenW, screenH, r.x + (r.w - static_cast<int>(WmMetric(24))) / 2,
                       r.y + (r.h - static_cast<int>(WmMetric(24))) / 2, WmMetric(24), WindowIcon(w, icons.get()));
        if (r.contains(mouseX, mouseY)) hoverTitle = w.title;
    }
    if (l.next.w > 0) {
        button(l.next, ">", false, false);
        if (l.next.contains(mouseX, mouseY)) hoverTitle = "More windows";
    }
    // Use the already-reserved tray for the hover label. Geometry stays fixed
    // and no additional empty strip is reserved when nothing is hovered.
    int labelX = static_cast<int>(screenW);
    const WmRect trayRects[] = {l.metrics[0],l.metrics[1],l.metrics[2],l.metrics[3],l.metrics[4],l.metrics[5],l.clock};
    for (const auto& r : trayRects) if (r.w && r.x < labelX) labelX = r.x;
    if (hoverTitle && labelX + static_cast<int>(WmMetric(12)) < static_cast<int>(screenW))
    {
        WmRenderString(backBuffer, stride, screenW, screenH, labelX, l.textY,
                       hoverTitle, WmCurrentPalette().text, WmCurrentPalette().taskbar, static_cast<int>(screenW) - labelX - WmMetric(8));
        return;
    }
    auto trayText = [&](const WmRect& r, const char* text, uint32_t fg) {
        if (r.w > 0) WmRenderString(backBuffer, stride, screenW, screenH,
            r.x, l.textY, text, fg, WmCurrentPalette().taskbar, r.w);
    };
    trayText(l.clock, l.clockText, WmCurrentPalette().secondaryText);
    for(unsigned i=0;i<6;++i)DrawDesktopMetric(backBuffer,stride,screenW,screenH,l.metrics[i],i,uptimeMs);
}

int WmTaskbarHitTest(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH)
{
    if (mx < 0 || my < 0 || mx >= static_cast<int32_t>(screenW) || my >= static_cast<int32_t>(screenH)) return -1;
    const TaskbarLayout l = GetTaskbarLayout(screenW, screenH);
    if (l.apps.contains(mx, my)) return -3;
    if (l.terminal.contains(mx, my)) return -2;
    if (l.next.contains(mx, my)) return l.nextIndex <= -10 ? l.nextIndex - 90 : l.nextIndex;
    for (int i = 0; i < l.count; ++i)
        if (l.buttons[i].contains(mx, my)) return l.indices[i];
    return -1;
}

uint32_t WmDesktopHeight(uint32_t screenH)
{
    return screenH > WmMetric(WM_TASKBAR_HEIGHT) ? screenH - WmMetric(WM_TASKBAR_HEIGHT) : 0;
}

void WmSpawnTerminal()
{
    if (!g_wmActive || !ShutdownLaunchAllowed()) return;

    uint32_t clientW = 800;
    uint32_t clientH = 600;

    int termIdx = TerminalCreate(clientW, clientH);
    if (termIdx < 0)
    {
        SerialPuts("WM: failed to spawn new terminal\n");
        return;
    }

    Terminal* t = TerminalGet(termIdx);
    if (t && t->child)
    {
        // Stagger position based on terminal index
        int16_t winX = static_cast<int16_t>(60 + (termIdx % 6) * 40);
        int16_t winY = static_cast<int16_t>(60 + (termIdx % 6) * 40);

        t->child->fbDestX = winX + static_cast<int16_t>(WmMetric(WM_BORDER_WIDTH));
        t->child->fbDestY = winY + static_cast<int16_t>(WmMetric(WM_TITLE_BAR_HEIGHT) + WmMetric(WM_BORDER_WIDTH));
        t->child->fbScale = 1;
        t->child->fbDirty = 1;

        WmCreateWindow(t->child, winX, winY,
                       static_cast<uint16_t>(clientW),
                       static_cast<uint16_t>(clientH), "Terminal");

        SerialPrintf("WM: spawned new terminal (bash pid %u)\n", t->child->pid);
    }
}

// ---------------------------------------------------------------------------
// App Launcher — popup panel with shortcut items
// ---------------------------------------------------------------------------

// Launcher visual constants
static constexpr uint32_t LAUNCHER_ICON_SIZE    = LAUNCHER_ICON_PX;
static constexpr uint32_t LAUNCHER_ICON_MARGIN  = 8;
static constexpr uint32_t LAUNCHER_ITEM_HEIGHT  = 40;
static constexpr uint32_t LAUNCHER_ITEM_WIDTH   = 230;
static constexpr uint32_t LAUNCHER_PADDING      = 6;
static constexpr uint32_t LAUNCHER_MAX_ROWS     = 14; // max rows before scrolling
static constexpr uint32_t LAUNCHER_MAX_COLS     = 3;  // max columns

static void LauncherDrawIcon(uint32_t* buf, uint32_t stride,
                             uint32_t screenW, uint32_t screenH,
                             int32_t x, int32_t y, const LauncherItem* item, const LauncherSnapshot& launcher)
{
    WmDrawIcon(buf, stride, screenW, screenH, x, y, WmMetric(LAUNCHER_ICON_PX),
               item->iconPixels ? item->iconPixels : launcher.applicationIcon);
}

// Read title and explicit window identity from a shortcut's comment header.
// Identity metadata matches Wayland app_id/executable without guessing by title.
static bool ParseShortcutMetadata(const char* path, LauncherItem& item)
{
    Vnode* vn = VfsOpen(path, 0);
    if (!vn) return false;
    char buf[512]; uint64_t offset = 0;
    int rd = VfsRead(vn, buf, sizeof(buf) - 1, &offset);
    VfsClose(vn);
    if (rd <= 0) return false;
    buf[rd] = 0;
    struct Field { const char* key; char* output; uint32_t capacity; };
    const Field fields[] = {
        {"title:", item.title, sizeof(item.title)},
        {"app_id:", item.appId, sizeof(item.appId)},
        {"executable:", item.executable, sizeof(item.executable)},
    };
    for (char* line = buf; *line;)
    {
        char* end = line;
        while (*end && *end != '\n') ++end;
        const bool more = *end != 0; *end = 0;
        char* text = line;
        while (*text == ' ' || *text == '\t') ++text;
        if (!*text || *text == '\r') {
            if (!more) break;
            line = end + 1;
            continue;
        }
        if (*text != '#') break;
        ++text; while (*text == ' ' || *text == '\t') ++text;
        for (const auto& field : fields)
        {
            const uint32_t length = strlen(field.key);
            if (strncmp(text, field.key, length) != 0) continue;
            char* value = text + length;
            while (*value == ' ' || *value == '\t') ++value;
            char* tail = value + strlen(value);
            while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t' || tail[-1] == '\r')) --tail;
            *tail = 0;
            WmStrCopy(field.output, value, field.capacity);
            break;
        }
        if (!more) break;
        line = end + 1;
    }
    return item.title[0] != 0;
}

// Asset files are 24x24 straight-alpha pixels encoded as little-endian ARGB.
static uint32_t* LoadIconFile(const char* name)
{
    if (!name || !name[0]) return nullptr;
    for (const char* p = name; *p; ++p) if (*p == '/' || *p == '\\') return nullptr;
    char path[192];
    const char* prefix = "/nix/share/applications/icons/";
    uint32_t n = 0;
    while (*prefix) path[n++] = *prefix++;
    for (; *name && n + 1 < sizeof(path); ++name) path[n++] = *name;
    if (*name) return nullptr;
    path[n] = 0;
    Vnode* vn = VfsOpen(path, 0);
    if (!vn) return nullptr;
    auto* pixels = static_cast<uint32_t*>(kmalloc(LAUNCHER_ICON_BYTES));
    uint64_t offset = 0;
    if (pixels)
    {
        uint32_t done = 0;
        while (done < LAUNCHER_ICON_BYTES)
        {
            int rd = VfsRead(vn, reinterpret_cast<uint8_t*>(pixels) + done,
                             LAUNCHER_ICON_BYTES - done, &offset);
            if (rd <= 0) break;
            done += static_cast<uint32_t>(rd);
        }
        if (done != LAUNCHER_ICON_BYTES) { kfree(pixels); pixels = nullptr; }
    }
    VfsClose(vn);
    return pixels;
}

static void LauncherLoadIcon(LauncherItem* item)
{
    char name[64]; uint32_t n = 0;
    for (const char* p = item->title; *p && n + 6 < sizeof(name); ++p)
    {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c == ' ') c = '_';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') name[n++] = c;
    }
    WmStrCopy(name + n, ".rgba", sizeof(name) - n);
    item->iconPixels = LoadIconFile(name);
}

static bool LauncherFileExists(const char* path)
{
    VnodeStat st{};
    return path[0] == '/' && VfsStatPath(path, &st) == 0 && !st.isDir;
}

static bool LauncherExecutable(const char* command, char* executable, uint32_t capacity)
{
    command = LauncherExecutableStart(command);
    if (!command) return false;
    char path[256]; uint32_t n = 0;
    while (*command == ' ' || *command == '\t') ++command;
    char quote = (*command == '\"' || *command == '\'') ? *command++ : 0;
    while (*command && (quote ? *command != quote : *command != ' ' && *command != '\t'))
    {
        if (n + 1 >= sizeof(path)) return false;
        path[n++] = *command++;
    }
    if (!n || (quote && *command != quote)) return false;
    path[n] = 0;
    WmStrCopy(executable, WmBaseName(path), capacity);
    if (path[0] == '/') {
        if (strncmp(path, "/nix/store/", 11) != 0) return LauncherFileExists(path);
        // Store objects survive profile removal. A desktop entry is installed
        // only while the active profile exposes this same executable, not just
        // another package's executable with an identical basename. stat follows
        // profile symlinks; compare mount identity as well as the inode.
        const char* basename = WmBaseName(path);
        if (!*basename) return false;
        char profile[288];
        constexpr char prefix[] = "/nix/profile/bin/";
        constexpr uint32_t prefixLength = sizeof(prefix) - 1;
        WmStrCopy(profile, prefix, sizeof(profile));
        WmStrCopy(profile + prefixLength, basename, sizeof(profile) - prefixLength);
        VnodeStat target{}, installed{};
        return VfsStatPath(path, &target) == 0 && !target.isDir && target.ino &&
               VfsStatPath(profile, &installed) == 0 && !installed.isDir &&
               target.dev == installed.dev && target.ino == installed.ino;
    }
    // Only a bare executable name may be searched in installed package bins.
    for (uint32_t i = 0; i < n; ++i) if (path[i] == '/') return false;
    const char* roots[] = {"/nix/profile/bin/", "/nix/bin/"};
    for (const char* root : roots)
    {
        char resolved[288]; uint32_t p = 0;
        while (*root) resolved[p++] = *root++;
        WmStrCopy(resolved + p, path, sizeof(resolved) - p);
        if (LauncherFileExists(resolved)) return true;
    }
    return false;
}

static bool ShortcutRequirementsMet(const char* path)
{
    Vnode* vn = VfsOpen(path, 0);
    if (!vn) return false;
    char buf[1024]; uint64_t offset = 0;
    int rd = VfsRead(vn, buf, sizeof(buf) - 1, &offset);
    VfsClose(vn);
    if (rd <= 0) return false;
    buf[rd] = 0;
    bool declared = false;
    for (char* line = buf; *line;)
    {
        char* end = line;
        while (*end && *end != '\n') ++end;
        const bool more = *end != 0; *end = 0;
        const char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '#') break;
        ++p; while (*p == ' ' || *p == '\t') ++p;
        if (strncmp(p, "Requires:", 9) == 0)
        {
            p += 9; while (*p == ' ' || *p == '\t') ++p;
            char dependency[256]; WmStrCopy(dependency, p, sizeof(dependency));
            uint32_t len = strlen(dependency);
            while (len && (dependency[len - 1] == ' ' || dependency[len - 1] == '\t' || dependency[len - 1] == '\r')) dependency[--len] = 0;
            if (!WmEqualI(dependency, "builtin") && !LauncherFileExists(dependency)) return false;
            declared = true;
        }
        if (!more) break;
        line = end + 1;
    }
    return declared;
}

// A candidate is all-or-nothing: oversized/short/erroring manifests or item
// overflow retain the published snapshot. Limits bound memory and scan work.
static constexpr uint32_t LAUNCHER_MANIFEST_LIMIT = 64 * 1024;
static constexpr uint32_t LAUNCHER_RECORD_LIMIT = 1024;

static bool LauncherLoadDesktopEntries(LauncherSnapshot& launcher)
{
    const char* manifest = "/nix/share/applications/applications.idx";
    VnodeStat st{};
    if (VfsStatPath(manifest, &st) != 0) {
        // ext2 currently conflates missing paths and I/O failures. A missing
        // manifest is valid at first boot, but cannot safely replace a live
        // menu. Removal notifications should publish an empty manifest.
        if (!g_launcherLoaded) return true;
        SerialPuts("WM: launcher manifest unavailable; retaining menu\n");
        return false;
    }
    if (st.isDir || st.size > LAUNCHER_MANIFEST_LIMIT) {
        SerialPuts("WM: launcher manifest exceeds 64KiB limit or is not a file; retaining menu\n");
        return false;
    }
    Vnode* vn = VfsOpen(manifest, 0);
    if (!vn) return false;
    char* buf = static_cast<char*>(kmalloc(LAUNCHER_MANIFEST_LIMIT + 2));
    if (!buf) { VfsClose(vn); return false; }
    uint64_t offset = 0; uint32_t done = 0; bool ok = true;
    while (done <= LAUNCHER_MANIFEST_LIMIT)
    {
        int rd = VfsRead(vn, buf + done, LAUNCHER_MANIFEST_LIMIT + 1 - done, &offset);
        if (rd < 0) { ok = false; break; }
        if (!rd) break;
        done += static_cast<uint32_t>(rd);
    }
    VfsClose(vn);
    if (done > LAUNCHER_MANIFEST_LIMIT || done != st.size) ok = false;
    // Embedded NUL would silently hide records if treated as a C string.
    for (uint32_t i = 0; i < done && ok; ++i) if (!buf[i]) ok = false;
    buf[done] = 0;
    for (char* line = buf; ok && *line;)
    {
        char* end = line; while (*end && *end != '\n') ++end;
        const bool more = *end != 0;
        if (end - line > LAUNCHER_RECORD_LIMIT) { ok = false; break; }
        *end = 0;
        if (end > line && end[-1] == '\r') end[-1] = 0;
        if (!*line) { if (!more) break; line = end + 1; continue; }
        char* fields[5] = {line, nullptr, nullptr, nullptr, nullptr};
        uint32_t count = 1;
        for (char* p = line; *p && count < 5; ++p)
            if (*p == '\t') { *p = 0; fields[count++] = p + 1; }
        if (count < 2 || !fields[0][0] || strlen(fields[1]) >= sizeof(LauncherItem{}.scriptPath) ||
            (count >= 5 && strlen(fields[4]) >= sizeof(LauncherItem{}.appId))) { ok = false; break; }
        char executable[64] = {};
        if (LauncherExecutable(fields[1], executable, sizeof(executable)))
        {
            LauncherItem* item = nullptr;
            for (uint32_t i = 0; i < launcher.count; ++i)
                if (!launcher.items[i].isDesktopEntry && WmEqualI(launcher.items[i].title, fields[0])) { item = &launcher.items[i]; break; }
            if (!item)
            {
                if (launcher.count == WM_LAUNCHER_MAX_ITEMS) { ok = false; break; }
                item = &launcher.items[launcher.count++];
            }
            // The installed executable has passed profile validation. A
            // same-title legacy shortcut must not retain its stale command.
            // This candidate is unpublished; its icon can be replaced here.
            item->valid = true; item->isDesktopEntry = true;
            WmStrCopy(item->title, fields[0], sizeof(item->title));
            WmStrCopy(item->scriptPath, fields[1], sizeof(item->scriptPath));
            WmStrCopy(item->executable, executable, sizeof(item->executable));
            WmStrCopy(item->appId, count >= 5 ? fields[4] : "", sizeof(item->appId));
            kfree(item->iconPixels);item->iconPixels = nullptr;
            if (count >= 3) item->iconPixels = LoadIconFile(fields[2]);
            if (!item->iconPixels) LauncherLoadIcon(item);
        }
        if (!more) break;
        line = end + 1;
    }
    kfree(buf);
    if (!ok) SerialPuts("WM: launcher manifest read/record/capacity error (64KiB/1KiB/128 items); retaining menu\n");
    return ok;
}

static void FreeLauncherSnapshot(LauncherSnapshot* snapshot)
{
    if (!snapshot || snapshot == &g_emptyLauncher) return;
    for (uint32_t i = 0; i < snapshot->count; ++i) kfree(snapshot->items[i].iconPixels);
    kfree(snapshot->applicationIcon); kfree(snapshot->terminalIcon); kfree(snapshot->appsIcon);
    kfree(snapshot);
}

static bool IdentifyLauncherItems(LauncherSnapshot& catalog)
{
    const char* hex="0123456789abcdef";char generation[9];
    for(unsigned i=0;i<8;++i)generation[i]=hex[(catalog.generation>>((7-i)*4))&15];
    generation[8]=0;
    for(uint32_t i=0;i<catalog.count;++i) {
        auto& item=catalog.items[i];LauncherIdentity identity;
        identity.field(item.isDesktopEntry?"desktop":"shortcut");
        // New manifests carry the exact desktop-file basename. For legacy
        // manifests without it, use the command and label as an explicit fallback.
        if(item.isDesktopEntry&&item.appId[0])identity.field(item.appId);
        else {identity.field(item.scriptPath);if(item.isDesktopEntry)identity.field(item.title);}
        identity.finish(item.catalogId);
        for(uint32_t j=0;j<i;++j)if(!strcmp(item.catalogId,catalog.items[j].catalogId)) {
            SerialPuts("WM: duplicate/colliding launcher identity; retaining previous catalog\n");return false;
        }
        LauncherIdentity spec;spec.field(item.catalogId);spec.field(item.scriptPath);
        spec.field(item.isDesktopEntry?"desktop":"shortcut");spec.field(generation);
        spec.finish(item.specification);
    }
    return true;
}

static LauncherSnapshot* BuildLauncherSnapshot(uint32_t generation)
{
    auto* snapshot = static_cast<LauncherSnapshot*>(kmalloc(sizeof(LauncherSnapshot)));
    if (!snapshot) return nullptr;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->generation = generation;
    snapshot->applicationIcon = LoadIconFile("application.rgba");
    snapshot->terminalIcon = LoadIconFile("terminal.rgba");
    snapshot->appsIcon = LoadIconFile("applications.rgba");
    bool ok = true;
    Vnode* dir = VfsOpen("/boot/SHORTCUTS", 0);
    if (dir)
    {
        DirEntry de; uint32_t cookie = 0; int rd;
        while ((rd = VfsReaddir(dir, &de, &cookie)) == 1)
        {
            if (de.isDir) continue;
            LauncherItem item{};
            const char* prefix = "/boot/SHORTCUTS/";
            const uint32_t prefixLen = strlen(prefix);
            if (prefixLen + strlen(de.name) >= sizeof(item.scriptPath)) { ok = false; break; }
            WmStrCopy(item.scriptPath, prefix, sizeof(item.scriptPath));
            WmStrCopy(item.scriptPath + prefixLen, de.name, sizeof(item.scriptPath) - prefixLen);
            if (!ShortcutRequirementsMet(item.scriptPath)) continue;
            if (snapshot->count == WM_LAUNCHER_MAX_ITEMS) { ok = false; break; }
            if (!ParseShortcutMetadata(item.scriptPath, item))
            {
                uint32_t ti = 0;
                for (uint32_t j = 0; de.name[j] && de.name[j] != '.' && ti + 1 < sizeof(item.title); ++j)
                    item.title[ti++] = de.name[j];
                item.title[ti] = 0;
            }
            item.valid = true;
            LauncherLoadIcon(&item);
            snapshot->items[snapshot->count++] = item;
        }
        if (rd < 0) ok = false;
        VfsClose(dir);
    }
    else if (g_launcherLoaded) {
        SerialPuts("WM: shortcut directory unavailable; retaining menu\n");
        ok = false;
    }
    if (ok) ok = LauncherLoadDesktopEntries(*snapshot);
    if (ok) ok = IdentifyLauncherItems(*snapshot);
    if (!ok) {
        SerialPuts("WM: launcher rebuild failed; previous snapshot retained\n");
        FreeLauncherSnapshot(snapshot);
        return nullptr;
    }
    snapshot->builtTick=ApicTickCount();
    return snapshot;
}

static bool g_launchInhibited=false;
static void LauncherWorker(void*)
{
    uint32_t attempted = 0;
    { LauncherReadGuard guard; attempted = guard.get().generation; }
    for (;;)
    {
        LauncherSnapshot* disposable = nullptr;
        SpinLockAcquire(&g_launcherLock);
        if (g_launcherRetired && !__atomic_load_n(&g_launcherRetired->refs, __ATOMIC_ACQUIRE)) {
            disposable = g_launcherRetired;
            g_launcherRetired = nullptr;
        }
        const bool occupied = g_launcherPending || g_launcherRetired;
        SpinLockRelease(&g_launcherLock);
        FreeLauncherSnapshot(disposable);
        const uint32_t requested = __atomic_load_n(&g_launcherRequested, __ATOMIC_ACQUIRE);
        if (!occupied && requested != attempted)
        {
            LauncherSnapshot* next = BuildLauncherSnapshot(requested);
            attempted = requested; // failure retries on the next notification/open
            if (next)
            {
                SpinLockAcquire(&g_launcherLock);
                g_launcherPending = next;
                SpinLockRelease(&g_launcherLock);
                CompositorWake();
            }
        }
        char command[128];
        bool desktop = false;
        uint64_t token = 0;
        SpinLockAcquire(&g_launchLock);
        int launch = g_launchInhibited?-1:g_launches.Take(ApicTickCount());
        if (launch >= 0) {
            token = g_launches.entries[launch].token;
            g_launchHistory.Start(token,ApicTickCount());
            WmStrCopy(command, g_launchRequests[launch].command, sizeof(command));
            desktop = g_launchRequests[launch].desktop;
        }
        SpinLockRelease(&g_launchLock);
        if (token) {
            Process* self = ProcessCurrent();
            const uint64_t previous = self->launchId;
            self->launchId = token;
            int result;
            char executable[64];
            const bool available=desktop?LauncherExecutable(command,executable,sizeof(executable)):ShortcutRequirementsMet(command);
            if(!available)result=-2;
            else if (desktop) {
                char script[512];
                // Helpers can fail independently of a successful app spawn.
                ShellExecString("set wm\nset vfb none\nrun --service --once waylandd\n");
                const char* hdr = "run ";
                uint32_t si = 0;
                while (*hdr && si < sizeof(script) - 2) script[si++] = *hdr++;
                const char* exec = command;
                while (*exec && si < sizeof(script) - 2) script[si++] = *exec++;
                script[si] = 0;
                result = ShellLaunchCommand(script);
            } else result = ShellExecScript(command);
            self->launchId = previous;
            SpinLockAcquire(&g_launchLock);
            g_launches.Complete(token, result >= 0, ApicTickCount(),result,true);
            g_launchHistory.Complete(token,result,ApicTickCount());
            SpinLockRelease(&g_launchLock);
            SerialPrintf("WM: launch token %lu dispatch %s result=%d\n", token, result >= 0 ? "complete" : "failed",result);
            CompositorMarkDirty();
            CompositorWake();
        }
        // One worker, coalesced requests, no busy waiting or retained rebuild
        // list. A slow reader delays reclamation and subsequent builds.
        SchedulerSleepMs(250);
    }
}

static void LauncherStartWorker()
{
    bool expected = false;
    if (!__atomic_compare_exchange_n(&g_launcherWorkerStarted, &expected, true,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    Process* worker = KernelThreadCreate("launcher", LauncherWorker, nullptr, 3);
    if (!worker) {
        __atomic_store_n(&g_launcherWorkerStarted, false, __ATOMIC_RELEASE);
        SerialPuts("WM: launcher worker allocation failed\n");
        return;
    }
    SchedulerAddProcess(worker);
}

void WmLauncherLoad()
{
    if (g_launcherLoaded) return;
    const uint32_t generation = __atomic_load_n(&g_launcherRequested, __ATOMIC_ACQUIRE);
    LauncherSnapshot* snapshot = BuildLauncherSnapshot(generation);
    if (snapshot)
    {
        // Initial activation precedes rendering; later publication belongs
        // exclusively to the compositor's frame-boundary hook.
        SpinLockAcquire(&g_launcherLock);
        g_launcherActive = snapshot;
        SpinLockRelease(&g_launcherLock);
        SerialPrintf("WM: launcher initial generation=%u items=%u\n", generation, snapshot->count);
    }
    g_launcherLoaded = true;
}

void WmLauncherToggle()
{
    // Loading belongs to WmSetActive (shell context), never the input/render thread.
    if (!g_launcherLoaded) return;
    g_launcherOpen = !g_launcherOpen;
    if (g_launcherOpen) {
        g_launcherError = 0;
        g_launcherScroll = 0;
        WmRequestLauncherRefresh(); // also catches installs/removals outside the installer
    }
}

bool WmLauncherVisible()
{
    return g_launcherOpen;
}

struct LauncherLayout
{
    int x, y;
    uint32_t w, h, cols, rows, itemW, capacity;
};

static LauncherLayout GetLauncherLayout(uint32_t screenW, uint32_t screenH,
                                        const LauncherSnapshot& launcher)
{
    uint32_t total = 0;
    for (uint32_t i = 0; i < launcher.count; ++i)
        if (launcher.items[i].valid) ++total;
    const uint32_t desktopH = screenH > WmMetric(WM_TASKBAR_HEIGHT) ? screenH - WmMetric(WM_TASKBAR_HEIGHT) : 0;
    LauncherLayout l = {};
    l.x = screenW > WmMetric(8) ? WmMetric(4) : 0;
    const uint32_t availableW = screenW > WmMetric(8) ? screenW - WmMetric(8) : screenW;
    const uint32_t availableH = desktopH > WmMetric(4) ? desktopH - WmMetric(4) : desktopH;
    const uint32_t fixedH = 2 * WmMetric(LAUNCHER_ITEM_HEIGHT) + 2 * WmMetric(LAUNCHER_PADDING);
    l.rows = availableH > fixedH ? (availableH - fixedH) / (WmMetric(LAUNCHER_ITEM_HEIGHT) + WmMetric(2)) : 0;
    if (l.rows > LAUNCHER_MAX_ROWS) l.rows = LAUNCHER_MAX_ROWS;
    l.cols = l.rows ? (total + l.rows - 1) / l.rows : 1;
    if (!l.cols) l.cols = 1;
    if (l.cols > LAUNCHER_MAX_COLS) l.cols = LAUNCHER_MAX_COLS;
    uint32_t fitCols = availableW > 2 * WmMetric(LAUNCHER_PADDING) ?
        (availableW - 2 * WmMetric(LAUNCHER_PADDING)) / (WmMetric(LAUNCHER_ITEM_WIDTH) + WmMetric(4)) : 0;
    if (!fitCols) fitCols = 1;
    if (l.cols > fitCols) l.cols = fitCols;
    l.itemW = availableW > 2 * WmMetric(LAUNCHER_PADDING) + WmMetric(4) ?
        (availableW - 2 * WmMetric(LAUNCHER_PADDING)) / l.cols - WmMetric(4) : 0;
    if (l.itemW > WmMetric(LAUNCHER_ITEM_WIDTH)) l.itemW = WmMetric(LAUNCHER_ITEM_WIDTH);
    uint32_t neededRows = (total + l.cols - 1) / l.cols;
    if (l.rows > neededRows) l.rows = neededRows;
    l.capacity = l.rows * l.cols;
    uint32_t maxScroll = total > l.capacity ? total - l.capacity : 0;
    if (g_launcherScroll > maxScroll) g_launcherScroll = maxScroll;
    l.w = l.cols * (l.itemW + WmMetric(4)) + 2 * WmMetric(LAUNCHER_PADDING);
    if (l.w > availableW) l.w = availableW;
    l.h = fixedH + l.rows * (WmMetric(LAUNCHER_ITEM_HEIGHT) + WmMetric(2));
    if (l.h > availableH) l.h = availableH;
    l.y = static_cast<int>(desktopH) - static_cast<int>(l.h) - WmMetric(2);
    if (l.y < 0) l.y = 0;
    return l;
}

static WmRect LauncherItemRect(const LauncherLayout& l, uint32_t slot)
{
    return {l.x + static_cast<int>(WmMetric(LAUNCHER_PADDING) + (slot % l.cols) * (l.itemW + WmMetric(4))),
            l.y + static_cast<int>(WmMetric(LAUNCHER_PADDING) + WmMetric(LAUNCHER_ITEM_HEIGHT) +
                                   (slot / l.cols) * (WmMetric(LAUNCHER_ITEM_HEIGHT) + WmMetric(2))),
            static_cast<int>(l.itemW), static_cast<int>(WmMetric(LAUNCHER_ITEM_HEIGHT))};
}

void WmLauncherScroll(int delta, uint32_t screenW, uint32_t screenH)
{
    if (!g_launcherOpen) return;
    LauncherReadGuard guard;
    const auto& launcher = guard.get();
    uint32_t total = 0;
    for (uint32_t i = 0; i < launcher.count; ++i)
        if (launcher.items[i].valid) ++total;
    const LauncherLayout l = GetLauncherLayout(screenW, screenH, launcher);
    if (!l.capacity || total <= l.capacity) return;
    if (delta < 0 && g_launcherScroll < total - l.capacity) ++g_launcherScroll;
    else if (delta > 0 && g_launcherScroll) --g_launcherScroll;
}

static void LauncherGetRect(uint32_t screenW, uint32_t screenH,
                            int32_t* outX, int32_t* outY,
                            uint32_t* outW, uint32_t* outH, const LauncherSnapshot& launcher)
{
    const LauncherLayout l = GetLauncherLayout(screenW, screenH, launcher);
    *outX = l.x; *outY = l.y; *outW = l.w; *outH = l.h;
}

void WmLauncherGetRect(uint32_t screenW, uint32_t screenH,
                       int32_t* outX, int32_t* outY,
                       uint32_t* outW, uint32_t* outH)
{
    LauncherReadGuard guard;
    LauncherGetRect(screenW, screenH, outX, outY, outW, outH, guard.get());
}

void WmLauncherRender(uint32_t* backBuffer, uint32_t stride,
                      uint32_t screenW, uint32_t screenH,
                      int32_t mouseX, int32_t mouseY)
{
    LauncherReadGuard guard;
    const auto& launcher = guard.get();
    if (!g_launcherOpen) return;

    int32_t px, py;
    uint32_t pw, ph;
    LauncherGetRect(screenW, screenH, &px, &py, &pw, &ph, launcher);

    // Panel background
    WmFillRect(backBuffer, stride, screenW, screenH,
               px, py, static_cast<int>(pw), static_cast<int>(ph), WmMaterialColor(WmCurrentPalette().launcher));

    // Border
    WmFillRect(backBuffer, stride, screenW, screenH, px, py, static_cast<int>(pw), 1, WmCurrentPalette().launcherBorder);
    WmFillRect(backBuffer, stride, screenW, screenH, px, py + static_cast<int32_t>(ph) - 1, static_cast<int>(pw), 1, WmCurrentPalette().launcherBorder);
    WmFillRect(backBuffer, stride, screenW, screenH, px, py, 1, static_cast<int>(ph), WmCurrentPalette().launcherBorder);
    WmFillRect(backBuffer, stride, screenW, screenH, px + static_cast<int32_t>(pw) - 1, py, 1, static_cast<int>(ph), WmCurrentPalette().launcherBorder);

    int textYOff = (static_cast<int>(WmMetric(LAUNCHER_ITEM_HEIGHT)) - WmDesktopFont().lineHeight) / 2;

    // Extremely small surfaces cannot fit even the header.
    if (ph < WmMetric(LAUNCHER_ITEM_HEIGHT) + 2 * WmMetric(LAUNCHER_PADDING) || pw < 2 * WmMetric(LAUNCHER_PADDING) + 1) return;

    // Header: "Applications"
    int32_t iy = py + static_cast<int32_t>(WmMetric(LAUNCHER_PADDING));
    if (pw > WmMetric(56)) WmRenderMaterialString(backBuffer, stride, screenW, screenH,
                   px + static_cast<int32_t>(WmMetric(LAUNCHER_PADDING)) + static_cast<int>(WmMetric(4)),
                   iy + static_cast<int32_t>(textYOff),
                   g_launcherError == -2 ? "App launch already pending" : g_launcherError ? "Launch queue full" : ShutdownDesktopStatus(), WmCurrentPalette().secondaryText, WmCurrentPalette().launcher, static_cast<int>(pw) - WmMetric(56));
    iy += WmMetric(LAUNCHER_ITEM_HEIGHT);

    // Separator line below header
    WmFillRect(backBuffer, stride, screenW, screenH,
               px + static_cast<int32_t>(WmMetric(LAUNCHER_PADDING)),
               iy - 1, static_cast<int>(pw) - 2 * WmMetric(LAUNCHER_PADDING), 1, WmCurrentPalette().launcherBorder);

    // Always available, independent of installed package shortcuts.
    const int footerY=py+static_cast<int>(ph)-WmMetric(LAUNCHER_PADDING)-WmMetric(LAUNCHER_ITEM_HEIGHT);
    const int third=(static_cast<int>(pw)-2*WmMetric(LAUNCHER_PADDING))/3;
    const char* labels[]={"Shut down","Restart","Cancel"};
    for(int i=0;i<3;++i)if(third>static_cast<int>(WmMetric(12)))
        WmRenderMaterialString(backBuffer,stride,screenW,screenH,px+WmMetric(LAUNCHER_PADDING)+i*third+WmMetric(4),footerY+textYOff,labels[i],WmCurrentPalette().text,WmCurrentPalette().launcher,third-WmMetric(8));

    // Items — render in multi-column layout based on scroll offset
    uint32_t totalValid = 0;
    for (uint32_t i = 0; i < launcher.count; i++)
        if (launcher.items[i].valid) totalValid++;

    const LauncherLayout layout = GetLauncherLayout(screenW, screenH, launcher);
    uint32_t maxVisible = layout.capacity;

    uint32_t validIdx = 0;
    uint32_t rendered = 0;
    for (uint32_t i = 0; i < launcher.count && rendered < maxVisible; i++)
    {
        if (!launcher.items[i].valid) continue;

        // Skip items before scroll offset
        if (validIdx < g_launcherScroll)
        {
            validIdx++;
            continue;
        }
        validIdx++;

        const WmRect item = LauncherItemRect(layout, rendered);
        const int itemX = item.x, itemY = item.y;
        bool hovered = item.contains(mouseX, mouseY);
        uint32_t itemBg = WmMaterialColor(hovered ? WmCurrentPalette().itemHover : WmCurrentPalette().launcher);

        WmFillRect(backBuffer, stride, screenW, screenH,
                   itemX, itemY,
                   static_cast<int>(layout.itemW),
                   static_cast<int>(WmMetric(LAUNCHER_ITEM_HEIGHT)),
                   itemBg);

        // Draw icon
        int32_t iconX = itemX + WmMetric(6);
        int32_t iconY = itemY + static_cast<int32_t>((WmMetric(LAUNCHER_ITEM_HEIGHT) - WmMetric(LAUNCHER_ICON_SIZE)) / 2);
        if (layout.itemW >= WmMetric(LAUNCHER_ICON_SIZE) + WmMetric(12))
            LauncherDrawIcon(backBuffer, stride, screenW, screenH,
                             iconX, iconY, &launcher.items[i], launcher);

        // Title text (offset to right of icon)
        int32_t textX = iconX + static_cast<int32_t>(WmMetric(LAUNCHER_ICON_SIZE)) +
                        static_cast<int32_t>(WmMetric(LAUNCHER_ICON_MARGIN));
        uint32_t textFg = hovered ? WmCurrentPalette().text : WmCurrentPalette().taskText;
        int maxTextW = static_cast<int>(layout.itemW) - static_cast<int>(WmMetric(LAUNCHER_ICON_SIZE) + WmMetric(LAUNCHER_ICON_MARGIN) + WmMetric(12));
        if (maxTextW > 0) WmRenderMaterialString(backBuffer, stride, screenW, screenH,
                       textX, itemY + static_cast<int32_t>(textYOff),
                       launcher.items[i].title,
                       textFg, itemBg, maxTextW);

        rendered++;
    }

    // Scroll indicators (arrows) if content overflows
    if (pw >= WmMetric(48) && g_launcherScroll > 0)
    {
        // Wheel-scroll hints live in the header, clear of application labels.
        WmRenderMaterialString(backBuffer, stride, screenW, screenH,
                       px + static_cast<int32_t>(pw) - static_cast<int>(WmMetric(32)),
                       py + static_cast<int32_t>(WmMetric(LAUNCHER_PADDING)) + static_cast<int32_t>(textYOff),
                       "^", WmCurrentPalette().secondaryText, WmCurrentPalette().launcher);
    }
    if (pw >= WmMetric(48) && g_launcherScroll + maxVisible < totalValid)
    {
        // Down arrow beside the up hint.
        WmRenderMaterialString(backBuffer, stride, screenW, screenH,
                       px + static_cast<int32_t>(pw) - static_cast<int>(WmMetric(16)),
                       py + static_cast<int32_t>(WmMetric(LAUNCHER_PADDING)) + textYOff,
                       "v", WmCurrentPalette().secondaryText, WmCurrentPalette().launcher);
    }
}

int WmLauncherHitTest(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH)
{
    LauncherReadGuard guard;
    const auto& launcher = guard.get();
    if (!g_launcherOpen) return -1;

    int32_t px, py;
    uint32_t pw, ph;
    LauncherGetRect(screenW, screenH, &px, &py, &pw, &ph, launcher);

    // Outside panel?
    if (mx < px || mx >= px + static_cast<int32_t>(pw) ||
        my < py || my >= py + static_cast<int32_t>(ph))
        return -1;

    const int footerY=py+static_cast<int>(ph)-WmMetric(LAUNCHER_PADDING)-WmMetric(LAUNCHER_ITEM_HEIGHT);
    const int third=(static_cast<int>(pw)-2*WmMetric(LAUNCHER_PADDING))/3;
    const int relative=mx-px-WmMetric(LAUNCHER_PADDING);
    if(my>=footerY&&my<footerY+static_cast<int>(WmMetric(LAUNCHER_ITEM_HEIGHT))&&third>0&&relative>=0&&relative<3*third)
        return WM_LAUNCHER_MAX_ITEMS+relative/third;
    const LauncherLayout layout = GetLauncherLayout(screenW, screenH, launcher);
    uint32_t maxVisible = layout.capacity;

    // Map visual slot to actual launcher item, accounting for scroll
    uint32_t validIdx = 0;
    uint32_t slot = 0;
    for (uint32_t i = 0; i < launcher.count && slot < maxVisible; i++)
    {
        if (!launcher.items[i].valid) continue;
        if (validIdx < g_launcherScroll) { validIdx++; continue; }
        validIdx++;

        if (LauncherItemRect(layout, slot).contains(mx, my))
        {
            return static_cast<int>(i);
        }
        slot++;
    }

    return -1; // clicked in panel but not on an item (header or padding)
}

static int AdmitLauncherItem(const LauncherSnapshot& catalog,int itemIdx,bool tracked,
                             uint64_t* token,bool* coalesced)
{
    if(itemIdx<0||itemIdx>=static_cast<int>(catalog.count)||!catalog.items[itemIdx].valid)return -2;
    LauncherStartWorker();
    if(!__atomic_load_n(&g_launcherWorkerStarted,__ATOMIC_ACQUIRE))return -12;
    const auto& item=catalog.items[itemIdx];int slot;
    SpinLockAcquire(&g_launchLock);
    if(g_launchInhibited){SpinLockRelease(&g_launchLock);return -108;}
    uint64_t pending=0;
    for(const auto& e:g_launches.entries)if((e.busy()||e.inFlight())&&LaunchFeedbackTable::Exact(e.identity,item.catalogId)) {pending=e.token;break;}
    // Reserve durable tracking capacity before any launch admission side effect.
    if(tracked&&!g_launchHistory.CanTrack(pending)){SpinLockRelease(&g_launchLock);return -28;}
    bool created=false;
    slot=g_launches.Begin(ApicTickCount(),__atomic_load_n(&g_windowIdentity,__ATOMIC_ACQUIRE),
        item.title,item.appId,item.executable,item.catalogId,item.specification,&created,catalog.generation);
    if(slot>=0) {
        const auto& entry=g_launches.entries[slot];*token=entry.token;*coalesced=!created;
        if(created) {
            auto& request=g_launchRequests[slot];
            WmStrCopy(request.command,item.scriptPath,sizeof(request.command));
            request.desktop=item.isDesktopEntry;request.hasIcon=item.iconPixels!=nullptr;
            if(request.hasIcon)memcpy(request.icon,item.iconPixels,sizeof(request.icon));
        }
        if(tracked)g_launchHistory.Track(entry,created);
        else g_launchHistory.ShareWithUser(entry.token);
    }
    SpinLockRelease(&g_launchLock);
    g_launcherError=slot<0?slot:0;g_launcherOpen=slot<0;
    if(slot>=0) {
        g_taskbarPageTarget=-10-slot;__atomic_store_n(&g_launchBusy,true,__ATOMIC_RELEASE);
    }else SerialPuts(slot==-2?"WM: same app has pending launch from older catalog\n":"WM: launcher request queue full\n");
    CompositorMarkDirty();CompositorWake();
    return slot>=0?0:slot==-2?-16:-11;
}

void WmSetLaunchInhibited(bool inhibited) {
    SpinLockAcquire(&g_launchLock);g_launchInhibited=inhibited;
    if(inhibited)for(auto& entry:g_launches.entries)if(entry.phase==LaunchPhase::Queued) {
        const auto token=entry.token;entry={};
        const int index=g_launchHistory.Find(token);
        if(index>=0){g_launchHistory.records[index].cancelled=true;g_launchHistory.Complete(token,-125,ApicTickCount());}
    }
    SpinLockRelease(&g_launchLock);
}
bool WmLauncherDispatching() {
    bool dispatching=false;SpinLockAcquire(&g_launchLock);
    for(const auto& entry:g_launches.entries)if(entry.inFlight())dispatching=true;
    SpinLockRelease(&g_launchLock);return dispatching;
}
void WmLauncherExec(int itemIdx)
{
    if(itemIdx==WM_LAUNCHER_MAX_ITEMS){ShutdownDesktopRequest(false);return;}
    if(itemIdx==WM_LAUNCHER_MAX_ITEMS+1){ShutdownDesktopRequest(true);return;}
    if(itemIdx==WM_LAUNCHER_MAX_ITEMS+2){ShutdownDesktopCancel();return;}
    LauncherReadGuard guard;uint64_t token=0;bool coalesced=false;
    AdmitLauncherItem(guard.get(),itemIdx,false,&token,&coalesced);
}

int WmLauncherActivate(const char* appId,uint32_t generation,uint64_t* token,bool* coalesced)
{
    LauncherReadGuard guard;const auto& catalog=guard.get();
    if(!generation||catalog.generation!=generation)return -116;
    for(uint32_t i=0;i<catalog.count;++i)if(!strcmp(catalog.items[i].catalogId,appId))
        return AdmitLauncherItem(catalog,static_cast<int>(i),true,token,coalesced);
    return -2;
}

bool WmSnapshotLaunch(uint64_t token,LaunchRecord* out)
{
    SpinLockAcquire(&g_launchLock);int index=g_launchHistory.Find(token);
    if(index>=0)*out=g_launchHistory.records[index];
    SpinLockRelease(&g_launchLock);return index>=0;
}

uint32_t WmSnapshotLaunches(LaunchRecord* out,uint32_t capacity)
{
    uint32_t count=0;SpinLockAcquire(&g_launchLock);
    for(const auto& r:g_launchHistory.records)if(r.token&&count<capacity)out[count++]=r;
    SpinLockRelease(&g_launchLock);return count;
}

void WmCancelBridgeQueuedLaunches(uint32_t* cancelled,uint32_t* startedOrShared)
{
    *cancelled=*startedOrShared=0;
    SpinLockAcquire(&g_launchLock);
    for(auto& record:g_launchHistory.records) {
        if(!record.token||record.cancelled||record.dispatchFinished)continue;
        const int slot=g_launches.Find(record.token);
        if(!record.started&&record.bridgeOnly&&slot>=0&&g_launches.entries[slot].phase==LaunchPhase::Queued) {
            record.cancelled=true;g_launches.entries[slot]={};++*cancelled;
        }else if(record.started||(slot>=0&&g_launches.entries[slot].phase==LaunchPhase::Queued))++*startedOrShared;
    }
    SpinLockRelease(&g_launchLock);CompositorMarkDirty();
}

// ---------------------------------------------------------------------------
// Per-window VFB API (Phase A of wayland↔WM unification).
// ---------------------------------------------------------------------------

Window* WmFindWindowById(Process* proc, uint32_t wmId)
{
    if (!proc || wmId == 0) return nullptr;
    int idx = static_cast<int>(wmId) - 1;
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return nullptr;
    Window& w = g_windows[idx];
    if (w.proc != proc || w.wmId != wmId) return nullptr;
    return &w;
}

int WmSnapshotOwnedWindowState(Process* proc, uint32_t wmId,
                               uint64_t expectedIncarnation, BrookWindowState* out)
{
    if (!proc || !out) return -22; // EINVAL
    VfbGuard guard;
    const Window* w = WmFindWindowById(proc, wmId);
    if (!w) return -2; // ENOENT (also hides foreign windows)
    if (expectedIncarnation && expectedIncarnation != w->creationIdentity)
        return -116; // ESTALE
    BrookWindowState state{};
    state.version = BROOK_WINDOW_STATE_VERSION;
    state.window = w->wmId;
    state.incarnation = w->creationIdentity;
    state.client_x = w->clientX(); state.client_y = w->clientY();
    state.width = w->clientW; state.height = w->clientH;
    state.flags = (w->visible ? BROOK_WINDOW_STATE_VISIBLE : 0) |
        (w->focused ? BROOK_WINDOW_STATE_FOCUSED : 0) |
        (w->minimized ? BROOK_WINDOW_STATE_MINIMIZED : 0) |
        (w->state == WindowState::Maximized ? BROOK_WINDOW_STATE_MAXIMIZED : 0) |
        (w->noChrome ? BROOK_WINDOW_STATE_CLIENT_CHROME : 0) |
        (w->focusable ? BROOK_WINDOW_STATE_FOCUSABLE : 0);
    *out = state;
    return 0;
}

WmCreateWindowResult WmCreateWindowForProcess(Process* proc,
                                               uint16_t clientW,
                                               uint16_t clientH,
                                               const char* title,
                                               bool focusable)
{
    WmCreateWindowResult res = {};
    VfbGuard guard;

    if (!proc || clientW == 0 || clientH == 0) return res;
    // Provider buffers define their exact client extent (including tiny menus
    // and X11 utility windows). Interactive chrome resize minima are separate.

    // Cascade placement: each new window offset 30px from the prior count.
    static int s_cascade = 0;
    int16_t winX = static_cast<int16_t>(40 + (s_cascade % 8) * 30);
    int16_t winY = static_cast<int16_t>(40 + (s_cascade % 8) * 30);
    s_cascade++;

    int idx = CreateWindowLocked(proc, winX, winY, clientW, clientH, title, 1,
                             focusable);
    if (idx < 0) return res;

    Window& w = g_windows[idx];

    // Allocate the VFB pages kernel-side.
    // API-created windows may be relayed by a shared userspace compositor.
    // Its PID is provider identity, never an inferred application identity.
    w.clientPid=0;w.clientIncarnation=0;w.clientLaunchId=0;
    w.clientOrigin=WmClientOrigin::Unknown;
    uint32_t stride = clientW;
    uint64_t bytes  = static_cast<uint64_t>(stride) * clientH * 4;
    uint64_t pages  = (bytes + 4095) / 4096;

    VirtualAddress kAddr = VmmAllocPages(pages, VMM_WRITABLE,
                                          MemTag::Device, proc->pid);
    if (!kAddr)
    {
        SerialPrintf("WM: VFB alloc failed for window '%s' (%ux%u)\n",
                     w.title, clientW, clientH);
        DestroyWindowLocked(idx);
        return res;
    }
    auto* kVfb = reinterpret_cast<uint32_t*>(kAddr.raw());
    memset(kVfb, 0, bytes);

    auto* retirement=CompositorPrepareVfbRetirement(kAddr.raw(),pages);
    if (!retirement) { VmmFreePages(kAddr,pages);DestroyWindowLocked(idx);return res; }
    auto* backing=UserMappingBufferSnapshot(kAddr.raw(),pages*4096);
    int64_t mapped=backing?UserMappingBufferAlias(ProcessCurrent(),proc->addressSpace,backing,3):-12;
    if (mapped<0) {
        MappingBackingUnref(backing);CompositorCancelVfbRetirement(retirement);
        VmmFreePages(kAddr,pages);DestroyWindowLocked(idx);return res;
    }
    uint64_t userBase=static_cast<uint64_t>(mapped);
    w.vfbBacking=backing;
    w.vfbRetirement=retirement;

    w.vfb       = kVfb;
    w.vfbStride = stride;
    w.vfbWidth=clientW;w.vfbHeight=clientH;
    w.vfbBytes  = pages * 4096;
    w.vfbUser   = reinterpret_cast<void*>(userBase);
    AddressSpaceRef(proc->addressSpace);
    w.vfbAddressSpace = proc->addressSpace;
    w.vfbDirty  = 1;  // first frame

    SerialPrintf("WM: window %d '%s' VFB %ux%u kvfb=0x%lx user=0x%lx pid=%u\n",
                 idx, w.title, clientW, clientH,
                 reinterpret_cast<uint64_t>(kVfb), userBase, proc->pid);

    res.wmId      = w.wmId;
    res.vfbUser   = w.vfbUser;
    res.vfbStride = w.vfbStride;
    return res;
}

void WmDestroyWindowById(Process* proc, uint32_t wmId)
{
    VfbGuard guard;
    Window* w = WmFindWindowById(proc, wmId);
    if (!w) return;
    int idx = static_cast<int>(wmId) - 1;
    DestroyWindowLocked(idx);
}

int WmResizeVfbForProcess(Process* proc, uint32_t wmId,
                          uint16_t newW, uint16_t newH,
                          WmCreateWindowResult* outResult)
{
    if (!proc || !outResult) return -22; // -EINVAL
    if (!newW || !newH) return -22;

    VfbGuard guard;
    Window* w = WmFindWindowById(proc, wmId);
    if (!w) return -2; // -ENOENT
    if (!w->vfb) return -22; // -EINVAL — only for WM-API windows

    // Compute new size; bail if identical to current to avoid churn.
    uint32_t newStride = newW;
    uint64_t newBytes  = static_cast<uint64_t>(newStride) * newH * 4;
    uint64_t newPages  = (newBytes + 4095) / 4096;
    uint64_t roundedBytes = newPages * 4096;

    // Allocate new kernel-side VFB pages.
    VirtualAddress newKAddr = VmmAllocPages(newPages, VMM_WRITABLE,
                                             MemTag::Device, proc->pid);
    if (!newKAddr)
    {
        SerialPrintf("WM: resize_vfb wm=%u alloc %lu pages failed\n", wmId, newPages);
        return -12; // -ENOMEM
    }
    auto* newKVfb = reinterpret_cast<uint32_t*>(newKAddr.raw());
    memset(newKVfb, 0, roundedBytes);

    auto* retirement=CompositorPrepareVfbRetirement(newKAddr.raw(),newPages);
    if (!retirement) { VmmFreePages(newKAddr,newPages);return -12; }
    auto* newBacking=UserMappingBufferSnapshot(newKAddr.raw(),newPages*4096);
    int64_t mapped=newBacking?UserMappingBufferAlias(ProcessCurrent(),proc->addressSpace,newBacking,3):-12;
    if (mapped<0) {
        MappingBackingUnref(newBacking);CompositorCancelVfbRetirement(retirement);
        VmmFreePages(newKAddr,newPages);return -12;
    }
    uint64_t newUserBase=static_cast<uint64_t>(mapped);

    // Snapshot old mapping so the compositor can finish any in-flight
    // blit using w->vfb before we tear it down.
    auto* oldRetirement=w->vfbRetirement;
    MappingBacking* oldBacking = w->vfbBacking;
    AddressSpaceState* oldSpace = w->vfbAddressSpace;

    // Atomically swap the kernel VFB pointer + sizes so the compositor
    // sees a consistent view from this point on.  Mark dirty so the
    // first blit at the new size happens before any client commit.
    w->externalCaptured=false;w->externalW=w->externalH=0;
    w->publishedGeneration=NextPublication(*w);
    w->vfbRetirement = retirement;
    w->vfbBacking = newBacking;
    w->vfb       = newKVfb;
    w->vfbStride = newStride;
    w->vfbWidth=newW;w->vfbHeight=newH;
    w->vfbBytes  = roundedBytes;
    w->vfbUser   = reinterpret_cast<void*>(newUserBase);
    AddressSpaceRef(proc->addressSpace);
    w->vfbAddressSpace = proc->addressSpace;
    w->clientW   = newW;
    w->clientH   = newH;
    w->vfbDirty  = 1;

    if (oldSpace && oldBacking && UserMappingRemoveBacking(ProcessCurrent(),oldSpace,oldBacking)<0)
        KernelPanic("WM: invalid framebuffer mappings during resize");
    MappingBackingUnref(oldBacking);AddressSpaceUnref(oldSpace);
    // Only the independent kernel-buffer owner waits for renderer retirement.
    CompositorRetireVfb(oldRetirement);

    SerialPrintf("WM: resize_vfb wm=%u -> %ux%u stride=%u kvfb=0x%lx user=0x%lx\n",
                 wmId, newW, newH, newStride,
                 reinterpret_cast<uint64_t>(newKVfb), newUserBase);

    outResult->wmId      = wmId;
    outResult->vfbUser   = w->vfbUser;
    outResult->vfbStride = w->vfbStride;
    // Client-accepted sizes can differ from the WM offer. Invalidate cached
    // caption geometry along with publishing the replacement client buffer.
    CompositorMarkDirty();CompositorWake();
    return 0;
}

bool WmSnapshotVfb(const Window* window,uint32_t** pixels,uint32_t* stride,uint64_t* bytes) {
    VfbGuard guard;
    *pixels=window->vfb;*stride=window->vfbStride;*bytes=window->vfbBytes;
    return *pixels && *stride && *bytes;
}

uint64_t WmMapWindowVfb(Process* caller,uint32_t wmId) {
    if (!caller) return 0;
    VfbGuard guard;
    for (uint32_t i=0;i<WM_MAX_WINDOWS;++i) {
        const auto& window=g_windows[i];
        if (window.proc && window.wmId==wmId && window.vfbBacking) {
            int64_t mapped=UserMappingBufferAlias(ProcessCurrent(),caller->addressSpace,window.vfbBacking,1);
            return mapped<0?0:static_cast<uint64_t>(mapped);
        }
    }
    return 0;
}

void WmSignalDirtyById(Process* proc, uint32_t wmId)
{
    VfbGuard guard;
    Window* w = WmFindWindowById(proc, wmId);
    if (!w) return;
    // This syscall publishes CPU-written VFB content, including a surface
    // switching from dmabuf to SHM. Stop sampling any previous GL buffer.
    w->externalCaptured = false;
    w->publishedGeneration=NextPublication(*w);
    w->externalW = w->externalH = 0;
    w->vfbDirty = 1;
    CompositorWake();
}

bool WmReserveCapture(Process* proc,uint32_t wmId,uint32_t width,uint32_t height,WmCaptureToken* out)
{
    VfbGuard guard;
    auto* win=WmFindWindowById(proc,wmId);
    if (!win || !width || !height || !out || win->publicationGeneration>=~uint64_t{0}-1) return false;
    *out={wmId,width,height,win->creationIdentity,++win->publicationGeneration};
    return true;
}
bool WmGetCaptureBinding(Process* proc,uint32_t wmId,WmCaptureToken* out)
{
    VfbGuard guard;auto* win=WmFindWindowById(proc,wmId);
    if (!win || !out) return false;
    *out={wmId,1,1,win->creationIdentity,win->publicationGeneration};return true;
}
static Window* CaptureWindowLocked(const WmCaptureToken& token)
{
    if (!token.wmId || token.wmId>WM_MAX_WINDOWS) return nullptr;
    auto& win=g_windows[token.wmId-1];
    return win.proc && win.creationIdentity==token.incarnation &&
           win.publicationGeneration==token.generation ? &win : nullptr;
}
bool WmReserveBoundCapture(const WmCaptureToken& binding,uint32_t width,uint32_t height,WmCaptureToken* out)
{
    VfbGuard guard;
    if (!out || !width || !height || !binding.wmId || binding.wmId>WM_MAX_WINDOWS) return false;
    auto& win=g_windows[binding.wmId-1];
    if (!win.proc || win.creationIdentity!=binding.incarnation || win.publicationGeneration>=~uint64_t{0}-1)
        return false;
    *out={binding.wmId,width,height,win.creationIdentity,++win.publicationGeneration};return true;
}
bool WmCaptureCurrent(const WmCaptureToken& token)
{
    VfbGuard guard;return CaptureWindowLocked(token)!=nullptr;
}
bool WmCompleteCapture(const WmCaptureToken& token)
{
    VfbGuard guard;
    auto* win=CaptureWindowLocked(token);
    if (!win) return false;
    win->externalCaptured=true;win->externalW=token.w;win->externalH=token.h;
    win->publishedGeneration=token.generation;win->vfbDirty=1;
    CompositorWake();return true;
}
WmContentSnapshot WmSnapshotContent(uint32_t slot)
{
    VfbGuard guard;
    if (slot>=WM_MAX_WINDOWS || !g_windows[slot].proc) return {};
    const auto& w=g_windows[slot];
    return {true,w.externalCaptured,w.creationIdentity,w.publishedGeneration,w.externalW,w.externalH};
}

void WmAcknowledgeContent(uint32_t slot,const WmContentSnapshot& uploaded)
{
    VfbGuard guard;
    if (slot>=WM_MAX_WINDOWS) return;
    auto& w=g_windows[slot];
    if (w.proc && w.creationIdentity==uploaded.incarnation &&
        w.publishedGeneration==uploaded.generation && !w.externalCaptured) w.vfbDirty=0;
}

bool WmSetAppIdById(Process* proc, uint32_t wmId, const char* appId)
{
    VfbGuard guard;
    Window* w = WmFindWindowById(proc, wmId);
    if (!w || !appId) return false;
    WmStrCopy(w->appId, appId, sizeof(w->appId));
    CompositorMarkDirty();
    CompositorWake();
    return true;
}

bool WmRegisterClientOrigin(Process* provider,uint32_t wmId,uint16_t clientPid,
                            uint64_t incarnation,uint64_t launchId)
{
    VfbGuard guard;
    Window* w=WmFindWindowById(provider,wmId);
    if (!w || !w->vfb || !clientPid || !incarnation) return false;
    if (w->clientOrigin!=WmClientOrigin::Unknown)
        return w->clientOrigin==WmClientOrigin::RegisteredUnixPeer &&
               w->clientPid==clientPid && w->clientIncarnation==incarnation && w->clientLaunchId==launchId;
    w->clientPid=clientPid;w->clientIncarnation=incarnation;w->clientLaunchId=launchId;
    w->clientOrigin=WmClientOrigin::RegisteredUnixPeer;
    CompositorWake();return true;
}

void WmSetTitleById(Process* proc, uint32_t wmId, const char* title)
{
    VfbGuard guard;
    Window* w = WmFindWindowById(proc, wmId);
    if (!w || !title) return;
    WmStrCopy(w->title, title, sizeof(w->title));
    CompositorMarkDirty();
    CompositorWake();
}

bool WmInputPush(Window* win, const InputEvent& ev, int16_t localX, int16_t localY, uint32_t captureToken)
{
    if (!win) return false;
    uint32_t head = __atomic_load_n(&win->inputHead, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&win->inputTail, __ATOMIC_ACQUIRE);
    uint32_t next = (head + 1) % Window::WM_INPUT_QUEUE;
    if (next == tail) {
        // Queue full — for mouse-move events, coalesce by overwriting the
        // newest entry if it's also a mouse-move (consumer only needs latest
        // position). For other event types, drop with a rate-limited warning.
        if (ev.type == InputEventType::MouseMove && head != tail)
        {
            uint32_t prev = (head + Window::WM_INPUT_QUEUE - 1) % Window::WM_INPUT_QUEUE;
            Window::WmInputEvent& slot = win->inputQueue[prev];
            if (slot.type == static_cast<uint8_t>(InputEventType::MouseMove))
            {
                slot.x = localX;
                slot.y = localY;
                slot.modifiers = ev.modifiers;
                return true; // coalesced
            }
        }
        // Rate-limit the warning: only print once per ~256 drops per window
        win->inputDropCount++;
        if ((win->inputDropCount & 0xFF) == 1)
            SerialPrintf("WmInputPush: queue FULL wm=%u type=%d (dropped %u)\n",
                         win->wmId, (int)ev.type, win->inputDropCount);
        return false;
    }
    Window::WmInputEvent& slot = win->inputQueue[head];
    slot.type      = static_cast<uint8_t>(ev.type);
    slot.scanCode  = ev.scanCode;
    slot.ascii     = static_cast<uint8_t>(ev.ascii);
    slot.modifiers = ev.modifiers;
    slot.x         = localX;
    slot.y         = localY;
    slot.reserved  = captureToken;
    __atomic_store_n(&win->inputHead, next, __ATOMIC_RELEASE);
    return true;
}

uint32_t WmInputPop(Window* win, Window::WmInputEvent* out, uint32_t max)
{
    if (!win || !out || !max) return 0;
    uint32_t produced = 0;
    while (produced < max)
    {
        uint32_t tail = __atomic_load_n(&win->inputTail, __ATOMIC_ACQUIRE);
        uint32_t head = __atomic_load_n(&win->inputHead, __ATOMIC_ACQUIRE);
        if (tail == head) break;
        out[produced++] = win->inputQueue[tail];
        __atomic_store_n(&win->inputTail,
                         (tail + 1) % Window::WM_INPUT_QUEUE,
                         __ATOMIC_RELEASE);
    }
    return produced;
}

bool WmPushWmEvent(Window* win, uint8_t type, int16_t x, int16_t y)
{
    if (!win) return false;
    uint32_t head = __atomic_load_n(&win->inputHead, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&win->inputTail, __ATOMIC_ACQUIRE);
    uint32_t next = (head + 1) % Window::WM_INPUT_QUEUE;
    if (next == tail) return false;
    Window::WmInputEvent& slot = win->inputQueue[head];
    slot.type      = type;
    slot.scanCode  = 0;
    slot.ascii     = 0;
    slot.modifiers = 0;
    slot.x         = x;
    slot.y         = y;
    slot.reserved  = type==WM_EVT_RESIZED ? (win->state==WindowState::Maximized ? WM_RESIZE_MAXIMIZED : WM_RESIZE_RESIZING) : 0;
    __atomic_store_n(&win->inputHead, next, __ATOMIC_RELEASE);
    return true;
}

} // namespace brook
