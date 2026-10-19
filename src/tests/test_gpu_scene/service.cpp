#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include <functional>
#include "../../kernel/src/compositor_scene.cpp"

#define EXPECT(x) do { if (!(x)) { std::fprintf(stderr,"SCENE_SERVICE line %d: %s\n",__LINE__,#x);std::abort(); } } while(0)
namespace {
unsigned sceneLockDepth=0;
std::map<uint64_t,uint64_t> pages;
std::map<uint32_t,std::vector<uint32_t>> textures;
uint32_t nextTexture=1,rootPixel=0,stagingPixel=0;
unsigned rootBuildCalls=0;
uint64_t windowIncarnation=1,windowGeneration=1;
bool liveWindow=true,failCopy=false,failAllocation=false,failRetire=false,failRoot=false;
bool deferWorker=false;
brook::GpuCaptureResult operationResult=brook::GpuCaptureResult::Complete;
std::function<void()> atGpu,atSleep;
brook::GpuCompositorOps fakeGpu{};
brook::Process* fakeOwner;
void Callback(std::function<void()>& callback) {
    if (!callback) return;
    auto once=std::move(callback);callback={};once();
}
uint32_t CreateTexture(uint32_t w,uint32_t h,bool) {
    EXPECT(!sceneLockDepth);
    if (failAllocation) return 0;
    const uint32_t id=nextTexture++;textures[id].resize(w*h);return id;
}
bool DestroyTexture(uint32_t id) {
    EXPECT(!sceneLockDepth && textures.count(id));
    if (failRetire) return false;
    textures.erase(id);return true;
}
brook::GpuCaptureResult Upload(uint32_t id,uint64_t source,uint32_t bytes) {
    EXPECT(!sceneLockDepth && textures.count(id) && pages.count(source));Callback(atGpu);
    if (operationResult==brook::GpuCaptureResult::Complete)
        std::memcpy(textures.at(id).data(),reinterpret_cast<void*>(source),bytes);
    return operationResult;
}
brook::GpuCaptureResult Capture(uint32_t id,int32_t producer,uint32_t resource,uint32_t format,uint32_t w,uint32_t h) {
    EXPECT(!sceneLockDepth && producer==8 && resource==99 && format==1 && textures.at(id).size()==w*h);
    Callback(atGpu);
    if (operationResult==brook::GpuCaptureResult::Complete)
        for (auto& pixel:textures.at(id)) pixel=0xff123456;
    return operationResult;
}
}
namespace brook {
volatile uint64_t g_lapicTickCount=0;
extern "C" void* kmalloc(uint64_t bytes) { return std::malloc(bytes); }
extern "C" void kfree(void* ptr) { std::free(ptr); }
void KMutexLock(KMutex*) { EXPECT(sceneLockDepth++==0); }
void KMutexUnlock(KMutex*) { EXPECT(sceneLockDepth--==1); }
extern "C" void CompositorWake() {}
extern "C" void SchedulerSleepMs(uint32_t ms) {
    EXPECT(!sceneLockDepth);g_lapicTickCount+=ms;Callback(atSleep);
    if (!deferWorker) CompositorDrainScenes();
    deferWorker=false;
}
VirtualAddress VmmAllocPages(uint64_t count,uint64_t,MemTag,uint16_t) {
    EXPECT(!sceneLockDepth);
    auto* p=std::aligned_alloc(4096,count*4096);EXPECT(p);
    pages[reinterpret_cast<uint64_t>(p)]=count;return VirtualAddress(reinterpret_cast<uint64_t>(p));
}
void VmmFreePages(VirtualAddress address,uint64_t count) {
    EXPECT(!sceneLockDepth && pages.at(address.raw())==count);
    pages.erase(address.raw());std::free(reinterpret_cast<void*>(address.raw()));
}
uint64_t CopyFromUserBytes(void* destination,uint64_t source,uint64_t bytes) {
    EXPECT(!sceneLockDepth);
    if (failCopy) return 0;
    std::memcpy(destination,reinterpret_cast<void*>(source),bytes);return bytes;
}
void MappingBackingRef(MappingBacking* backing) { EXPECT(backing && backing->references);++backing->references; }
void MappingBackingUnref(MappingBacking* backing) {
    EXPECT(!sceneLockDepth && backing && backing->references);
    if (!--backing->references && backing->release) backing->release(backing->object);
}
const GpuCompositorOps* GpuCompositorGet() { return &fakeGpu; }
bool CompositorGpuActive() { return true; }
bool WmGetCaptureBinding(Process* caller,uint32_t window,WmCaptureToken* out) {
    EXPECT(!sceneLockDepth);
    if (caller!=fakeOwner || window!=1 || !liveWindow) return false;
    *out={1,1,1,windowIncarnation,windowGeneration};return true;
}
WmContentSnapshot WmSnapshotContent(uint32_t slot) {
    EXPECT(sceneLockDepth==1);
    return {liveWindow && slot==0,true,windowIncarnation,windowGeneration,4,4};
}
bool WmReserveBoundCapture(const WmCaptureToken& binding,uint32_t w,uint32_t h,WmCaptureToken* out) {
    EXPECT(sceneLockDepth==1);
    if (!liveWindow || binding.incarnation!=windowIncarnation) return false;
    *out={1,w,h,windowIncarnation,++windowGeneration};return true;
}
GpuCaptureResult CompositorBuildSceneRoot(const WmCaptureToken& token,const GpuSceneLayer* layers,uint32_t count) {
    EXPECT(!sceneLockDepth);++rootBuildCalls;const bool uncertain=failRoot;
    if (token.incarnation!=windowIncarnation || token.generation!=windowGeneration) return GpuCaptureResult::Rejected;
    stagingPixel=count?textures.at(layers[0].texture).at(0):0;
    Callback(atGpu);return uncertain?GpuCaptureResult::Uncertain:GpuCaptureResult::Complete;
}
bool CompositorPublishSceneRoot(const WmCaptureToken& token,SceneRetiredRoot& retired) {
    EXPECT(sceneLockDepth==1);retired={};
    if (!liveWindow || token.incarnation!=windowIncarnation || token.generation!=windowGeneration) return false;
    rootPixel=stagingPixel;return true;
}
void CompositorRetireSceneRoot(const SceneRetiredRoot&) { EXPECT(!sceneLockDepth); }
}

