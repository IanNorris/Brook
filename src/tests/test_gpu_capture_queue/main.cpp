#include <cstdio>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <chrono>
#include <atomic>
#include "../../kernel/src/compositor.cpp"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"CAPTURE_QUEUE line %d: %s\n",__LINE__,#x);std::abort(); } } while(0)
namespace {
std::mutex queueMutex,windowMutex,frameMutex,geometryMutex;
thread_local unsigned locks;
std::atomic<int> leases{0};
std::atomic<unsigned> wakeRequests{0};
std::atomic<bool> workerDone{false};
std::thread worker;
int mode=0,created=0,destroyed=0;
uint64_t backdropStyle=2;bool badgeEnabled=false,changeStyleDuringDraw=false;
bool allocationFails=false;
unsigned sourcePixel=0x00ff00,pixels[256]{};
brook::Process* caller;
brook::WmContentSnapshot content{true,false,1,1,0,0};
uint64_t latest=1;
brook::CaptureRequest* running;
brook::GpuCompositorOps ops{};
}
namespace brook {
uint64_t WmBackdropStyleVersion(){return backdropStyle;}
void WmDrawBuildIdSpan(uint32_t* out,uint32_t,uint32_t count,uint32_t,uint32_t,uint32_t){
    if(badgeEnabled && count)out[0]=0x00abcdef;
    if(changeStyleDuringDraw)backdropStyle+=2;
}
void SerialPrintf(const char*,...) {}
volatile uint64_t g_lapicTickCount=0;
uint64_t ApicTickCount() { return g_lapicTickCount; }
void KMutexLock(KMutex* m) { (m==&g_debugFrameLock?frameMutex:m==&g_scanoutMappingLock?geometryMutex:queueMutex).lock();++locks; }
void KMutexUnlock(KMutex* m) { CHECK(locks>0);--locks;(m==&g_debugFrameLock?frameMutex:m==&g_scanoutMappingLock?geometryMutex:queueMutex).unlock(); }
VirtualAddress VmmAllocPages(uint64_t,uint64_t,MemTag,uint16_t) { return VirtualAddress{}; }
void VmmFreePages(VirtualAddress,uint64_t) { std::abort(); }
uint32_t WmDesktopHeight(uint32_t h) { return h-1; }
void WmRenderTaskbar(uint32_t* p,uint32_t stride,uint32_t w,uint32_t h,uint64_t,int32_t,int32_t) {
    for(uint32_t x=0;x<w;++x)p[(h-1)*stride+x]=0x00123456;
}
bool WmLauncherVisible() {return true;}
void WmLauncherGetRect(uint32_t,uint32_t,int32_t* x,int32_t* y,uint32_t* w,uint32_t* h){*x=1;*y=1;*w=2;*h=2;}
void WmLauncherRender(uint32_t* p,uint32_t stride,uint32_t,uint32_t,int32_t,int32_t){for(unsigned y=1;y<3;++y)for(unsigned x=1;x<3;++x)p[y*stride+x]=0x00654321;}
bool WmBootSummaryRect(uint32_t,uint32_t,int32_t*,int32_t*,uint32_t*,uint32_t*){return false;}
void WmRenderBootSummary(uint32_t*,uint32_t,uint32_t,uint32_t){std::abort();}
extern "C" void* kmalloc(uint64_t bytes) { return allocationFails?nullptr:std::calloc(1,bytes); }
extern "C" void kfree(void* p) { std::free(p); }
void MappingBackingRef(MappingBacking*) { ++leases; }
void MappingBackingUnref(MappingBacking*) { --leases; }
void SchedulerUnblock(Process* p) { CHECK(p==g_compositorProcess);++wakeRequests; }
void DisplayFlush(uint32_t first,uint32_t last) { CHECK(first<last); }
uint64_t DisplayGetFramebufferPhys(){return 0;}
bool BootLogoActive(){return false;}
bool TtyDisplaySuppressed(){return false;}
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
    badgeEnabled=true;backdropStyle+=2;
    CHECK(!CompositorReadBackdropSpan(changedBackdrop,2,2,3,backdrop));
    CHECK(CompositorGetBackdropInfo(&changedBackdrop));
    CHECK(CompositorReadBackdropSpan(changedBackdrop,2,2,3,backdrop) && backdrop[0]==0xffabcdef);
    changeStyleDuringDraw=true;
    CHECK(!CompositorReadBackdropSpan(changedBackdrop,2,2,3,backdrop));
    changeStyleDuringDraw=false;badgeEnabled=false;
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
    auto frame=CompositorRequestDebugFrame(77);CHECK(frame.state==DebugFrameState::Pending);
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
    CaptureDesktopOverlay(400,0,0,false);auto ready=CompositorDebugFrameStatus(overlay.token,77);
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
    overlay=CompositorRequestDebugFrame(77,true);CaptureDesktopOverlay(401,4,2,true);
    CHECK(overlayPixels[20]==0x80ff0000u);CHECK(!CompositorOverlayBands(overlay.token,77,overlayBands));
    {unsigned hits=0;for(unsigned i=0;i<overlayBands.count;++i){const auto& b=overlayBands.bands[i];
        if(4>=b.x && 4<b.x+b.width && 2>=b.y && 2<b.y+b.height)++hits;}CHECK(hits==1);}
    CHECK(CompositorReleaseDebugFrame(overlay.token,77));
    overlay=CompositorRequestDebugFrame(77,true);CaptureDesktopOverlay(402,4,5,true);
    const uint32_t red=(255u*128u+0x12u*127u)/255u,green=0x34u*127u/255u,blue=0x56u*127u/255u;
    CHECK(overlayPixels[44]==(0xff000000u|(red<<16)|(green<<8)|blue));
    CHECK(CompositorReleaseDebugFrame(overlay.token,77));g_debugFramePages=0;g_debugFrameBuffer=VirtualAddress{};
    auto fail=[](void*,uint64_t,void*,uint64_t){return false;};
    g_needsFullRepaint=false;CHECK(CompositorPresentProbe(2,1,3,2,4,source,fail,samples)==-5 && g_needsFullRepaint);
    g_debugFrameBuffer=VirtualAddress(reinterpret_cast<uint64_t>(capturedPixels));g_debugFramePages=1;
    frame=CompositorRequestDebugFrame(77);CaptureCompletedDebugFrame(50,{g_physFb,8,6,8});
    CHECK(CompositorDebugFrameStatus(frame.token,77).state==DebugFrameState::Pending);
    g_backBuffer=software;g_dirtyMinY=0;g_dirtyMaxY=6;CompositorFlipSoftware();
    CaptureCompletedDebugFrame(51,{g_physFb,8,6,8});
    CHECK(CompositorDebugFrameStatus(frame.token,77).state==DebugFrameState::Ready &&
          CompositorDebugFrameStatus(frame.token,77).source==BROOK_DEBUG_FRAME_SOURCE_FRONT && capturedPixels[0]==0xdeadbeef);
    CHECK(CompositorReleaseDebugFrame(frame.token,77));g_debugFramePages=0;g_debugFrameBuffer=VirtualAddress{};g_backBuffer=nullptr;
    g_compositorProcess=nullptr;const auto finalWakes=wakeRequests.load();CompositorWake();CHECK(wakeRequests==finalWakes);
    std::free(compositor);
    std::puts("CAPTURE_QUEUE_PASS bounded staging, SHM/reuse races, running cancellation, queued timeout, uncertain lease retention");
}
