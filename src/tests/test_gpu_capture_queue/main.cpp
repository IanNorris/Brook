#include <cstdio>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <chrono>
#include <atomic>
#include "../../kernel/src/compositor.cpp"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"CAPTURE_QUEUE line %d: %s\n",__LINE__,#x);std::abort(); } } while(0)
namespace {
std::mutex queueMutex,windowMutex;
thread_local unsigned locks;
std::atomic<int> leases{0};
std::atomic<unsigned> wakeRequests{0};
std::atomic<bool> workerDone{false};
std::thread worker;
int mode=0,created=0,destroyed=0;
bool allocationFails=false;
unsigned sourcePixel=0x00ff00,pixels[256]{};
brook::Process* caller;
brook::WmContentSnapshot content{true,false,1,1,0,0};
uint64_t latest=1;
brook::CaptureRequest* running;
brook::GpuCompositorOps ops{};
}
namespace brook {
volatile uint64_t g_lapicTickCount=0;
uint64_t ApicTickCount() { return g_lapicTickCount; }
void KMutexLock(KMutex*) { queueMutex.lock();++locks; }
void KMutexUnlock(KMutex*) { CHECK(locks==1);--locks;queueMutex.unlock(); }
extern "C" void* kmalloc(uint64_t bytes) { return allocationFails?nullptr:std::calloc(1,bytes); }
extern "C" void kfree(void* p) { std::free(p); }
void MappingBackingRef(MappingBacking*) { ++leases; }
void MappingBackingUnref(MappingBacking*) { --leases; }
void SchedulerUnblock(Process* p) { CHECK(p==g_compositorProcess);++wakeRequests; }
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
    g_compositorProcess=nullptr;const auto finalWakes=wakeRequests.load();CompositorWake();CHECK(wakeRequests==finalWakes);
    std::free(compositor);
    std::puts("CAPTURE_QUEUE_PASS bounded staging, SHM/reuse races, running cancellation, queued timeout, uncertain lease retention");
}