namespace {
brook::SceneHandle* OpenScene() {
    int error=0;auto* handle=brook::SceneCreate(fakeOwner,1,&error);
    EXPECT(handle && !error && brook::SceneActivate(handle));return handle;
}
void Drain() { for (unsigned i=0;i<20;++i) brook::CompositorDrainScenes(); }
int64_t UploadPixels(brook::SceneHandle* scene,uint32_t color) {
    // Strided source: the snapshot must pack only the four image pixels/row.
    uint32_t pixels[24];for (auto& p:pixels) p=color;
    const brook::SceneUploadDesc desc{reinterpret_cast<uint64_t>(pixels),sizeof(pixels),4,4,24,1};
    return brook::SceneUpload(fakeOwner,scene,desc);
}
int64_t Present(brook::SceneHandle* scene,uint64_t version) {
    const brook::SceneLayerDesc layer{version,0,0,4,4,0,0,4,4,255,0,0,4,4,0};
    const brook::ScenePresentDesc desc{0,1,4,4,0};
    return brook::ScenePresent(fakeOwner,scene,desc,&layer);
}
void CheckedEmpty() {
    Drain();EXPECT(!brook::g_scenes && !brook::g_jobs && !brook::g_sceneCount);
    EXPECT(!brook::g_snapshotBytes && !brook::g_head && !brook::g_tail && !brook::g_quarantine);
    EXPECT(pages.empty() && textures.empty() && !sceneLockDepth);
}
// Test fixture destruction only, after asserting quarantine remains retained.
// Represents host/device destruction; no production reset support is claimed.
void DropDeviceFixture() {
    using namespace brook;
    while (g_quarantine) { auto* held=g_quarantine;g_quarantine=held->next;JobUnref(held); }
    while (g_scenes) { auto* held=g_scenes;g_scenes=held->next;held->~Scene();kfree(held); }
    g_sceneCount=0;textures.clear();EXPECT(pages.empty() && !g_jobs && !g_snapshotBytes);
}
void SharedQuarantine(unsigned mode) {
    using namespace brook;
    auto* handle=OpenScene();const auto a=UploadPixels(handle,8),b=UploadPixels(handle,9);
    EXPECT(a>0 && b>a);SceneJob* queued=nullptr;
    const auto calls=rootBuildCalls;
    failRoot=true;
    atGpu=[&] {
        queued=NewJob(handle->scene,JobKind::Present);EXPECT(queued);queued->count=2;
        queued->versions[0]=a;queued->versions[1]=b;
        KMutexLock(&g_sceneLock);EXPECT(!Admit(queued,0));
        EXPECT(handle->scene->images.Pin(queued->versions,2));
        for (unsigned i=0;i<2;++i) {
            SceneImageInfo image{};EXPECT(handle->scene->images.GetPinned(queued->versions[i],image));
            queued->layers[i]={image.texture,0,0,4,4,0,0,4,4,255,0,0,4,4};
        }
        EXPECT(WmReserveBoundCapture(handle->scene->binding,4,4,&queued->token));KMutexUnlock(&g_sceneLock);
        if (mode!=2) { EXPECT(!Enqueue(queued));queued->canceled=mode==1; }
    };
    EXPECT(Present(handle,a)==-5);failRoot=false;
    if (mode==2) {
        SceneHandleUnref(handle);handle=nullptr;
        EXPECT(Enqueue(queued)==-5); // close during preparation; mixed unpin fails
    }
    EXPECT(queued->state==JobState::Done && queued->result==-5);
    EXPECT(rootBuildCalls==calls+1); // never draw again with quarantined A
    EXPECT(g_jobs==2 && g_scenes->images.Stats().quarantined==2);
    EXPECT(g_scenes->images.Stats().pinned==3); // A twice, B once
    JobUnref(queued); // discard creator reference; quarantine owns the worker ref
    if (handle) SceneHandleUnref(handle);
    Drain();EXPECT(g_jobs==2 && textures.size()==2);DropDeviceFixture();CheckedEmpty();
}
}
void TestSceneService() {
    using namespace brook;
    fakeOwner=static_cast<Process*>(std::calloc(1,sizeof(Process)));EXPECT(fakeOwner);
    fakeGpu.CreateOwnedTexture=CreateTexture;fakeGpu.DestroyOwnedTexture=DestroyTexture;
    fakeGpu.UploadOwnedTexture=Upload;fakeGpu.CaptureExternal=Capture;
    fakeGpu.ComposeOwnedLayers=[](uint32_t,const GpuSceneLayer*,uint32_t) { return GpuCaptureResult::Complete; };
    fakeGpu.DrawPremultipliedQuad=[](uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t) {};
    auto* scene=OpenScene();SceneStatsDesc stats{};SceneGetStats(scene,stats);
    EXPECT(stats.abi==1 && stats.sceneCount==1 && stats.versionSlots==64);
    const auto a=UploadPixels(scene,0xffaabbcc);EXPECT(a>0 && pages.empty());
    const auto b=UploadPixels(scene,0xff112233);EXPECT(b>a && pages.empty());
    failCopy=true;EXPECT(UploadPixels(scene,0)==-14);failCopy=false;
    EXPECT(Present(scene,a)==0 && rootPixel==0xffaabbcc);
    EXPECT(Present(scene,b)==0 && rootPixel==0xff112233);
    EXPECT(SceneRelease(scene,a)==0 && SceneRelease(scene,b)==0);
    EXPECT(Present(scene,a)==-22);
    const auto allocations=nextTexture;
    const auto c=UploadPixels(scene,0xff778899);EXPECT(c>b && nextTexture==allocations);
    EXPECT(Present(scene,c)==0 && rootPixel==0xff778899);
    SceneHandleRef(scene);SceneHandleUnref(scene); // alias closure leaves original valid
    EXPECT(SceneRelease(scene,c)==0);SceneHandleUnref(scene);CheckedEmpty();
    EXPECT(rootPixel==0xff778899); // completed root belongs to the window

    scene=OpenScene();auto* admittedScene=scene->scene;
    KMutexLock(&g_sceneLock);EXPECT(BeginPublication(admittedScene));KMutexUnlock(&g_sceneLock);
    SceneHandleUnref(scene); // publication linearized first; close must remain pending
    EXPECT(__atomic_load_n(&admittedScene->lifecycle,__ATOMIC_ACQUIRE)==3);
    KMutexLock(&g_sceneLock);EndPublication(admittedScene);KMutexUnlock(&g_sceneLock);
    EXPECT(admittedScene->closing);CheckedEmpty();

    // Queued capture keeps the canonical source lease independently of caller.
    scene=OpenScene();unsigned released=0;
    MappingBacking backing{1,&released,[](void* value) { ++*static_cast<unsigned*>(value); },nullptr};
    const SceneCaptureDesc captureDesc{-1,1,4,4,1,0};
    atSleep=[&] { MappingBackingUnref(&backing);EXPECT(backing.references==1 && !released); };
    const auto captured=SceneCapture(fakeOwner,scene,captureDesc,&backing,8,99,1);
    EXPECT(captured>0 && released==1 && !backing.references);
    EXPECT(Present(scene,captured)==0 && rootPixel==0xff123456);
    SceneHandleUnref(scene);CheckedEmpty();

    scene=OpenScene();fakeOwner->terminalPending=true;
    EXPECT(UploadPixels(scene,1)==-110);fakeOwner->terminalPending=false;
    EXPECT(pages.empty());SceneHandleUnref(scene);CheckedEmpty();
    scene=OpenScene();atSleep=[&] { g_lapicTickCount+=6000;deferWorker=true; };
    EXPECT(UploadPixels(scene,1)==-110 && pages.empty());SceneHandleUnref(scene);CheckedEmpty();

    // The source user memory can be overwritten after the job owns its snapshot.
    scene=OpenScene();uint32_t source[16];for (auto& p:source) p=0xffabcdef;
    const SceneUploadDesc snapshotDesc{reinterpret_cast<uint64_t>(source),sizeof(source),4,4,16,1};
    atSleep=[&] { std::memset(source,0,sizeof(source)); };
    const auto snapshotVersion=SceneUpload(fakeOwner,scene,snapshotDesc);EXPECT(snapshotVersion>0);
    atSleep=[&] { EXPECT(SceneRelease(scene,snapshotVersion)==0); };
    EXPECT(Present(scene,snapshotVersion)==0 && rootPixel==0xffabcdef);
    const auto cached=UploadPixels(scene,0xff999999);EXPECT(cached>0);
    const auto completedRoot=rootPixel;atGpu=[&] { ++windowGeneration; }; // SHM/new-content publication
    EXPECT(Present(scene,cached)==-116 && rootPixel==completedRoot);
    SceneHandleUnref(scene);CheckedEmpty();

    // Close while queued, while reading a snapshot, and while composing a root.
    scene=OpenScene();atSleep=[&] { SceneHandleUnref(scene); };
    EXPECT(UploadPixels(scene,1)==-125);CheckedEmpty();
    scene=OpenScene();atGpu=[&] { SceneHandleUnref(scene); };
    EXPECT(UploadPixels(scene,2)==-125);CheckedEmpty();
    scene=OpenScene();const auto image=UploadPixels(scene,3);EXPECT(image>0);
    const auto previousRoot=rootPixel;atGpu=[&] { SceneHandleUnref(scene); };
    EXPECT(Present(scene,image)==-116 && rootPixel==previousRoot);CheckedEmpty();

    // A replacement scene and a reused WM slot both reject old publication.
    scene=OpenScene();const auto oldImage=UploadPixels(scene,4);
    auto* replacement=OpenScene();EXPECT(Present(scene,oldImage)==-116);
    SceneHandleUnref(scene);Drain();const auto newImage=UploadPixels(replacement,5);
    EXPECT(Present(replacement,newImage)==0 && rootPixel==5);
    ++windowIncarnation;EXPECT(Present(replacement,newImage)==-116);
    SceneHandleUnref(replacement);CheckedEmpty();

    // No target capacity: preserve existing images and free the unused snapshot.
    scene=OpenScene();failAllocation=true;EXPECT(UploadPixels(scene,6)==-12);failAllocation=false;
    EXPECT(pages.empty());SceneGetStats(scene,stats);EXPECT(!stats.pixelBytes && !stats.snapshotBytes);
    SceneHandleUnref(scene);CheckedEmpty();

    // An uncertain upload retains source pages, target, scene and queue budget
    // even after close. Fixture teardown below models host/device destruction;
    // it is deliberately NOT a production recovery path.
    scene=OpenScene();operationResult=GpuCaptureResult::Uncertain;
    EXPECT(UploadPixels(scene,7)==-5);operationResult=GpuCaptureResult::Complete;
    SceneGetStats(scene,stats);EXPECT(stats.quarantined==1 && stats.snapshotBytes==4096 && stats.queued==1);
    SceneHandleUnref(scene);Drain();EXPECT(pages.size()==1 && textures.size()==1 && g_quarantine && g_scenes);
    DropDeviceFixture();CheckedEmpty();
    SharedQuarantine(0);SharedQuarantine(1);SharedQuarantine(2);
    std::free(fakeOwner);fakeOwner=nullptr;
    std::puts("SCENE_SERVICE_PASS real queue/ownership transitions with fake GPU: packing, source leases, close, replacement, reuse, capacity, uncertainty");
}
