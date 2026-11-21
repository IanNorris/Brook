#include <cstdio>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <chrono>
#include <atomic>
#include "../../kernel/src/compositor.cpp"
#include "../../kernel/src/desktop_frame_scenes.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"CAPTURE_QUEUE line %d: %s\n",__LINE__,#x);std::abort(); } } while(0)
namespace {
std::mutex queueMutex,windowMutex,frameMutex,geometryMutex;
thread_local unsigned locks;
std::atomic<int> leases{0};
std::atomic<unsigned> wakeRequests{0};
int expectedFlagsAtWake=-1;
std::atomic<bool> workerDone{false};
std::thread worker;
int mode=0,created=0,destroyed=0;
uint64_t backdropStyle=2;unsigned readsUntilStyleChange=0;
bool allocationFails=false;
unsigned sourcePixel=0x00ff00,pixels[256]{};
unsigned displayFlushes=0;
bool ttySuppressed=false,screenIdleDisabled=false;
constexpr uint8_t lettering[] = {255};
constexpr brook::GlyphInfo letters[] = {{0, 0, 1, 1, 0, 1, 1}};
brook::Process* caller;
brook::WmContentSnapshot content{true,false,1,1,0,0};
uint64_t latest=1;
brook::CaptureRequest* running;
brook::GpuCompositorOps ops{};

void CaptureOverlayAt(uint64_t now, int32_t mx, int32_t my, bool cursor) {
    using namespace brook;
    const auto frame = CaptureShellFrame(g_physFbWidth, g_physFbHeight, now, mx, my);
    CaptureDesktopOverlay(frame, now, mx, my, cursor);
}

void CheckOverlayAt(uint64_t now, int32_t mx, int32_t my) {
    using namespace brook;
    CheckDesktopOverlay(now, CaptureShellFrame(g_physFbWidth, g_physFbHeight, now, mx, my));
}
}
namespace brook {
const FontAtlas g_fontAtlas{lettering, 1, 1, letters, 1, 'I', 1, 0, 1};
const FontAtlas g_desktopFont200{lettering, 1, 1, letters, 1, 'I', 1, 0, 1};
uint64_t WmBackdropStyleVersion(){
    if(readsUntilStyleChange && --readsUntilStyleChange==0)backdropStyle+=2;
    return backdropStyle;
}
void SerialPrintf(const char*,...) {}
volatile uint64_t g_lapicTickCount=0;
uint64_t ApicTickCount() { return g_lapicTickCount; }
uint64_t captureClockNs=50000;
bool KvmClockAvailable(){return true;}
uint64_t KvmClockReadNs(){return captureClockNs;}
bool TscClockAvailable(){return false;}
uint64_t TscClockReadNs(){return 0;}
int WmCopyDebugWindowBuffer(uint32_t id,uint64_t incarnation,uint32_t* out,uint64_t capacity,
                            uint32_t* width,uint32_t* height,uint32_t* scale) {
    if(id!=9 || incarnation!=90)return -2;
    *width=2;*height=1;*scale=2;
    if(out){
        CHECK(capacity>=2);out[0]=0x223344;out[1]=0x667788;
        captureClockNs+=15000;
    }
    return 0;
}
void KMutexLock(KMutex* m) { (m==&g_debugFrameLock?frameMutex:m==&g_scanoutMappingLock?geometryMutex:queueMutex).lock();++locks; }
void KMutexUnlock(KMutex* m) { CHECK(locks>0);--locks;(m==&g_debugFrameLock?frameMutex:m==&g_scanoutMappingLock?geometryMutex:queueMutex).unlock(); }
VirtualAddress VmmAllocPages(uint64_t,uint64_t,MemTag,uint16_t) { return VirtualAddress{}; }
void VmmFreePages(VirtualAddress,uint64_t) { std::abort(); }
uint32_t WmDesktopHeight(uint32_t h) { return h-1; }
bool taskbarHidden=false;
bool WmTaskbarHidden() { return taskbarHidden; }
WmPalette WmCurrentPalette() {
    static const WmPalette palette = [] { WmPalette value{}; value.taskbar = 0x00abc123; return value; }();
    return palette;
}
bool launcherVisible=true;
bool WmLauncherVisible() {return launcherVisible;}
uint64_t InputLatestSequence(){return 0;}
Process* waiterProc;int blockCalls=0;bool captureOnBlock=false;unsigned waiterWakes=0;
Process* ProcessCurrent(){return waiterProc;}
void SchedulerBlock(Process* p){
    CHECK(!locks && p==waiterProc);++blockCalls;
    if(captureOnBlock){captureOnBlock=false;CaptureOverlayAt(g_lapicTickCount,0,0,false);}
    else g_lapicTickCount=p->wakeupTick;
}
void WmLauncherGetRect(uint32_t,uint32_t,int32_t* x,int32_t* y,uint32_t* w,uint32_t* h){*x=1;*y=1;*w=2;*h=2;}
bool volumeVisible=false;
bool WmTrayTooltipGetRect(uint32_t,uint32_t,int32_t,int32_t,int32_t*,int32_t*,uint32_t*,uint32_t*){return false;}
bool WmVolumePopupVisible(){return volumeVisible;}
void WmVolumePopupGetRect(uint32_t,uint32_t,int32_t* x,int32_t* y,uint32_t* w,uint32_t* h){*x=4;*y=1;*w=2;*h=2;}
bool WmPointerCaptureActive(){return false;}
void WmGetShellTrayGeometry(uint32_t, uint32_t, int32_t, int32_t, WmShellTrayGeometry& out) { out = {}; }
void DesktopDiskSetVisible(bool) {}
void DesktopStatsSetVisible(DesktopStatsPanel) {}
DesktopStatsPanel WmDetailedStatsSelected() { return DesktopStatsPanel::None; }
bool WmDiskFlyoutSelected() { return false; }
bool WmTrayFlyoutRect(uint32_t,uint32_t,int32_t*,int32_t*,uint32_t*,uint32_t*){return false;}
bool dockVisible=false;
bool WmDockSurfaceRect(uint32_t,uint32_t,int32_t* x,int32_t* y,uint32_t* w,uint32_t* h,bool* material){
    *x=0;*y=3;*w=1;*h=1;*material=true;return dockVisible;
}
bool summaryAndHudVisible = false, extraPopupsVisible = false;
bool WmPerformanceHudRect(uint32_t,uint32_t,int32_t* x,int32_t* y,uint32_t* w,uint32_t* h) {
    *x = 3; *y = 3; *w = 1; *h = 1;
    return summaryAndHudVisible;
}
uint32_t WmCollectTaskbarPopupSurfaces(uint32_t,uint32_t,WmShellSurface* out) {
    uint32_t count = 0;
    if (volumeVisible) out[count++] = {
        WmShellSurfaceKind::VolumePopup, summaryAndHudVisible ? 1 : 4, 1, 2, 2, true,
        summaryAndHudVisible ? -3 : 0
    };
    if (extraPopupsVisible) {
        out[count++] = {WmShellSurfaceKind::TrayFlyout, 3, 1, 1, 1, true};
        out[count++] = {WmShellSurfaceKind::CalendarFlyout, 5, 1, 1, 1, true};
    }
    if (summaryAndHudVisible) out[count++] = {WmShellSurfaceKind::PerformanceHud,3,3,1,1,true};
    return count;
}
bool WmBootSummaryRect(uint32_t,uint32_t,int32_t* x,int32_t* y,uint32_t* w,uint32_t* h) {
    *x = 7; *y = 0; *w = 1; *h = 1;
    return summaryAndHudVisible;
}
// Stage a publication for the next compositor frame. The fake rasterizer
// counts cache misses and paints distinguishable data for each surface.
uint64_t shellKeyBump=0, publishedKeyBump=0;
unsigned taskbarDraws=0;
bool shellRunning=true, publishedShellRunning=true;
BrookDesktopSurface WmShellSurfaceRecord(const WmShellSurface& s,uint64_t key){
    BrookDesktopSurface r{};r.kind=uint32_t(s.kind)+3;r.key=key;r.x=s.x;r.y=s.y;r.width=s.w;r.height=s.h;
    if (s.material && CompositorGlassActive()) r.material = BROOK_DESKTOP_MATERIAL_GLASS;
    else r.flags = BROOK_DESKTOP_SURFACE_OPAQUE;
    r.opacity = 255;
    return r;
}
void ShellServiceBeginFrame(uint64_t) {
    publishedKeyBump = shellKeyBump;
    publishedShellRunning = shellRunning;
}
bool ShellServiceWarmLauncher(uint32_t, uint32_t, uint64_t) { return false; }
void ShellServiceEndCapture(uint32_t) {}
uint64_t ShellServiceCapture(const WmShellSurface& s,uint32_t,uint32_t,uint64_t,int32_t,int32_t) {
    if (!publishedShellRunning) return 0;
    return 0x100 + uint64_t(s.kind) + (s.kind == WmShellSurfaceKind::Taskbar ? publishedKeyBump : 0);
}
void ShellServiceDraw(const WmShellSurface& s,uint32_t* p,uint32_t stride,uint32_t,uint32_t) {
    CHECK(publishedShellRunning);
    uint32_t color = 0;
    switch (s.kind) {
    case WmShellSurfaceKind::Taskbar: ++taskbarDraws; color = 0x00123456; break;
    case WmShellSurfaceKind::Launcher: color = 0x00654321; break;
    case WmShellSurfaceKind::VolumePopup: color = 0x00777777; break;
    case WmShellSurfaceKind::PerformanceHud: color = 0x00112233; break;
    case WmShellSurfaceKind::BootSummary: color = 0x00445566; break;
    case WmShellSurfaceKind::Dock: color = 0x00998877; break;
    case WmShellSurfaceKind::TrayFlyout: color = 0x00334455; break;
    case WmShellSurfaceKind::CalendarFlyout: color = 0x00224466; break;
    default: std::abort();
    }
    for (uint32_t y = 0; y < s.h; ++y)
        for (uint32_t x = 0; x < s.w; ++x)
            p[(s.y + y)*stride + s.x + x] = color;
}
extern "C" void* kmalloc(uint64_t bytes) { return allocationFails?nullptr:std::calloc(1,bytes); }
extern "C" void kfree(void* p) { std::free(p); }
void MappingBackingRef(MappingBacking*) { ++leases; }
void MappingBackingUnref(MappingBacking*) { --leases; }
void SchedulerUnblock(Process* p) {
    if(p && p==waiterProc){++waiterWakes;return;}
    CHECK(p==g_compositorProcess);
    if(expectedFlagsAtWake>=0)CHECK(g_debugFrameFlags==unsigned(expectedFlagsAtWake));
    ++wakeRequests;
}
void DisplayFlush(uint32_t first,uint32_t last) { CHECK(first<last); ++displayFlushes; }
bool DisplayPrepareCursor(const CursorImage*, int32_t, int32_t) { return false; }
void DisplayCommitCursor() {}
void DisplayPollModeHints() {}
void WmApplyDisplaySize(uint32_t, uint32_t) {}
uint64_t DisplayGetFramebufferPhys(){return 0;}
bool BootLogoActive(){return false;}
bool TtyDisplaySuppressed(){return ttySuppressed;}
void TtySuppressDisplay(bool suppress){ttySuppressed=suppress;}
bool DisplayPowerSetIdle(uint32_t timeout){screenIdleDisabled=!timeout;return true;}
const GpuCompositorOps* GpuCompositorGet() { return &ops; }
WmContentSnapshot WmSnapshotContent(uint32_t slot) {
    std::lock_guard<std::mutex> guard(windowMutex);return slot==0?content:WmContentSnapshot{};
}
bool WmCaptureCurrent(const WmCaptureToken& t) {
    std::lock_guard<std::mutex> guard(windowMutex);
    return content.live && t.wmId==1 && content.incarnation==t.incarnation && latest==t.generation;
}
bool WmCompleteCapture(const WmCaptureToken& t) {
    CHECK(!locks);std::lock_guard<std::mutex> guard(windowMutex);
    if (!content.live || content.incarnation!=t.incarnation || latest!=t.generation) return false;
    content={true,true,t.incarnation,t.generation,t.w,t.h};return true;
}
extern "C" void SchedulerSleepMs(uint32_t) {
    if (mode==4) { g_lapicTickCount+=6000;return; }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
}
namespace {
brook::WmCaptureToken token() {
    std::lock_guard<std::mutex> guard(windowMutex);return {1,4,4,content.incarnation,++latest};
}
void startWorker() {
    CHECK(!worker.joinable());workerDone=false;
    worker=std::thread([] {
        for (;;) {
            brook::KMutexLock(&brook::g_captureLock);
            running=brook::g_captureHead;
            brook::KMutexUnlock(&brook::g_captureLock);
            if (running) break;
            std::this_thread::yield();
        }
        brook::DrainOwnedCaptures();workerDone=true;
    });
}
uint32_t expectedSourceFormat=2;
int capture(uint32_t format=2) {
    expectedSourceFormat=format;
    auto t=token();startWorker();
    const int r=brook::CompositorCaptureExternal(caller,t,reinterpret_cast<brook::MappingBacking*>(1),256,65536,format);
    worker.join();CHECK(workerDone);return r;
}
}
int main() {
    using namespace brook;
    caller=static_cast<Process*>(std::calloc(1,sizeof(Process)));CHECK(caller);
    auto* compositor=static_cast<Process*>(std::calloc(1,sizeof(Process)));CHECK(compositor);
    g_compositorProcess=compositor;
    // Exercise the real queue with the worker still Running, as when work
    // arrives after its last drain and before it reaches SchedulerBlock.
    compositor->state=ProcessState::Running;
    g_gpuComposite=g_gpuDrawReady=true;
    ops.CreateOwnedTexture=[](uint32_t,uint32_t,bool)->GpuTexId { CHECK(!locks);return mode==6?0:++created; };
    ops.DestroyOwnedTexture=[](GpuTexId)->bool { CHECK(!locks);++destroyed;return true; };
    ops.DestroyTexture=[](GpuTexId) { CHECK(!locks);++destroyed; };
    ops.CaptureExternal=[](GpuTexId dst,int32_t,uint32_t,uint32_t format,uint32_t,uint32_t) {
        CHECK(format==expectedSourceFormat);
        CHECK(!locks);pixels[dst]=sourcePixel;
        if (mode==1) { std::lock_guard<std::mutex> g(windowMutex);++latest;content.externalCaptured=false;content.generation=latest; }
        if (mode==2) { std::lock_guard<std::mutex> g(windowMutex);++content.incarnation;latest=1;content.externalCaptured=false; }
        if (mode==3) {
            __atomic_store_n(&caller->terminalPending,true,__ATOMIC_RELEASE);
            for (;;) {
                KMutexLock(&g_captureLock);bool canceled=running->canceled;KMutexUnlock(&g_captureLock);
                if (canceled) break;
                std::this_thread::yield();
            }
        }
        return mode==5?GpuCaptureResult::Uncertain:GpuCaptureResult::Complete;
    };
    CHECK(CompositorCaptureExternal(caller,token(),reinterpret_cast<MappingBacking*>(1),256,65536,28)==-22);
    CHECK(leases==0 && created==0 && g_captureCount==0);
    CHECK(capture()==0 && leases==0 && created==1);
    CHECK(wakeRequests>0); // queued capture must reach the scheduler in this state
    const auto wakes= wakeRequests.load();
    compositor->state=ProcessState::Ready;CompositorWake();CHECK(wakeRequests==wakes+1);
    compositor->state=ProcessState::Blocked;CompositorWake();CHECK(wakeRequests==wakes+2);
    compositor->state=ProcessState::Running;
    auto first=g_winTex[0].tex;CHECK(pixels[first]==0x00ff00);
    sourcePixel=0x0000ff;CHECK(pixels[first]==0x00ff00);
    CHECK(capture()==0 && created==2 && leases==0);
    CHECK(capture()==0 && created==2); // staging reuse, bounded allocations
    const auto retainedFront=g_winTex[0].tex;
    ops.DestroyOwnedTexture(g_winTex[0].staging);g_winTex[0].staging=0;
    mode=6;CHECK(capture()==-12 && g_winTex[0].tex==retainedFront && leases==0);
    mode=0;allocationFails=true;auto oom=token();
    CHECK(CompositorCaptureExternal(caller,oom,reinterpret_cast<MappingBacking*>(1),256,65536,2)==-12);
    allocationFails=false;g_captureCount=MAX_CAPTURE_REQUESTS;
    CHECK(CompositorCaptureExternal(caller,token(),reinterpret_cast<MappingBacking*>(1),256,65536,2)==-11 && leases==0);
    g_captureCount=0;
    const auto beforeShm=g_winTex[0].tex;const auto priorPixel=pixels[beforeShm];
    sourcePixel=0xff0000;
    mode=1;CHECK(capture()==-116 && leases==0 && pixels[beforeShm]==priorPixel); // SHM beats in-flight GPU copy
    CHECK(!content.externalCaptured);
    mode=0;CHECK(capture()==0 && content.externalCaptured);
    mode=2;CHECK(capture()==-116 && leases==0); // destroyed/reused identity
    mode=0;CHECK(capture()==0);const auto beforeCancel=g_winTex[0].tex;
    const auto cancelPixel=pixels[beforeCancel];sourcePixel=0xffff00;
    mode=3;CHECK(capture()==-116 && leases==0 && g_winTex[0].tex==beforeCancel && pixels[beforeCancel]==cancelPixel); // fatal cancellation waits for safe completion
    CHECK(!g_captureCount && !g_captureHead && !g_captureTail);
    __atomic_store_n(&caller->terminalPending,false,__ATOMIC_RELEASE);
    mode=4;auto t=token();CHECK(CompositorCaptureExternal(caller,t,reinterpret_cast<MappingBacking*>(1),256,65536,2)==-110);
    CHECK(!g_captureCount && !g_captureHead && !g_captureTail && leases==0);
    mode=5;CHECK(capture()==-5 && leases==1 && g_captureCount==1 && g_captureQuarantine);
    CHECK(g_captureQuarantine->state==CaptureState::Done);
    // Simulate device reset only for test cleanup. Production retains this lease.
    auto* retained=g_captureQuarantine;g_captureQuarantine=nullptr;g_captureCount=0;CaptureUnref(retained);CHECK(leases==0);
    mode=0;CHECK(capture(1)==0 && leases==0); // canonical ARGB reaches the worker unchanged
    DestroyWindowTexture(&ops,g_winTex[0]);std::free(caller);
    // Exercise the compositor's actual one-shot display probe against a fake
    // MMIO surface, including stride, corner readback and failure repaint.
    uint32_t framebuffer[48]{},source[8]={1,2,3,99,4,5,6,99},samples[4]{};
    const uint32_t wallpaper[]={10,11,12,13,14,15};uint32_t backdrop[6]={};
    DesktopBackdropSpan(backdrop,0,6,1,6,4,wallpaper,3,2);
    CHECK(backdrop[0]==0x1a3a && backdrop[1]==10 && backdrop[2]==11 && backdrop[3]==12 && backdrop[4]==0x1a3a);
    DesktopBackdropSpan(backdrop,2,5,2,6,4,wallpaper,3,2);
    CHECK(backdrop[0]==14 && backdrop[1]==15 && backdrop[2]==0x1a3a);
    DesktopBackdropSpan(backdrop,0,3,0,3,1,wallpaper,6,1);
    CHECK(backdrop[0]==10 && backdrop[1]==11 && backdrop[2]==12);
    DesktopBackdropSpan(backdrop,0,3,0,3,1,nullptr,0,0);
    CHECK(backdrop[0]==0x1a3a && backdrop[1]==0x1a3a && backdrop[2]==0x1a3a);
    g_gpuComposite=false;g_physFb=framebuffer;g_physFbWidth=8;g_physFbHeight=6;g_physFbStride=8;
    g_wallpaper=const_cast<uint32_t*>(wallpaper);g_wallpaperWidth=3;g_wallpaperHeight=2;
    CompositorBackdropInfo backdropInfo{};CHECK(CompositorGetBackdropInfo(&backdropInfo));
    CHECK(CompositorReadBackdropSpan(backdropInfo,2,1,4,backdrop));
    CHECK(backdrop[0]==0xff001a3a && backdrop[1]==0xff00000a && backdrop[2]==0xff00000b && backdrop[3]==0xff00000c);
    uint32_t replacement[]={21,22,23,24,25,26};
    CompositorSetWallpaper(replacement,3,2);
    CHECK(!CompositorReadBackdropSpan(backdropInfo,2,2,4,backdrop));
    CompositorBackdropInfo changedBackdrop{};CHECK(CompositorGetBackdropInfo(&changedBackdrop));
    CHECK(changedBackdrop.generation!=backdropInfo.generation && changedBackdrop.display==backdropInfo.display);
    CHECK(CompositorReadBackdropSpan(changedBackdrop,2,2,3,backdrop));
    CHECK(backdrop[0]==0xff000015 && backdrop[1]==0xff000016 && backdrop[2]==0xff000017);
    backdropStyle+=2;
    CHECK(!CompositorReadBackdropSpan(changedBackdrop,2,2,3,backdrop));
    CHECK(CompositorGetBackdropInfo(&changedBackdrop));
    CHECK(CompositorReadBackdropSpan(changedBackdrop,2,2,3,backdrop) && backdrop[0]==0xff000015);
    readsUntilStyleChange=2;
    CHECK(!CompositorReadBackdropSpan(changedBackdrop,2,2,3,backdrop));
    CHECK(CompositorGetBackdropInfo(&changedBackdrop));
    ++backdropStyle;CHECK(!CompositorGetBackdropInfo(&backdropInfo));++backdropStyle;
    CHECK(CompositorGetBackdropInfo(&backdropInfo));++g_displayGeneration;
    CHECK(!CompositorReadBackdropSpan(backdropInfo,2,2,4,backdrop));
    g_wallpaper=nullptr;g_wallpaperWidth=g_wallpaperHeight=0;
    auto copy=[](void* context,uint64_t offset,void* dest,uint64_t bytes){
        __builtin_memcpy(dest,static_cast<uint8_t*>(context)+offset,bytes);return true;
    };
    CHECK(!CompositorPresentProbe(2,1,3,2,4,source,copy,samples));
    CHECK(framebuffer[10]==1 && framebuffer[11]==2 && framebuffer[12]==3 && framebuffer[18]==4 && framebuffer[20]==6);
    CHECK(samples[0]==1 && samples[1]==3 && samples[2]==4 && samples[3]==6);
    const auto display=CompositorDisplayGeneration();CHECK(display);
    const auto staleBadgeBackdrop=g_backdropGeneration;backdropStyle+=2;
    CHECK(CompositorPresentProbe(0,0,8,6,8,framebuffer,copy,samples,display,staleBadgeBackdrop,42)==-116);
    CHECK(g_backdropGeneration!=staleBadgeBackdrop);
    framebuffer[10]=77;
    CHECK(CompositorPresentProbe(2,1,3,2,4,source,copy,samples,display+1)==-116 && framebuffer[10]==77);
    CHECK(CompositorPresentProbe(2,1,3,2,4,source,copy,samples,display,g_backdropGeneration+1)==-116 && framebuffer[10]==77);
    CHECK(!CompositorPresentProbe(2,1,3,2,4,source,copy,samples,display,g_backdropGeneration));
    CHECK(!CompositorPresentProbe(2,1,3,2,4,source,copy,samples,display) && framebuffer[10]==1);
    CHECK(CompositorPresentProbe(7,1,3,2,4,source,copy,samples)==-22);
    uint32_t fullSource[48];for(unsigned i=0;i<48;++i)fullSource[i]=0x12340000u+i;
    g_lapicTickCount=100;g_compositorEpoch=31;
    CHECK(CompositorPresentProbe(0,0,8,6,8,fullSource,copy,nullptr,display)==-22);
    CHECK(!CompositorPresentProbe(0,0,8,6,8,fullSource,copy,nullptr,display,g_backdropGeneration,42));
    CHECK(framebuffer[0]==fullSource[0] && framebuffer[47]==fullSource[47]);
    CHECK(!CompositorPresentProbe(0,0,8,6,8,fullSource,copy,samples,display,g_backdropGeneration,42));
    CHECK(g_presentLease.owner==42 && g_presentLease.deadline==100+COMPOSITOR_PRESENT_LEASE_MS && framebuffer[47]==fullSource[47]);
    // Exercise dependency coverage against the real software scanout and
    // lease implementation. This fixture does not exercise the Intel ioctl.
    int sceneMarkers[2]{};DesktopFrameScenes<int> dependencies{};
    CHECK(dependencies.Insert({42,11,1,display,g_backdropGeneration,0,false,&sceneMarkers[0],false}));
    CHECK(dependencies.Insert({42,12,2,display,g_backdropGeneration,0,false,&sceneMarkers[1],false}));
    uint32_t olderPixels[]={101,102};CompositorPresentRegion olderRegion{2,1,2,1,2,0};
    CHECK(!CompositorPresentRegions(&olderRegion,1,sizeof(olderPixels),olderPixels,copy,display,g_backdropGeneration,42));
    DesktopDependencyRect olderWrite{2,1,2,1};dependencies.RecordAccepted(42,11,&olderWrite,1,false);
    DesktopDependencyRect missingWrite{2,1,1,1},coveredWrites[]={{2,1,1,1},{3,1,1,1}};
    CHECK(!DesktopDependenciesCovered(dependencies.Find(42,12)->dependencies,&missingWrite,1));
    CHECK(DesktopDependenciesCovered(dependencies.Find(42,12)->dependencies,coveredWrites,2));
    CHECK(framebuffer[10]==101 && framebuffer[11]==102);
    // A rejected update makes no framebuffer change; a covering successor
    // reconstructs both pixels using the actual region presentation path.
    uint32_t newerPixels[]={201,202};
    if(DesktopDependenciesCovered(dependencies.Find(42,12)->dependencies,&missingWrite,1))
        (void)CompositorPresentRegions(&olderRegion,1,sizeof(newerPixels),newerPixels,copy,display,g_backdropGeneration,42);
    CHECK(framebuffer[10]==101 && framebuffer[11]==102);
    CHECK(DesktopDependenciesCovered(dependencies.Find(42,12)->dependencies,coveredWrites,2));
    CHECK(!CompositorPresentRegions(&olderRegion,1,sizeof(newerPixels),newerPixels,copy,display,g_backdropGeneration,42));
    CHECK(framebuffer[10]==201 && framebuffer[11]==202);
    dependencies.RecordAccepted(42,11,nullptr,0,true);
    CHECK(!DesktopDependenciesCovered(dependencies.Find(42,12)->dependencies,coveredWrites,2));
    CHECK(!CompositorPresentProbe(0,0,8,6,8,fullSource,copy,samples,display,g_backdropGeneration,42));
    CHECK(g_presentLease.Active(100+COMPOSITOR_PRESENT_LEASE_MS-1));
    CHECK(!g_presentLease.Active(100+COMPOSITOR_PRESENT_LEASE_MS));
    uint32_t patchPixels[]={11,12,13,14};
    CompositorPresentRegion regions[]={{2,1,2,1,2,0},{5,4,2,1,2,8}};
    auto invalidRegions=regions[1];regions[1].offset=sizeof(patchPixels);
    CHECK(CompositorPresentRegions(regions,2,sizeof(patchPixels),patchPixels,copy,display,g_backdropGeneration,42)==-22);
    CHECK(framebuffer[10]==fullSource[10] && framebuffer[37]==fullSource[37]);regions[1]=invalidRegions;
    CHECK(CompositorPresentRegions(regions,2,sizeof(patchPixels),patchPixels,copy,display,g_backdropGeneration,43)==-116);
    CHECK(!CompositorPresentRegions(regions,2,sizeof(patchPixels),patchPixels,copy,display,g_backdropGeneration,42));
    CHECK(framebuffer[10]==11 && framebuffer[11]==12 && framebuffer[37]==13 && framebuffer[38]==14);
    for(unsigned i=0;i<48;++i)if(i!=10 && i!=11 && i!=37 && i!=38)CHECK(framebuffer[i]==fullSource[i]);
    uint32_t partialCapture[48]{};g_debugFrameBuffer=VirtualAddress(reinterpret_cast<uint64_t>(partialCapture));g_debugFramePages=1;
    auto partialFrame=CompositorRequestDebugFrame(77);CaptureCompletedDebugFrame(31,{g_physFb,8,6,8});
    auto partialStatus=CompositorDebugFrameStatus(partialFrame.token,77);
    CHECK(partialStatus.state==DebugFrameState::Ready && partialStatus.source==BROOK_DEBUG_FRAME_SOURCE_LEASED &&
          partialStatus.presentationMode==BROOK_DEBUG_FRAME_PRESENT_PARTIAL && partialCapture[10]==11 && partialCapture[37]==13);
    CHECK(!CompositorPresentProbe(0,0,8,6,8,fullSource,copy,samples,display,g_backdropGeneration,42));
    CHECK(CompositorDebugFrameStatus(partialFrame.token,77).presentationMode==BROOK_DEBUG_FRAME_PRESENT_PARTIAL && partialCapture[10]==11);
    CHECK(CompositorReleaseDebugFrame(partialFrame.token,77));
    auto failSecond=[](void* context,uint64_t offset,void* dest,uint64_t bytes){
        if(offset>=8)return false;
        __builtin_memcpy(dest,static_cast<uint8_t*>(context)+offset,bytes);return true;};
    CHECK(CompositorPresentRegions(regions,2,sizeof(patchPixels),patchPixels,failSecond,display,g_backdropGeneration,42)==-5);
    CHECK(!g_presentLease.owner && g_scanoutNeedsRepair);
    CHECK(!CompositorPresentProbe(0,0,8,6,8,fullSource,copy,samples,display,g_backdropGeneration,42));
    auto expireBatch=[](void* context,uint64_t offset,void* dest,uint64_t bytes){
        __builtin_memcpy(dest,static_cast<uint8_t*>(context)+offset,bytes);g_lapicTickCount=100+COMPOSITOR_PRESENT_LEASE_MS;return true;};
    CHECK(CompositorPresentRegions(regions,2,sizeof(patchPixels),patchPixels,expireBatch,display,g_backdropGeneration,42)==-116);
    CHECK(!g_presentLease.owner && g_scanoutNeedsRepair);
    g_lapicTickCount=100;
    CHECK(!CompositorPresentProbe(0,0,8,6,8,fullSource,copy,samples,display,g_backdropGeneration,42));
    CHECK(CompositorPresentProbe(2,1,2,2,2,patchPixels,copy,samples,display,g_backdropGeneration,43,true)==-116);
    CHECK(framebuffer[10]==fullSource[10]);
    CHECK(!CompositorPresentProbe(2,1,2,2,2,patchPixels,copy,samples,display,g_backdropGeneration,42,true));
    CHECK(framebuffer[10]==11 && framebuffer[11]==12 && framebuffer[18]==13 && framebuffer[19]==14);
    CHECK(framebuffer[9]==fullSource[9] && framebuffer[20]==fullSource[20]);
    ++g_backdropGeneration;
    CHECK(CompositorPresentProbe(2,1,2,2,2,patchPixels,copy,samples,display,g_backdropGeneration,42,true)==-116);
    --g_backdropGeneration;
    g_lapicTickCount=100+COMPOSITOR_PRESENT_LEASE_MS;
    CHECK(CompositorPresentProbe(2,1,2,2,2,patchPixels,copy,samples,display,g_backdropGeneration,42,true)==-116);
    g_lapicTickCount=100;
    CHECK(!CompositorPresentProbe(0,0,8,6,8,fullSource,copy,samples,display,g_backdropGeneration,42));
    uint32_t capturedPixels[48]{};g_debugFrameBuffer=VirtualAddress(reinterpret_cast<uint64_t>(capturedPixels));g_debugFramePages=1;
    int requestError=0;
    auto window=CompositorRequestDebugFrameOptions(77,BROOK_DEBUG_FRAME_FLAG_WINDOW_BUFFER,9,90,&requestError);
    CHECK(!requestError && window.state==DebugFrameState::Ready && window.copyUs==15 &&
          window.source==BROOK_DEBUG_FRAME_SOURCE_WINDOW_BUFFER && window.plane==BROOK_DEBUG_FRAME_PLANE_UNREAD &&
          window.windowId==9 && window.windowScale==2 && capturedPixels[0]==0x223344 && capturedPixels[1]==0x667788);
    CHECK(CompositorReleaseDebugFrame(window.token,77));
    expectedFlagsAtWake=BROOK_DEBUG_FRAME_FLAG_CURSOR;
    auto frame=CompositorRequestDebugFrameOptions(77,BROOK_DEBUG_FRAME_FLAG_CURSOR,0,0,&requestError);
    expectedFlagsAtWake=-1;
    CHECK(!requestError && frame.state==DebugFrameState::Pending && g_debugFrameFlags==BROOK_DEBUG_FRAME_FLAG_CURSOR);
    g_compositorEpoch=40;g_lapicTickCount=120;
    CaptureCompletedDebugFrame(40,{g_physFb,8,6,8});
    auto captured=CompositorDebugFrameStatus(frame.token,77);
    CHECK(captured.state==DebugFrameState::Ready && captured.epoch==31 && captured.tick==100 && captured.source==BROOK_DEBUG_FRAME_SOURCE_LEASED && captured.presentationMode==BROOK_DEBUG_FRAME_PRESENT_FULL);
    for(unsigned i=0;i<48;++i)CHECK(capturedPixels[i]==fullSource[i]);
    fullSource[0]=0xaabbccdd;CHECK(!CompositorPresentProbe(0,0,8,6,8,fullSource,copy,samples,display,g_backdropGeneration,42));
    CHECK(capturedPixels[0]==0x12340000 && framebuffer[0]==0xaabbccdd);
    CHECK(CompositorReleaseDebugFrame(frame.token,77));
    CompositorReleasePresentLease(42);
    frame=CompositorRequestDebugFrame(77);CaptureCompletedDebugFrame(40,{g_physFb,8,6,8});
    captured=CompositorDebugFrameStatus(frame.token,77);
    CHECK(captured.state==DebugFrameState::Ready && captured.epoch==40 && captured.tick==120 && capturedPixels[0]==0xaabbccdd);
    CHECK(CompositorReleaseDebugFrame(frame.token,77));
    CHECK(!CompositorPresentProbe(0,0,8,6,8,fullSource,copy,samples,display,g_backdropGeneration,42));
    uint32_t software[48];for(auto& pixel:software)pixel=0xdeadbeef;
    g_backBuffer=software;g_backBufStride=8;g_dirtyMinY=0;g_dirtyMaxY=6;
    CompositorFlipSoftware();CHECK(framebuffer[47]==fullSource[47] && g_dirtyMinY==0 && g_dirtyMaxY==6);
    CHECK(CompositorPresentProbe(0,0,8,6,8,fullSource,copy,samples,display,g_backdropGeneration,43)==-16);
    CHECK(CompositorPresentProbe(0,0,7,6,8,fullSource,copy,samples,display,g_backdropGeneration,42)==-16);
    CompositorReleasePresentLease(43);CHECK(g_presentLease.owner==42);
    g_lapicTickCount=120+COMPOSITOR_PRESENT_LEASE_MS;g_needsFullRepaint=false;CompositorExpirePresentLease();
    CHECK(!g_presentLease.owner && g_needsFullRepaint);
    CompositorFlipSoftware();CHECK(framebuffer[47]==0xdeadbeef && g_dirtyMinY==0xffffffffu && !g_dirtyMaxY);
    CHECK(!CompositorPresentProbe(0,0,8,6,8,fullSource,copy,samples,display,g_backdropGeneration,42));
    g_needsFullRepaint=false;CompositorReleasePresentLease(42);CHECK(!g_presentLease.owner && g_needsFullRepaint);
    CHECK(CompositorPresentProbe(2,1,2,2,2,patchPixels,copy,samples,display,g_backdropGeneration,42,true)==-116);
    CHECK(!CompositorPresentProbe(0,0,8,6,8,fullSource,copy,samples,display,g_backdropGeneration,42));
    auto expireDuringCopy=[](void* context,uint64_t offset,void* dest,uint64_t bytes){
        __builtin_memcpy(dest,static_cast<uint8_t*>(context)+offset,bytes);g_lapicTickCount=120+2*COMPOSITOR_PRESENT_LEASE_MS;return true;};
    CHECK(CompositorPresentProbe(2,1,2,2,2,patchPixels,expireDuringCopy,samples,display,g_backdropGeneration,42,true)==-116);
    CHECK(!g_presentLease.owner && g_scanoutNeedsRepair);
    g_dirtyMinY=0;g_dirtyMaxY=6;CompositorFlipSoftware();CHECK(!g_scanoutNeedsRepair && framebuffer[10]==0xdeadbeef);
    g_backBuffer=nullptr;
    uint32_t overlayPixels[48]{};g_debugFrameBuffer=VirtualAddress(reinterpret_cast<uint64_t>(overlayPixels));g_debugFramePages=1;
    auto overlay=CompositorRequestDebugFrame(77,true);CHECK(overlay.state==DebugFrameState::Pending && overlay.token);
    CaptureOverlayAt(400,0,0,false);auto ready=CompositorDebugFrameStatus(overlay.token,77);
    CHECK(ready.state==DebugFrameState::Ready && ready.width==8 && ready.height==6);
    DesktopOverlayBands overlayBands{};
    CHECK(CompositorOverlayBands(overlay.token,78,overlayBands)==-116);
    CHECK(!CompositorOverlayBands(overlay.token,77,overlayBands));
    for(unsigned y=0;y<6;++y)for(unsigned x=0;x<8;++x){
        unsigned hits=0;
        for(unsigned i=0;i<overlayBands.count;++i){const auto& b=overlayBands.bands[i];
            if(x>=b.x && x<b.x+b.width && y>=b.y && y<b.y+b.height)++hits;}
        CHECK(hits<=1 && (!(overlayPixels[y*8+x]>>24) || hits==1));
    }
    CHECK(overlayPixels[0]==0 && overlayPixels[9]==0xff654321 && overlayPixels[47]==0xff123456);
    uint32_t pixel=0;CHECK(CompositorReadDebugFrame(overlay.token,78,0,&pixel,4)==-116);
    CHECK(CompositorReadDebugFrame(overlay.token,77,9*4,&pixel,4)==4 && pixel==0xff654321);
    CHECK(CompositorReleaseDebugFrame(overlay.token,77));
    g_cursorFrame={};g_cursorFrame.width=g_cursorFrame.height=1;g_cursorFrame.pixels[0]=0x80ff0000u;
    overlay=CompositorRequestDebugFrame(77,true);CaptureOverlayAt(401,4,2,true);
    CHECK(overlayPixels[20]==0x80ff0000u);CHECK(!CompositorOverlayBands(overlay.token,77,overlayBands));
    {unsigned hits=0;for(unsigned i=0;i<overlayBands.count;++i){const auto& b=overlayBands.bands[i];
        if(4>=b.x && 4<b.x+b.width && 2>=b.y && 2<b.y+b.height)++hits;}CHECK(hits==1);}
    CHECK(CompositorReleaseDebugFrame(overlay.token,77));
    overlay=CompositorRequestDebugFrame(77,true);CaptureOverlayAt(402,4,5,true);
    const uint32_t red=(255u*128u+0x12u*127u)/255u,green=0x34u*127u/255u,blue=0x56u*127u/255u;
    CHECK(overlayPixels[44]==(0xff000000u|(red<<16)|(green<<8)|blue));
    CHECK(CompositorReleaseDebugFrame(overlay.token,77));
    {
        // The generation excludes the pointer: a pointer-free capture of the
        // same content keeps it, and the pointer pixels are cleared.
        const uint64_t g0=CompositorOverlayGeneration();CHECK(g0>1);
        overlay=CompositorRequestDebugFrame(77,true,false);CaptureOverlayAt(403,6,1,true);
        auto status=CompositorDebugFrameStatus(overlay.token,77);
        CHECK(status.state==DebugFrameState::Ready && status.overlayGeneration==g0 && overlayPixels[1*8+6]==0);
        CHECK(overlayPixels[44]==0xff123456); // the previous pointer was cleared, not blended
        CHECK(!CompositorOverlayBands(overlay.token,77,overlayBands));
        for(unsigned i=0;i<overlayBands.count;++i){const auto& b=overlayBands.bands[i];CHECK(!(6>=b.x && 6<b.x+b.width && 1>=b.y && 1<b.y+b.height));}
        CHECK(CompositorReleaseDebugFrame(overlay.token,77));
        // Closing the launcher bumps the generation; only previously drawn
        // rectangles are cleared (a marker outside them survives).
        launcherVisible=false;overlayPixels[0]=0xdeadbeef;
        overlay=CompositorRequestDebugFrame(77,true,false);CaptureOverlayAt(404,0,0,false);
        status=CompositorDebugFrameStatus(overlay.token,77);
        CHECK(status.state==DebugFrameState::Ready && status.overlayGeneration==g0+1 && CompositorOverlayGeneration()==g0+1);
        CHECK(overlayPixels[9]==0 && overlayPixels[0]==0xdeadbeef && overlayPixels[47]==0xff123456);
        overlayPixels[0]=0;CHECK(CompositorReleaseDebugFrame(overlay.token,77));
        // Change detection: while subscribed and unleased, content changes
        // update the generation without a capture request.
        // Keys only: nothing is rasterised into the shared buffer.
        launcherVisible=true;const uint64_t now=ApicTickCount();(void)CompositorOverlayGeneration();
        CheckOverlayAt(now+1,0,0);CHECK(CompositorOverlayGeneration()==g0+2 && overlayPixels[9]==0);
        launcherVisible=false;CheckOverlayAt(now+2,0,0);
        CHECK(CompositorOverlayGeneration()==g0+3); // close without pointer or input changes
        CheckOverlayAt(now+3,1,0);CHECK(CompositorOverlayGeneration()==g0+3); // unchanged publication
        launcherVisible=true;CheckOverlayAt(now+4,1,0);CHECK(CompositorOverlayGeneration()==g0+4);
        // The volume popup takes the launcher's rectangle slot and is keyed.
        launcherVisible=false;volumeVisible=true;CheckOverlayAt(now+5,1,0);
        CHECK(CompositorOverlayGeneration()==g0+5 && overlayPixels[1*8+4]==0 && overlayPixels[9]==0);
        launcherVisible=true;volumeVisible=false;CheckOverlayAt(now+6,1,0);
        CHECK(CompositorOverlayGeneration()==g0+6 && overlayPixels[1*8+4]==0);
        // Detection never touches the buffer, so it continues under a lease.
        launcherVisible=false;overlay=CompositorRequestDebugFrame(77,true,false);
        CheckOverlayAt(now+500,2,0);CHECK(CompositorOverlayGeneration()==g0+7);
        // Op-7 wait: blocks without locks, woken by completion.
        waiterProc=static_cast<Process*>(std::calloc(1,sizeof(Process)));CHECK(waiterProc);
        DebugFrameSnapshot waited{};captureOnBlock=true;blockCalls=0;
        CHECK(!CompositorWaitDebugFrame(overlay.token,77,100,waited));
        CHECK(blockCalls==1 && waiterWakes==1 && waited.state==DebugFrameState::Ready && waited.overlayGeneration==g0+7 && !g_debugFrameWaiter);
        CHECK(!CompositorWaitDebugFrame(overlay.token,77,100,waited) && blockCalls==1); // ready: no block
        CHECK(CompositorWaitDebugFrame(overlay.token,78,100,waited)==-116);
        CHECK(CompositorReleaseDebugFrame(overlay.token,77));
        // Timeout: still pending after the deadline, waiter slot released.
        overlay=CompositorRequestDebugFrame(77,true,false);blockCalls=0;
        CHECK(!CompositorWaitDebugFrame(overlay.token,77,20,waited) && waited.state==DebugFrameState::Pending && blockCalls==1 && !g_debugFrameWaiter);
        CHECK(CompositorReleaseDebugFrame(overlay.token,77));
        std::free(waiterProc);waiterProc=nullptr;launcherVisible=true;
        CHECK(g_debugFrameCleanAddress==g_debugFrameBuffer.raw());
        // Per-surface keys: an unchanged taskbar is kept (not rasterised),
        // a changed one is redrawn while the launcher is kept.
        overlay=CompositorRequestDebugFrame(77,true,false);CaptureOverlayAt(700,9,9,false);
        CHECK(CompositorReleaseDebugFrame(overlay.token,77));
        const unsigned draws=taskbarDraws;overlayPixels[9]=0x11111111; // marker inside the launcher
        overlay=CompositorRequestDebugFrame(77,true,false);CaptureOverlayAt(701,9,9,false);
        CHECK(taskbarDraws==draws && overlayPixels[9]==0x11111111);
        DesktopOverlayBands first{};CHECK(!CompositorOverlayBands(overlay.token,77,first));
        CHECK(CompositorReleaseDebugFrame(overlay.token,77));
        ++shellKeyBump;
        overlay=CompositorRequestDebugFrame(77,true,false);CaptureOverlayAt(702,9,9,false);
        CHECK(taskbarDraws==draws+1 && overlayPixels[9]==0x11111111 && overlayPixels[47]==0xff123456);
        DesktopOverlayBands second{};CHECK(!CompositorOverlayBands(overlay.token,77,second));
        CHECK(first.count==2 && second.count==2);
        {BrookDesktopOverlaySurfaces records{};CHECK(CompositorOverlaySurfaces(overlay.token,78,records)==-116);
         CHECK(!CompositorOverlaySurfaces(overlay.token,77,records) && records.count==2 &&
               records.surfaces[0].kind==3 && records.surfaces[0].y==5 && records.surfaces[1].kind==4 && records.surfaces[1].key);}
        for(unsigned i=0;i<2;++i){const bool taskbar=first.bands[i].y==5;
            CHECK(taskbar?first.keys[i]!=second.keys[i]:first.keys[i]==second.keys[i]);}
        CHECK(CompositorReleaseDebugFrame(overlay.token,77));overlayPixels[9]=0xff654321;
        // The legacy capture can pack four shell surfaces and a cursor
        // into the same seven-band ABI. A later cursorless capture erases it.
        summaryAndHudVisible = true;
        launcherVisible = false;
        volumeVisible = true;
        overlay = CompositorRequestDebugFrame(77, true, true);
        CaptureOverlayAt(703, 6, 4, true);
        CHECK(CompositorDebugFrameStatus(overlay.token, 77).state == DebugFrameState::Ready);
        BrookDesktopOverlaySurfaces records{};
        CHECK(!CompositorOverlaySurfaces(overlay.token, 77, records) && records.count == 4);
        DesktopOverlayBands withCursor{};
        CHECK(!CompositorOverlayBands(overlay.token, 77, withCursor) && withCursor.count <= 7);
        CHECK(overlayPixels[7] == 0xff445566 && overlayPixels[27] == 0xff112233 &&
              overlayPixels[38] == g_cursorFrame.pixels[0]);
        bool cursorPacked = false;
        for (unsigned i = 0; i < withCursor.count; ++i) {
            const auto& r = withCursor.bands[i];
            cursorPacked |= r.x <= 6 && r.x + r.width > 6 && r.y <= 4 && r.y + r.height > 4;
        }
        CHECK(cursorPacked);
        CHECK(CompositorReleaseDebugFrame(overlay.token, 77));
        overlay = CompositorRequestDebugFrame(77, true, false);
        CaptureOverlayAt(704, 6, 4, true);
        CHECK(overlayPixels[38] == 0 && overlayPixels[7] == 0xff445566 && overlayPixels[27] == 0xff112233);
        CHECK(CompositorReleaseDebugFrame(overlay.token, 77));
        // Five shell surfaces (the side dock too) and a cursor: six
        // rectangles merge pairwise into the four the seven bands allow.
        dockVisible = true;
        overlay = CompositorRequestDebugFrame(77, true, true);
        CaptureOverlayAt(705, 6, 4, true);
        CHECK(CompositorDebugFrameStatus(overlay.token, 77).state == DebugFrameState::Ready);
        {
            BrookDesktopOverlaySurfaces five{};
            CHECK(!CompositorOverlaySurfaces(overlay.token, 77, five) && five.count == 5);
            bool dockRecord = false;
            for (unsigned i = 0; i < five.count; ++i) dockRecord |= five.surfaces[i].kind == uint32_t(WmShellSurfaceKind::Dock) + 3;
            CHECK(dockRecord);
            DesktopOverlayBands bands{};
            CHECK(!CompositorOverlayBands(overlay.token, 77, bands) && bands.count && bands.count <= 7);
            CHECK(overlayPixels[24] == 0xff998877 && overlayPixels[7] == 0xff445566 && overlayPixels[27] == 0xff112233);
            auto covered = [&](uint32_t x, uint32_t y) {
                for (unsigned i = 0; i < bands.count; ++i) {
                    const auto& r = bands.bands[i];
                    if (x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height) return true;
                }
                return false;
            };
            CHECK(covered(0, 3) && covered(7, 0) && covered(3, 3) && covered(6, 4) && covered(1, 1) && covered(0, 5));
        }
        CHECK(CompositorReleaseDebugFrame(overlay.token, 77));
        // Seven independent records must survive the old raster band limit.
        extraPopupsVisible = true;
        overlay = CompositorRequestDebugFrame(77, true, false);
        CaptureOverlayAt(706, 6, 4, false);
        CHECK(!CompositorOverlaySurfaces(overlay.token, 77, records) && records.count == 7);
        CHECK(overlayPixels[9] == 0xff777777 && overlayPixels[11] == 0xff334455 &&
              overlayPixels[13] == 0xff224466);
        DesktopOverlayBands multiple{};
        CHECK(!CompositorOverlayBands(overlay.token, 77, multiple) && multiple.count <= 7);
        CHECK(CompositorReleaseDebugFrame(overlay.token, 77));
        dockVisible = false;
        summaryAndHudVisible = extraPopupsVisible = volumeVisible = false;
        launcherVisible = true;
        g_debugFrameCleanAddress=0;
    }
    g_debugFramePages=0;g_debugFrameBuffer=VirtualAddress{};
    auto fail=[](void*,uint64_t,void*,uint64_t){return false;};
    g_needsFullRepaint=false;CHECK(CompositorPresentProbe(2,1,3,2,4,source,fail,samples)==-5 && g_needsFullRepaint);
    g_debugFrameBuffer=VirtualAddress(reinterpret_cast<uint64_t>(capturedPixels));g_debugFramePages=1;
    frame=CompositorRequestDebugFrame(77);CaptureCompletedDebugFrame(50,{g_physFb,8,6,8});
    CHECK(CompositorDebugFrameStatus(frame.token,77).state==DebugFrameState::Pending);
    g_backBuffer=software;g_dirtyMinY=0;g_dirtyMaxY=6;CompositorFlipSoftware();
    CaptureCompletedDebugFrame(51,{g_physFb,8,6,8});
    CHECK(CompositorDebugFrameStatus(frame.token,77).state==DebugFrameState::Ready &&
          CompositorDebugFrameStatus(frame.token,77).source==BROOK_DEBUG_FRAME_SOURCE_FRONT && capturedPixels[0]==0xdeadbeef);
    CHECK(!g_debugFrameCleanAddress); // full-image copy: next overlay capture clears fully
    CHECK(CompositorReleaseDebugFrame(frame.token,77));g_debugFramePages=0;g_debugFrameBuffer=VirtualAddress{};g_backBuffer=nullptr;
    // The manual power-off frame must reach scanout even with no renderer
    // and must never draw over a still-active writer or present lease.
    g_physFb=framebuffer;g_physFbWidth=8;g_physFbHeight=6;g_physFbStride=8;
    g_backBuffer=software;g_backBufStride=8;
    const auto flushes=displayFlushes;
    g_gopWriter=1;
    CHECK(!CompositorDrawManualPowerOff() && displayFlushes==flushes && framebuffer[0]==0xdeadbeef);
    g_gopWriter=0;
    CHECK(g_presentLease.Acquire(42,ApicTickCount(),COMPOSITOR_PRESENT_LEASE_MS));
    CHECK(!CompositorDrawManualPowerOff() && displayFlushes==flushes && framebuffer[0]==0xdeadbeef);
    g_presentLease.Clear();
    CHECK(CompositorDrawManualPowerOff() && displayFlushes==flushes+1 && ttySuppressed && screenIdleDisabled);
    for (unsigned i=0;i<48;++i) CHECK(framebuffer[i]==(i==19 ? 0xffffffu : 0u));
    g_backBuffer=nullptr;
    framebuffer[0]=0xdeadbeef;
    CHECK(CompositorDrawManualPowerOff() && displayFlushes==flushes+2 && framebuffer[0]==0 && framebuffer[19]==0xffffff);
    g_gpuComposite=g_gpuDrawReady=true;
    g_debugFrameBuffer=VirtualAddress(reinterpret_cast<uint64_t>(capturedPixels));g_debugFramePages=1;
    frame=CompositorRequestDebugFrame(77);
    CaptureCompletedDebugFrame(52,{g_physFb,8,6,8},true);
    captured=CompositorDebugFrameStatus(frame.token,77);
    CHECK(captured.state==DebugFrameState::Ready && captured.source==BROOK_DEBUG_FRAME_SOURCE_FRONT &&
          capturedPixels[0]==0 && capturedPixels[19]==0xffffff);
    CHECK(CompositorReleaseDebugFrame(frame.token,77));
    g_debugFramePages=0;g_debugFrameBuffer=VirtualAddress{};
    g_gpuComposite=g_gpuDrawReady=false;
    ttySuppressed=false;
    g_compositorProcess=nullptr;const auto finalWakes=wakeRequests.load();CompositorWake();CHECK(wakeRequests==finalWakes);
    std::free(compositor);
    {
        // Stage 1.5: backdrop blur follows the backend that shows the desktop.
        // CPU never; virgl when its driver can blur; a renderer holding the
        // present lease only if it reported the capability itself (op 18),
        // and only until the lease lapses, is released or a GOP writer runs.
        const uint64_t t=g_lapicTickCount=5000;g_presentLease.Clear();g_gopWriter=0;
        CHECK(!CompositorBackendBlur(t));
        g_gpuComposite=g_gpuDrawReady=true;ops.BlurSupported=[]{return true;};CHECK(CompositorBackendBlur(t));
        ops.BlurSupported=[]{return false;};CHECK(!CompositorBackendBlur(t));
        ops.BlurSupported=nullptr;CHECK(!CompositorBackendBlur(t));g_gpuComposite=g_gpuDrawReady=false;
        CHECK(g_presentLease.Acquire(42,t,COMPOSITOR_PRESENT_LEASE_MS));CHECK(!CompositorBackendBlur(t));
        CHECK(CompositorSetRendererCapabilities(42,0xffffffffu)==BROOK_DESKTOP_CAP_BACKDROP_BLUR);
        CHECK(CompositorBackendBlur(t));
        CHECK(CompositorSetRendererCapabilities(43,BROOK_DESKTOP_CAP_BACKDROP_BLUR)==BROOK_DESKTOP_CAP_BACKDROP_BLUR);
        CHECK(!CompositorBackendBlur(t)); // reported by a process that does not hold the lease
        CompositorSetRendererCapabilities(42,BROOK_DESKTOP_CAP_BACKDROP_BLUR);CHECK(CompositorBackendBlur(t));
        CHECK(!CompositorBackendBlur(t+COMPOSITOR_PRESENT_LEASE_MS)); // lapsed: the CPU path shows the desktop
        g_gopWriter=7;g_gopWriterOwner=0;CHECK(!CompositorBackendBlur(t));g_gopWriter=0;CHECK(CompositorBackendBlur(t));
        // The lease holder's own present in flight (held across the BCS copy
        // or flip) keeps blur: a compositor loop that runs mid-present must
        // not flip the theme to the Opaque stand-in (Dell dropouts, 19 Nov).
        // Any other writer, a revoked one or a lapsed lease still drops it.
        g_gopWriter=7;g_gopWriterOwner=42;g_gopWriterRevoked=false;CHECK(CompositorBackendBlur(t));
        CHECK(CompositorBackendBlur(t+COMPOSITOR_PRESENT_LEASE_MS-1));
        CHECK(!CompositorBackendBlur(t+COMPOSITOR_PRESENT_LEASE_MS));
        g_gopWriterRevoked=true;CHECK(!CompositorBackendBlur(t));g_gopWriterRevoked=false;
        g_gopWriterOwner=43;CHECK(!CompositorBackendBlur(t));
        CompositorSetRendererCapabilities(42,0);g_gopWriterOwner=42;CHECK(!CompositorBackendBlur(t));
        CompositorSetRendererCapabilities(42,BROOK_DESKTOP_CAP_BACKDROP_BLUR);CHECK(CompositorBackendBlur(t));
        g_gopWriter=0;g_gopWriterOwner=0;CHECK(CompositorBackendBlur(t));
        {   // Each branch names its reason (gpu glass status backend_blur_change_reasons).
            CompositorBlurReason why{};
            CHECK(CompositorBackendBlur(t,&why) && why==CompositorBlurReason::Renderer);
            CHECK(!CompositorBackendBlur(t+COMPOSITOR_PRESENT_LEASE_MS,&why) && why==CompositorBlurReason::LeaseLapsed);
            g_gopWriter=7;g_gopWriterOwner=43;CHECK(!CompositorBackendBlur(t,&why) && why==CompositorBlurReason::ForeignWriter);
            g_gopWriterOwner=42;g_gopWriterRevoked=true;CHECK(!CompositorBackendBlur(t,&why) && why==CompositorBlurReason::WriterRevoked);
            g_gopWriterRevoked=false;CHECK(!CompositorBackendBlur(t+COMPOSITOR_PRESENT_LEASE_MS,&why) && why==CompositorBlurReason::WriterUnleased);
            g_gopWriter=0;g_gopWriterOwner=0;
            CompositorSetRendererCapabilities(43,BROOK_DESKTOP_CAP_BACKDROP_BLUR);
            CHECK(!CompositorBackendBlur(t,&why) && why==CompositorBlurReason::CapsOwnerMismatch);
            CompositorSetRendererCapabilities(42,BROOK_DESKTOP_CAP_BACKDROP_BLUR);
        }
        {   // A refused-present streak: the renderer keeps reserving, so its
            // lease (and blur) survive past 500 ms, but only to the stall bound
            // after its last accepted present, and only with its frame intact.
            g_scanoutContainsLeasedFrame=true;g_scanoutNeedsRepair=false;g_leasedFrameTick=t;
            g_lapicTickCount=t+400;CHECK(CompositorRenewLiveRendererLease(42));
            CHECK(!CompositorRenewLiveRendererLease(43));
            CHECK(CompositorBackendBlur(t+COMPOSITOR_PRESENT_LEASE_MS+100)); // stale streak past the old lapse
            for(uint64_t k=t+800;k<t+COMPOSITOR_PRESENT_LEASE_STALL_MS;k+=300){g_lapicTickCount=k;CHECK(CompositorBackendBlur(k));
                (void)CompositorRenewLiveRendererLease(42);}
            g_lapicTickCount=t+COMPOSITOR_PRESENT_LEASE_STALL_MS;CHECK(!CompositorRenewLiveRendererLease(42));
            CompositorBlurReason why{};
            CHECK(!CompositorBackendBlur(t+COMPOSITOR_PRESENT_LEASE_STALL_MS,&why) && why==CompositorBlurReason::LeaseLapsed);
            // No renewal over a frame the software compositor must repair, or
            // without the renderer's own blur report.
            g_lapicTickCount=t;CHECK(g_presentLease.Acquire(42,t,COMPOSITOR_PRESENT_LEASE_MS));
            g_lapicTickCount=t+100;g_scanoutNeedsRepair=true;CHECK(!CompositorRenewLiveRendererLease(42));g_scanoutNeedsRepair=false;
            CompositorSetRendererCapabilities(42,0);CHECK(!CompositorRenewLiveRendererLease(42));
            CompositorSetRendererCapabilities(42,BROOK_DESKTOP_CAP_BACKDROP_BLUR);
            CHECK(CompositorReadBlurDiagnostics().leaseLiveRenewals>=2);
            g_scanoutContainsLeasedFrame=false;g_leasedFrameTick=0;g_lapicTickCount=t;
            g_presentLease.Clear();CHECK(g_presentLease.Acquire(42,t,COMPOSITOR_PRESENT_LEASE_MS));CHECK(CompositorBackendBlur(t));
        }
        CompositorReleasePresentLease(42); // detach or exit drops the report
        CHECK(g_presentLease.Acquire(42,t,COMPOSITOR_PRESENT_LEASE_MS));CHECK(!CompositorBackendBlur(t));
        CompositorSetRendererCapabilities(42,BROOK_DESKTOP_CAP_BACKDROP_BLUR);
        CHECK(!CompositorSetRendererCapabilities(42,0));CHECK(!CompositorBackendBlur(t)); // withdrawn
        CHECK(!CompositorSetRendererCapabilities(0,BROOK_DESKTOP_CAP_BACKDROP_BLUR));
        g_presentLease.Clear();
        // Under the resolved Glass theme (only ever for a renderer that can
        // blur) the overlay keeps material alpha on material surfaces and
        // reports them translucent; opaque otherwise.
        uint32_t pixels[48]{};DesktopOverlayRect rects[4]{};uint64_t keys[4]{};uint32_t opaque=0xffu;
        g_materialFrame=BROOK_APPEARANCE_DEFAULT;CHECK(CompositorGlassActive());++shellKeyBump;
        BrookDesktopOverlaySurfaces records{};
        uint32_t count=RenderDesktopOverlay(pixels,CaptureShellFrame(8,6,600,0,0),rects,keys,records,opaque);
        CHECK(count && !opaque && pixels[47]==(CompositorMaterialOpacity()<<24|0x123456u) && CompositorMaterialOpacity()<255);
        g_materialFrame=BROOK_APPEARANCE_DEFAULT|BROOK_APPEARANCE_PERFORMANCE;CHECK(!CompositorGlassActive());++shellKeyBump;
        count=RenderDesktopOverlay(pixels,CaptureShellFrame(8,6,601,0,0),rects,keys,records,opaque);
        CHECK(count && opaque==(1u<<count)-1u && pixels[47]==0xff123456u);
        // Service death clears its cached popup and restores a theme-colored
        // taskbar. Fullscreen hides even the fallback.
        launcherVisible = true;
        RenderDesktopOverlay(pixels,CaptureShellFrame(8,6,602,0,0),rects,keys,records,opaque);
        CHECK(pixels[9] == 0xff654321);
        shellRunning = false;
        count = RenderDesktopOverlay(pixels,CaptureShellFrame(8,6,603,0,0),rects,keys,records,opaque);
        CHECK(count == 1 && opaque == 1 && pixels[9] == 0 && pixels[47] == 0xffabc123);
        taskbarHidden = true;
        count = RenderDesktopOverlay(pixels,CaptureShellFrame(8,6,604,0,0),rects,keys,records,opaque);
        CHECK(count == 0 && opaque == 0 && !pixels[47]);
    }
    std::puts("CAPTURE_QUEUE_PASS bounded staging, SHM/reuse races, running cancellation, queued timeout, uncertain lease retention");
}
