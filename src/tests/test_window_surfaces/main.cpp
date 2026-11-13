#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include "../../kernel/src/window.cpp"
#include "../../kernel/src/desktop_layers_export.h"
#include "../../kernel/src/desktop_chrome_binding.h"
#include "../../kernel/src/desktop_frame_scenes.h"

// Exercise the production slot lifecycle and surface publication entry points.
// No renderer or VFB allocation runs here; their actual GPU behavior is a VM gate.
namespace {
unsigned lockDepth=0, wakes=0, dirty=0;
uint16_t caretPid=0;uint64_t caretIncarnation=0,testTick=0;
brook::Window* mutateDuringCopy=nullptr;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"WINDOW_SURFACES line %d: %s\n",__LINE__,#x); std::abort(); } } while (0)
}
namespace brook {
static uint8_t badgeAtlas[]={255,64,128,255};
static GlyphInfo badgeGlyphs[95]{};
extern const FontAtlas g_desktopFont100{badgeAtlas,2,2,badgeGlyphs,95,32,2,0,4};
extern const FontAtlas g_desktopFont150{badgeAtlas,2,2,badgeGlyphs,95,32,2,0,4};
extern const FontAtlas g_desktopFont200{badgeAtlas,2,2,badgeGlyphs,95,32,2,0,4};
const char* BuildGitHash(){return "test";}
bool CompositorGlassActive() { return false; }
uint32_t CompositorMaterialOpacity() { return 255; }
Terminal* TerminalFindByProcess(Process*) { return nullptr; }
bool TerminalSnapshotCaret(uint16_t pid,uint64_t incarnation,TerminalCaret* out) {
    if(!caretPid || pid!=caretPid || incarnation!=caretIncarnation)return false;
    *out={5,1,3,2};return true;
}
uint64_t ApicTickCount(){return testTick;}
extern "C" void SerialPuts(const char*) {}
extern "C" void SerialPrintf(const char*,...) {}
void KMutexLock(KMutex*) { CHECK(lockDepth++ < 2); }
void KMutexUnlock(KMutex*) { CHECK(lockDepth-- > 0); }
extern "C" void CompositorWake() { ++wakes; }
void CompositorMarkDirty() { ++dirty; }
extern "C" void MouseSetPosition(int32_t,int32_t) {}
extern "C" uint8_t MouseGetButtons() { return 0; }
uint64_t InputLatestSequence() { return 0; }
Process* ProcessCurrent() { std::abort(); }
int64_t UserMappingRemoveBacking(Process*,AddressSpaceState*,MappingBacking*) { std::abort(); }
void MappingBackingRef(MappingBacking* backing) { CHECK(backing && lockDepth>=1 && lockDepth<=2);++backing->references; }
void MappingBackingUnref(MappingBacking* backing) { CHECK(backing && lockDepth==0 && backing->references>1);--backing->references; }
bool UserMappingBufferRead(MappingBacking* backing,uint64_t offset,void* out,uint64_t bytes) {
    CHECK(!lockDepth && backing && backing->object && offset+bytes<=8192);
    __builtin_memcpy(out,static_cast<unsigned char*>(backing->object)+offset,bytes);
    if(mutateDuringCopy){++mutateDuringCopy->publishedGeneration;mutateDuringCopy=nullptr;}
    return true;
}
void AddressSpaceUnref(AddressSpaceState*) { std::abort(); }
void CompositorRetireVfb(CompositorVfbAllocation*) { std::abort(); }
}
extern "C" void KernelPanic(const char*,...) { std::abort(); }

int main() {
    using namespace brook;
    // Independent full-frame rendering is the oracle for translated spans,
    // including glyph antialiasing and cuts through both sides of the badge.
    for(auto& glyph:badgeGlyphs)glyph={0,0,2,2,0,2,3};
    const uint64_t initialStyle=WmBackdropStyleVersion();
    SetWmActiveLocked(true);CHECK(WmBackdropStyleVersion()==initialStyle+2);
    constexpr uint32_t badgeW=320,badgeH=240;
    uint32_t full[badgeW*badgeH],spans[badgeW*badgeH];
    for(const auto* metrics:{&DesktopMetric100,&DesktopMetric150,&DesktopMetric200}){
        g_desktopMetrics=metrics;
        for(unsigned i=0;i<badgeW*badgeH;++i)full[i]=spans[i]=0xff123456;
        WmDrawBuildId(full,badgeW,badgeW,badgeH);
        bool changed=false;for(auto pixel:full)if(pixel!=0xff123456){changed=true;break;}
        CHECK(changed);
        for(uint32_t y=0;y<badgeH;++y)for(uint32_t x=0;x<badgeW;){
            uint32_t n=badgeW-x<17?badgeW-x:17;
            WmDrawBuildIdSpan(spans+y*badgeW+x,x,n,y,badgeW,badgeH);x+=n;
        }
        CHECK(!__builtin_memcmp(full,spans,sizeof(full)));
    }
    g_desktopMetrics=&DesktopMetric100;
    BeginBackdropStyleChange();CHECK(WmBackdropStyleVersion()&1);EndBackdropStyleChange();
    const auto beforeInactive=WmBackdropStyleVersion();
    SetWmActiveLocked(false);CHECK(WmBackdropStyleVersion()==beforeInactive+2);
    SetWmActiveLocked(false);CHECK(WmBackdropStyleVersion()==beforeInactive+2);
    WmDesktopLayer caretLayer{};caretLayer.width=6;caretLayer.height=4;caretLayer.stride=8;
    caretLayer.caretX=5;caretLayer.caretY=1;caretLayer.caretWidth=3;caretLayer.caretHeight=2;
    uint32_t caretPixels[32];for(auto& pixel:caretPixels)pixel=0xff123456;
    // Split through a row to exercise page-chunk boundaries and padded stride.
    DesktopInvertCaret(caretLayer,0,caretPixels,13*4);
    DesktopInvertCaret(caretLayer,13*4,caretPixels+13,19*4);
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<8;++x)
        CHECK(caretPixels[y*8+x]==((x==5 && (y==1 || y==2))?0xffedcba9u:0xff123456u));
    auto* owner=static_cast<Process*>(std::calloc(1,sizeof(Process)));
    auto* stranger=static_cast<Process*>(std::calloc(1,sizeof(Process)));
    CHECK(owner && stranger);owner->pid=owner->tgid=7;owner->incarnation=77;stranger->pid=stranger->tgid=8;stranger->incarnation=88;
    // Poison a free slot as it was after a GL client died. Creation must clear
    // it independently of destruction, including all external dimensions.
    g_windows[0].externalCaptured=true;g_windows[0].externalW=1280;g_windows[0].externalH=720;
    int slot=WmCreateWindow(owner,0,0,640,480,"GL",1,false);
    CHECK(slot==0);auto& window=g_windows[slot];
    CHECK(!window.externalCaptured && !window.externalW && !window.externalH);
    const uint32_t id=window.wmId;
    BrookWindowState geometry{};
    CHECK(WmSnapshotOwnedWindowState(owner,id,0,&geometry)==0);
    CHECK(geometry.version==BROOK_WINDOW_STATE_VERSION && geometry.window==id);
    const auto originalIncarnation=geometry.incarnation;
    CHECK(originalIncarnation && originalIncarnation==window.creationIdentity);
    CHECK(WmGpuSourceProviderMatches(id,originalIncarnation,7,77));
    CHECK(!WmGpuSourceProviderMatches(id,originalIncarnation,8,88));
    CHECK(!WmGpuSourceProviderMatches(id,originalIncarnation,7,78));
    CHECK(!WmGpuSourceProviderMatches(id,0,7,77));
    CHECK(geometry.client_x==window.clientX() && geometry.client_y==window.clientY());
    CHECK(geometry.width==640 && geometry.height==480 && !geometry.reserved);
    CHECK(geometry.flags==BROOK_WINDOW_STATE_VISIBLE);
    CHECK(WmPlaceOwnedWindow(owner,id,originalIncarnation,100,120)==0);
    CHECK(window.clientX()==100 && window.clientY()==120 && window.clientW==640 && window.clientH==480);
    CHECK(WmPlaceOwnedWindow(stranger,id,originalIncarnation,1,1)==-2);
    CHECK(WmPlaceOwnedWindow(owner,id,originalIncarnation+1,1,1)==-116);
    CHECK(WmPlaceOwnedWindow(owner,id,0,1,1)==-22);
    CHECK(WmPlaceOwnedWindow(owner,id,originalIncarnation,-32768,-32768)==-22);
    CHECK(window.clientX()==100 && window.clientY()==120);
    window.x=0;window.y=0;wakes=dirty=0;
    const auto unchanged=geometry;
    CHECK(WmSnapshotOwnedWindowState(stranger,id,0,&geometry)==-2);
    CHECK(WmSnapshotOwnedWindowState(owner,0,0,&geometry)==-2);
    CHECK(WmSnapshotOwnedWindowState(owner,WM_MAX_WINDOWS+1,0,&geometry)==-2);
    CHECK(WmSnapshotOwnedWindowState(nullptr,id,0,&geometry)==-22);
    CHECK(WmSnapshotOwnedWindowState(owner,id,0,nullptr)==-22);
    CHECK(!__builtin_memcmp(&geometry,&unchanged,sizeof(geometry)) && !lockDepth);
    // Client coordinates must include scaled server decorations, but disappear
    // for client-side chrome. Negative desktop coordinates stay signed.
    window.x=-30;window.y=-20;window.metrics=&DesktopMetric200;
    CHECK(WmSnapshotOwnedWindowState(owner,id,originalIncarnation,&geometry)==0);
    CHECK(geometry.client_x==-26 && geometry.client_y==56);
    window.noChrome=true;window.focused=true;window.minimized=true;
    window.visible=false;window.focusable=true;window.state=WindowState::Maximized;
    CHECK(WmSnapshotOwnedWindowState(owner,id,originalIncarnation,&geometry)==0);
    CHECK(geometry.client_x==-30 && geometry.client_y==-20);
    CHECK(geometry.flags==(BROOK_WINDOW_STATE_CLIENT_CHROME|BROOK_WINDOW_STATE_FOCUSED|
        BROOK_WINDOW_STATE_MINIMIZED|BROOK_WINDOW_STATE_FOCUSABLE|BROOK_WINDOW_STATE_MAXIMIZED));
    window.x=0;window.y=0;window.metrics=nullptr;window.noChrome=false;
    window.focused=false;window.minimized=false;window.visible=true;
    window.focusable=false;window.state=WindowState::Normal;
    WmCaptureToken binding{};
    const auto bindingGeneration=window.publicationGeneration;
    CHECK(WmGetCaptureBinding(owner,id,&binding));
    CHECK(binding.incarnation==window.creationIdentity && window.publicationGeneration==bindingGeneration);
    CHECK(!WmGetCaptureBinding(stranger,id,&binding));
    WmCaptureToken first{};
    CHECK(WmReserveCapture(owner,id,1280,720,&first));
    CHECK(!window.externalCaptured && !window.vfbDirty);
    CHECK(WmCaptureCurrent(first) && WmCompleteCapture(first));
    CHECK(window.externalCaptured && window.externalW==1280 && window.externalH==720);
    CHECK(WmSnapshotContent(slot).generation==first.generation);
    CHECK(window.vfbDirty && wakes==1 && !lockDepth);
    WmCaptureToken rejected{};CHECK(!WmReserveCapture(stranger,id,1,1,&rejected));
    WmSignalDirtyById(stranger,id);
    CHECK(window.externalCaptured && wakes==1);
    // A live surface may switch from a GL dmabuf to a CPU-written SHM buffer.
    WmSignalDirtyById(owner,id);
    CHECK(!window.externalCaptured && !window.externalW && !window.externalH);
    CHECK(window.vfbDirty && wakes==2 && !lockDepth);
    CHECK(!WmCaptureCurrent(first) && !WmCompleteCapture(first));
    WmCaptureToken a{},b{};
    CHECK(WmReserveCapture(owner,id,800,600,&a));
    CHECK(WmReserveCapture(owner,id,800,600,&b));
    CHECK(!WmCaptureCurrent(a) && !WmCompleteCapture(a));
    CHECK(WmCompleteCapture(b));
    CHECK(window.publishedGeneration==b.generation);
    WmCaptureToken later{};CHECK(WmReserveCapture(owner,id,800,600,&later));
    CHECK(window.publishedGeneration==b.generation && window.externalCaptured);
    WmSignalDirtyById(owner,id);
    auto uploaded=WmSnapshotContent(slot);
    WmSignalDirtyById(owner,id);
    WmAcknowledgeContent(slot,uploaded);CHECK(window.vfbDirty);
    WmAcknowledgeContent(slot,WmSnapshotContent(slot));CHECK(!window.vfbDirty);
    CHECK(!WmCaptureCurrent(later) && !WmCompleteCapture(later));
    WmDestroyWindowById(stranger,id);CHECK(window.proc==owner);
    WmDestroyWindowById(owner,id);
    CHECK(!window.proc && !window.externalCaptured && !window.externalW && !window.externalH);
    CHECK(!WmCaptureCurrent(b) && !WmCompleteCapture(b));
    CHECK(WmCreateWindow(stranger,10,10,640,480,"Chromium SHM",1,false)==slot);
    const auto beforeStale=geometry;
    CHECK(WmSnapshotOwnedWindowState(stranger,id,originalIncarnation,&geometry)==-116);
    CHECK(!__builtin_memcmp(&geometry,&beforeStale,sizeof(geometry)));
    CHECK(WmSnapshotOwnedWindowState(owner,id,0,&geometry)==-2);
    CHECK(WmSnapshotOwnedWindowState(stranger,id,0,&geometry)==0);
    CHECK(geometry.incarnation!=originalIncarnation && geometry.client_x==12 && geometry.client_y==48);
    CHECK(!WmGpuSourceProviderMatches(id,originalIncarnation,7,77));
    CHECK(!WmGpuSourceProviderMatches(id,originalIncarnation,8,88));
    CHECK(WmGpuSourceProviderMatches(id,geometry.incarnation,8,88));
    CHECK(!window.externalCaptured && !window.externalW && !window.externalH);
    CHECK(!WmReserveCapture(owner,id,640,480,&rejected));
    CHECK(!WmReserveBoundCapture(binding,640,480,&rejected));
    CHECK(!WmCompleteCapture(b));
    WmSignalDirtyById(stranger,id);CHECK(window.vfbDirty);
    CHECK(!lockDepth);
    // The native maximized state crosses the provider queue atomically with
    // its dimensions; restore must clear it without changing the event ABI.
    Window::WmInputEvent discarded[256]{},event{};
    WmInputPop(&window,discarded,256);
    window.state=WindowState::Maximized;
    CHECK(WmPushWmEvent(&window,WM_EVT_RESIZED,1280,720));
    CHECK(WmInputPop(&window,&event,1)==1 && event.reserved==WM_RESIZE_MAXIMIZED);
    CHECK(event.type==WM_EVT_RESIZED && event.x==1280 && event.y==720);
    window.state=WindowState::Normal;
    CHECK(WmPushWmEvent(&window,WM_EVT_RESIZED,640,360));
    CHECK(WmInputPop(&window,&event,1)==1 && event.reserved==WM_RESIZE_RESIZING);
    // Quiet windows have no client dirty pixels. Focus/z-order changes still
    // require a new chrome composition, without waiting for periodic repaint.
    WmAcknowledgeContent(slot,WmSnapshotContent(slot));
    window.focusable=true;
    WmSetFocus(slot);
    const int other=WmCreateWindow(owner,20,20,320,240,"Quiet",1,true);
    CHECK(other>=0 && other!=slot && g_focusedIdx==other);
    auto previousDirty=dirty,previousWake=wakes;
    WmSetFocus(slot);
    CHECK(window.focused && !g_windows[other].focused && g_focusedIdx==slot);
    CHECK(window.zOrder>g_windows[other].zOrder && dirty==previousDirty+1 && wakes==previousWake+1);
    CHECK(!window.vfbDirty && !g_windows[other].vfbDirty);
    WmToggleFocusedWindowGlass();
    CHECK(window.opacity==WM_GLASS_OPACITY && window.blurRadius==WM_GLASS_BLUR && g_windows[other].blurRadius==0);
    WmToggleFocusedWindowGlass();
    CHECK(window.opacity==255 && window.blurRadius==0 && !lockDepth);
    previousDirty=dirty;previousWake=wakes;
    WmSetFocus(slot);WmSetFocus(-1);
    CHECK(dirty==previousDirty && wakes==previousWake);
    WmSendToBack(slot);
    CHECK(window.zOrder<g_windows[other].zOrder && dirty==previousDirty+1 && wakes==previousWake+1);
    previousDirty=dirty;previousWake=wakes;
    WmSetTitleById(stranger,window.wmId,"Updated quiet title");
    CHECK(dirty==previousDirty+1 && wakes==previousWake+1);
    previousDirty=dirty;previousWake=wakes;
    WmSetTitleById(owner,window.wmId,"Wrong owner");WmSendToBack(-1);
    CHECK(dirty==previousDirty && wakes==previousWake && !lockDepth);
    // The future renderer gets a z-ordered lease on each client backing,
    // rather than a stale kernel virtual address. Unsupported windows reject
    // the entire capture, and a later publication or geometry change makes
    // the snapshot stale without releasing its page pins.
    unsigned char inputA[8192]{},inputB[8192]{},outputA[8192]{},outputB[8192]{};
    inputA[0]=0x51;inputB[8191]=0xa4;
    MappingBacking backingA{1,inputA,nullptr},backingB{1,inputB,nullptr};
    auto& otherWindow=g_windows[other];
    Window* testWindows[]={&window,&otherWindow};
    for(auto* w:testWindows){
        w->vfb=reinterpret_cast<uint32_t*>(0x1000);w->vfbWidth=64;w->vfbHeight=32;
        w->vfbStride=64;w->vfbBytes=8192;w->externalCaptured=false;
    }
    window.vfbBacking=&backingA;otherWindow.vfbBacking=&backingB;
    {
        const uint64_t incarnation=window.creationIdentity;uint64_t tagged=0;
        CHECK(WmPublishSourceCommit(owner,window.wmId,incarnation,42)==-2);
        CHECK(WmPublishSourceCommit(stranger,window.wmId,incarnation+1,42)==-116);
        CHECK(WmPublishSourceCommit(stranger,window.wmId,incarnation,0)==-22);
        CHECK(WmPublishSourceCommit(stranger,window.wmId,incarnation,42)==-116);
        CHECK(WmBeginSourceWrite(stranger,window.wmId,incarnation)==0);
        CHECK(WmBeginSourceWrite(stranger,window.wmId,incarnation)==-16);
        CHECK(WmPublishSourceCommit(stranger,window.wmId,incarnation,42)==0);
        const uint64_t generation=window.publishedGeneration;
        CHECK(WmQuerySourceCommit(window.wmId,incarnation,generation,&tagged)==0 && tagged==42);
        CHECK(WmQuerySourceCommit(window.wmId,incarnation+1,generation,&tagged)==-116);
        CHECK(WmQuerySourceCommit(window.wmId,incarnation,generation+1,&tagged)==-116);
        CHECK(WmBeginSourceWrite(owner,window.wmId,incarnation)==-2);
        CHECK(WmBeginSourceWrite(stranger,window.wmId,incarnation+1)==-116);
        CHECK(WmBeginSourceWrite(stranger,window.wmId,incarnation)==0);
        CHECK(WmQuerySourceCommit(window.wmId,incarnation,generation,&tagged)==-116);
        CHECK(WmPublishSourceCommit(stranger,window.wmId,incarnation,43)==0);
        WmSignalDirtyById(stranger,window.wmId);
        CHECK(WmQuerySourceCommit(window.wmId,incarnation,generation,&tagged)==-116);
        CHECK(WmQuerySourceCommit(window.wmId,incarnation,window.publishedGeneration,&tagged)==-116);
    }
    WmDesktopLayers layers{};CHECK(WmAcquireDesktopLayers(&layers) && layers.count==2);
    Process* actualOwner=window.proc;
    Process* otherOwner=actualOwner==owner?stranger:owner;
    CHECK(!WmSetOwnedWindowProperties(otherOwner,window.wmId,WM_PROP_OPACITY,128,0));
    CHECK(WmDesktopLayersCurrent(layers));
    const auto initialOpacity=window.opacity;
    CHECK(WmSetOwnedWindowProperties(actualOwner,window.wmId,WM_PROP_OPACITY,128,0));
    CHECK(window.opacity==128 && !WmDesktopLayersCurrent(layers));
    CHECK(WmSetOwnedWindowProperties(actualOwner,window.wmId,WM_PROP_OPACITY,initialOpacity,0));
    CHECK(WmDesktopLayersCurrent(layers) && !lockDepth);
    CHECK(layers.layers[0].window==window.wmId && layers.layers[1].window==otherWindow.wmId);
    CHECK(backingA.references==2 && backingB.references==2 && WmDesktopLayersCurrent(layers));
    auto* retained=static_cast<WmDesktopLayers*>(std::malloc(sizeof(WmDesktopLayers)));CHECK(retained);
    CHECK(!WmRetainDesktopLayers(layers,&layers));
    CHECK(WmRetainDesktopLayers(layers,retained) && backingA.references==3 && backingB.references==3);
    WmReleaseDesktopLayers(retained);std::free(retained);
    CHECK(backingA.references==2 && backingB.references==2);
    {
        DesktopFrameScenes<WmDesktopLayers> frames{};
        auto* first=static_cast<WmDesktopLayers*>(std::malloc(sizeof(WmDesktopLayers)));
        auto* second=static_cast<WmDesktopLayers*>(std::malloc(sizeof(WmDesktopLayers)));CHECK(first && second);
        CHECK(WmRetainDesktopLayers(layers,first) && WmRetainDesktopLayers(layers,second));
        CHECK(backingA.references==4 && backingB.references==4);
        CHECK(frames.Insert({90,101,1001,11,21,3,true,first,false}));
        CHECK(frames.Insert({90,102,1002,12,22,1,true,second,true}));
        CHECK(!frames.Insert({90,103,1003,13,23,0,false,first,true}));
        CHECK(!frames.Find(90,101)->fullOnly && frames.Find(90,102)->fullOnly);
        CHECK(!frames.Find(91,101) && frames.Find(90,101)->backdrop==21 && frames.Find(90,102)->chromeMask==1);
        CHECK(!frames.Remove(91,101));
        auto* removed=frames.Remove(90,101);CHECK(removed==first && !frames.Find(90,101));
        WmReleaseDesktopLayers(removed);std::free(removed);
        CHECK(backingA.references==3 && frames.Find(90,102)->scene==second);
        removed=frames.Remove(90,102);CHECK(removed==second);WmReleaseDesktopLayers(removed);std::free(removed);
        CHECK(backingA.references==2 && backingB.references==2);
    }
    uint32_t chromeMask=0;auto observed=layers.layers[0];
    observed.title[0]^=1;CHECK(DesktopChromeBind(layers,observed,chromeMask) && chromeMask==1 && !DesktopChromeComplete(2,chromeMask));
    observed.clientX++;CHECK(!DesktopChromeBind(layers,observed,chromeMask));observed.clientX--;
    observed.backing=&backingB;CHECK(!DesktopChromeBind(layers,observed,chromeMask));
    CHECK(DesktopChromeBind(layers,layers.layers[1],chromeMask) && DesktopChromeComplete(2,chromeMask));
    CHECK(DesktopChromeComplete(32,~uint32_t{0}) && !DesktopChromeComplete(32,~uint32_t{1}));
    auto* changedScene=static_cast<WmDesktopLayers*>(std::malloc(sizeof(WmDesktopLayers)));CHECK(changedScene);
    *changedScene=layers;CHECK(DesktopChromeCarry(layers,chromeMask,*changedScene)==chromeMask);
    changedScene->layers[0].clientX++;CHECK(DesktopChromeCarry(layers,chromeMask,*changedScene)==2);
    changedScene->layers[0]=layers.layers[0];++changedScene->layers[1].incarnation;
    CHECK(DesktopChromeCarry(layers,chromeMask,*changedScene)==1);std::free(changedScene);
    g_wmActive=true;WmDesktopChrome chrome{};
    CHECK(!WmDesktopChromeCapture(layers,0,&chrome,nullptr,0,-1,-1));
    CHECK(chrome.width==window.outerWidth() && chrome.height==window.outerHeight());
    auto* chromePixels=static_cast<uint32_t*>(std::calloc(chrome.width*chrome.height,sizeof(uint32_t)));CHECK(chromePixels);
    CHECK(WmDesktopChromeCapture(layers,0,&chrome,chromePixels,1,-1,-1)==-22);
    CHECK(!WmDesktopChromeCapture(layers,0,&chrome,chromePixels,uint64_t(chrome.width)*chrome.height*4,-1,-1));
    window.title[0]^=1;window.focused=!window.focused;
    CHECK(WmDesktopChromeCapture(layers,0,&chrome,chromePixels,uint64_t(chrome.width)*chrome.height*4,-1,-1)==-116);
    CHECK(!WmDesktopChromeCapture(layers,0,&chrome,chromePixels,uint64_t(chrome.width)*chrome.height*4,-1,-1,true));
    window.title[0]^=1;window.focused=!window.focused;
    ++window.creationIdentity;
    CHECK(WmDesktopChromeCapture(layers,0,&chrome,chromePixels,uint64_t(chrome.width)*chrome.height*4,-1,-1,true)==-116);
    --window.creationIdentity;
    CHECK((chromePixels[0]>>24)==255 && chromePixels[uint64_t(window.clientY()-window.y)*chrome.width+window.clientX()-window.x]==0);
    for(uint32_t y=0;y<chrome.height;++y)for(uint32_t x=0;x<chrome.width;++x){
        const bool client=x>=uint32_t(window.clientX()-window.x) && x<uint32_t(window.clientX()-window.x)+window.clientW &&
                          y>=uint32_t(window.clientY()-window.y) && y<uint32_t(window.clientY()-window.y)+window.clientH;
        CHECK((chromePixels[uint64_t(y)*chrome.width+x]>>24)==(client?0u:255u));
    }
    otherWindow.x++;CHECK(WmDesktopChromeCapture(layers,0,&chrome,chromePixels,uint64_t(chrome.width)*chrome.height*4,-1,-1)==-116);otherWindow.x--;
    std::free(chromePixels);
    int uses=0;
    auto use=[](void* context)->int64_t {++*static_cast<int*>(context);return 42;};
    auto* candidate=static_cast<WmDesktopLayers*>(std::malloc(sizeof(WmDesktopLayers)));CHECK(candidate);
    *candidate=layers;int pairedUses=0;
    ++candidate->layers[0].generation;candidate->layers[0].clientX++;
    CHECK(WmDesktopLayersUseIfBothPresentable(layers,*candidate,&pairedUses,use)==42 && pairedUses==1);
    ++candidate->layers[0].incarnation;
    CHECK(WmDesktopLayersUseIfBothPresentable(layers,*candidate,&pairedUses,use)==-116 && pairedUses==1);
    --candidate->layers[0].incarnation;candidate->layers[0].backing=&backingB;
    CHECK(WmDesktopLayersUseIfBothPresentable(layers,*candidate,&pairedUses,use)==-116 && pairedUses==1);
    *candidate=layers;++candidate->layers[0].width;
    CHECK(WmDesktopLayersUseIfBothPresentable(*candidate,layers,&pairedUses,use)==-116 && pairedUses==1);
    *candidate=layers;++window.creationIdentity;
    CHECK(WmDesktopLayersUseIfBothPresentable(layers,*candidate,&pairedUses,use)==-116 && pairedUses==1);
    --window.creationIdentity;std::free(candidate);
    CHECK(WmDesktopLayersUseIfCurrent(layers,&uses,use)==42 && uses==1);
    ++window.publishedGeneration;window.x++;window.title[0]^=1;
    CHECK(!WmDesktopLayersCurrent(layers));
    CHECK(WmDesktopLayersUseIfPresentable(layers,&uses,use)==42 && uses==2);
    window.title[0]^=1;window.x--;--window.publishedGeneration;
    ++window.creationIdentity;CHECK(WmDesktopLayersUseIfPresentable(layers,&uses,use)==-116);--window.creationIdentity;
    auto* originalBacking=window.vfbBacking;window.vfbBacking=&backingB;
    CHECK(WmDesktopLayersUseIfPresentable(layers,&uses,use)==-116);window.vfbBacking=originalBacking;
    char originalTitle=window.title[0];window.title[0]^=1;
    CHECK(WmDesktopLayersUseIfCurrent(layers,&uses,use)==-116 && uses==2);window.title[0]=originalTitle;
    window.focused=!window.focused;CHECK(!WmDesktopLayersCurrent(layers));window.focused=!window.focused;
    window.appId[0]='x';CHECK(!WmDesktopLayersCurrent(layers));window.appId[0]=0;
    ++g_launcherSnapshotEpoch;CHECK(!WmDesktopLayersCurrent(layers));--g_launcherSnapshotEpoch;
    const auto theme=WmGetTheme();CHECK(WmSetTheme(theme==0?1:0));
    CHECK(!WmDesktopLayersCurrent(layers));CHECK(WmSetTheme(theme));CHECK(!WmDesktopLayersCurrent(layers));
    g_chromeSettingsEpoch=layers.settingsGeneration; // test restores the fixture snapshot
    otherWindow.x++;
    CHECK(WmDesktopLayersUseIfCurrent(layers,&uses,use)==-116 && uses==2);
    otherWindow.x--;
    WmDesktopLayerCopy copies[]={{outputA,sizeof(outputA)},{outputB,sizeof(outputB)}};
    CHECK(WmCopyDesktopLayers(layers,copies,2) && outputA[0]==0x51 && outputB[8191]==0xa4);
    copies[1].capacity=8191;CHECK(!WmCopyDesktopLayers(layers,copies,2));copies[1].capacity=8192;
    BrookDesktopLayers wire{};wire.version=BROOK_DESKTOP_LAYERS_VERSION;
    auto current=[](const WmDesktopLayers& snapshot){return WmDesktopLayersCurrent(snapshot);};
    auto sink=[](const WmDesktopLayer& l,uint64_t offset,uint64_t address,uint64_t bytes){
        if(!UserMappingBufferRead(l.backing,offset,reinterpret_cast<void*>(address),bytes))return false;
        DesktopInvertCaret(l,offset,reinterpret_cast<void*>(address),bytes);return true;
    };
    CHECK(!ExportDesktopLayers(layers,wire,false,sink,current) && wire.count==2);
    CHECK(wire.layers[0].bytes==8192 && wire.layers[0].window==window.wmId);
    auto alwaysCurrent=[](const WmDesktopLayers&){return true;};
    const auto savedOpacity=layers.layers[0].opacity;
    layers.layers[0].opacity=128;CHECK(!ExportDesktopLayers(layers,wire,false,sink,alwaysCurrent));
    CHECK((wire.layers[0].flags&BROOK_DESKTOP_LAYER_HAS_OPACITY) &&
          ((wire.layers[0].flags&BROOK_DESKTOP_LAYER_OPACITY_MASK)>>BROOK_DESKTOP_LAYER_OPACITY_SHIFT)==128);
    layers.layers[0].opacity=0;CHECK(!ExportDesktopLayers(layers,wire,false,sink,alwaysCurrent));
    CHECK((wire.layers[0].flags&BROOK_DESKTOP_LAYER_HAS_OPACITY) && !(wire.layers[0].flags&BROOK_DESKTOP_LAYER_OPACITY_MASK));
    layers.layers[0].opacity=savedOpacity;layers.layers[0].blurRadius=1;
    CHECK(ExportDesktopLayers(layers,wire,false,sink,alwaysCurrent)==-95);layers.layers[0].blurRadius=0;
    CHECK(!ExportDesktopLayers(layers,wire,false,sink,current));
    wire.layers[0].pixels=reinterpret_cast<uint64_t>(outputA);wire.layers[0].capacity=8192;
    wire.layers[1].pixels=reinterpret_cast<uint64_t>(outputB);wire.layers[1].capacity=8192;
    __builtin_memset(outputA,0,sizeof(outputA));__builtin_memset(outputB,0,sizeof(outputB));
    CHECK(!ExportDesktopLayers(layers,wire,true,sink,current) && outputA[0]==0x51 && outputB[8191]==0xa4);
    // Selective capture must not copy the shared source, but must still bind
    // every array entry to the query's window incarnation before skipping.
    wire.layers[0].pixels=wire.layers[0].capacity=0;
    __builtin_memset(outputA,0,sizeof(outputA));__builtin_memset(outputB,0,sizeof(outputB));
    CHECK(!ExportDesktopLayers(layers,wire,true,sink,current,65536,1) && outputA[0]==0 && outputB[8191]==0xa4);
    ++wire.layers[1].incarnation;CHECK(ExportDesktopLayers(layers,wire,true,sink,current,65536,1)==-11);--wire.layers[1].incarnation;
    CHECK(ExportDesktopLayers(layers,wire,true,sink,current,65536,4)==-22);
    wire.layers[1].pixels=wire.layers[1].capacity=0;
    auto unexpectedCopy=[](const WmDesktopLayer&,uint64_t,uint64_t,uint64_t){return false;};
    CHECK(!ExportDesktopLayers(layers,wire,true,unexpectedCopy,current,65536,3));
    wire.layers[1].pixels=reinterpret_cast<uint64_t>(outputB);wire.layers[1].capacity=8192;
    wire.layers[0].pixels=reinterpret_cast<uint64_t>(outputA);wire.layers[0].capacity=8192;
    CHECK(ExportDesktopLayers(layers,wire,true,sink,current,65536,1)==-22);
    unsigned batchedCalls=0;
    auto batchedSink=[&](const WmDesktopLayer& l,uint64_t offset,uint64_t destination,uint64_t bytes){
        ++batchedCalls;CHECK(bytes<=65536);return sink(l,offset,destination,bytes);
    };
    __builtin_memset(outputA,0,sizeof(outputA));__builtin_memset(outputB,0,sizeof(outputB));
    CHECK(!ExportDesktopLayers(layers,wire,true,batchedSink,current,65536) && batchedCalls==2);
    CHECK(outputA[0]==0x51 && outputB[8191]==0xa4);
    CHECK(ExportDesktopLayers(layers,wire,true,batchedSink,current,0)==-22);
    CHECK(ExportDesktopLayers(layers,wire,true,batchedSink,current,65537)==-22);
    constexpr uint64_t largeBytes=147456;
    auto* largeOutput=static_cast<uint8_t*>(std::malloc(largeBytes));CHECK(largeOutput);
    WmDesktopLayers large{};large.count=1;large.layers[0].stride=256;large.layers[0].height=144;
    large.layers[0].bytes=largeBytes;BrookDesktopLayers largeWire{};
    largeWire.version=BROOK_DESKTOP_LAYERS_VERSION;largeWire.count=1;
    largeWire.layers[0].pixels=reinterpret_cast<uint64_t>(largeOutput);largeWire.layers[0].capacity=largeBytes;
    unsigned largeCalls=0;
    auto patternedSink=[&](const WmDesktopLayer&,uint64_t offset,uint64_t destination,uint64_t bytes){
        ++largeCalls;auto* p=reinterpret_cast<uint8_t*>(destination);
        for(uint64_t i=0;i<bytes;++i)p[i]=uint8_t((offset+i)*37u);return true;
    };
    CHECK(!ExportDesktopLayers(large,largeWire,true,patternedSink,alwaysCurrent,65536) && largeCalls==3);
    for(uint64_t i=0;i<largeBytes;++i)CHECK(largeOutput[i]==uint8_t(i*37u));
    largeCalls=0;CHECK(!ExportDesktopLayers(large,largeWire,true,patternedSink,alwaysCurrent,4096) && largeCalls==36);
    std::free(largeOutput);
    auto captureCurrent=[](const WmDesktopLayers& snapshot){return WmDesktopLayersCaptureCurrent(snapshot);};
    bool changedVisual=false;
    auto visualSink=[&](const WmDesktopLayer& l,uint64_t offset,uint64_t address,uint64_t bytes){
        if(!changedVisual){window.title[0]^=1;window.focused=!window.focused;changedVisual=true;}
        return sink(l,offset,address,bytes);
    };
    CHECK(!ExportDesktopLayers(layers,wire,true,visualSink,captureCurrent) && changedVisual);
    CHECK(!WmDesktopLayersCurrent(layers) && WmDesktopLayersCaptureCurrent(layers));
    window.title[0]^=1;window.focused=!window.focused;
    ++window.publishedGeneration;CHECK(!WmDesktopLayersCaptureCurrent(layers));--window.publishedGeneration;
    bool changedIdentity=false;
    auto identitySink=[&](const WmDesktopLayer& l,uint64_t offset,uint64_t address,uint64_t bytes){
        if(!changedIdentity){++window.creationIdentity;changedIdentity=true;}
        return sink(l,offset,address,bytes);
    };
    CHECK(ExportDesktopLayers(layers,wire,true,identitySink,captureCurrent)==-11 && changedIdentity);
    --window.creationIdentity;
    auto saved=wire.layers[1].pixels;wire.layers[1].pixels=wire.layers[0].pixels+1;
    CHECK(ExportDesktopLayers(layers,wire,true,sink,current)==-22);wire.layers[1].pixels=saved;
    wire.count=1;CHECK(ExportDesktopLayers(layers,wire,true,sink,current)==-11);wire.count=2;
    wire.version=0;CHECK(ExportDesktopLayers(layers,wire,true,sink,current)==-22);wire.version=BROOK_DESKTOP_LAYERS_VERSION;
    auto failSink=[](const WmDesktopLayer&,uint64_t,uint64_t,uint64_t){return false;};
    CHECK(ExportDesktopLayers(layers,wire,true,failSink,current)==-14);
    otherWindow.x++;CHECK(!WmDesktopLayersCurrent(layers));otherWindow.x--;
    WmSetFocus(other);WmSetFocus(slot);CHECK(!WmDesktopLayersCurrent(layers));WmSendToBack(slot);
    window.focused=layers.layers[0].focused;otherWindow.focused=layers.layers[1].focused;
    CHECK(WmDesktopLayersCurrent(layers));mutateDuringCopy=&window;
    CHECK(ExportDesktopLayers(layers,wire,true,sink,current)==-11 && !mutateDuringCopy);
    CHECK(!WmCopyDesktopLayers(layers,copies,2));
    CHECK(!WmDesktopLayersCurrent(layers));WmReleaseDesktopLayers(&layers);
    CHECK(!layers.count && backingA.references==1 && backingB.references==1);
    otherWindow.externalCaptured=true;CHECK(!WmAcquireDesktopLayers(&layers));
    CHECK(!layers.count && backingA.references==1 && backingB.references==1);
    otherWindow.externalCaptured=false;
    // Legacy console/process buffers use a different lock and publication
    // generation. They are opaque in the existing compositor, irrespective
    // of the alpha byte stored in the source.
    otherWindow.vfb=nullptr;otherWindow.vfbBacking=nullptr;
    owner->fbVirtual=reinterpret_cast<uint32_t*>(0x2000);owner->fbBacking=&backingB;
    owner->fbVirtualSize=8192;owner->fbVfbWidth=64;owner->fbVfbHeight=32;owner->fbVfbStride=64;
    ProcessFramebufferMarkDirty(owner);
    caretPid=stranger->pid;caretIncarnation=stranger->incarnation;
    CHECK(!WmAcquireDesktopLayers(&layers)); // Native alpha needs post-blend inversion.
    testTick=500;CHECK(WmAcquireDesktopLayers(&layers)); // Blink off avoids inversion.
    CHECK(!layers.layers[1].caretWidth);WmReleaseDesktopLayers(&layers);
    testTick=0;caretPid=owner->pid;caretIncarnation=owner->incarnation; // Different owners cannot inherit the caret.
    CHECK(WmAcquireDesktopLayers(&layers) && layers.count==2);
    CHECK(!layers.layers[0].legacyOpaque && layers.layers[1].legacyOpaque && backingB.references==2);
    CHECK(!layers.layers[0].caretWidth && layers.layers[1].caretX==5 && layers.layers[1].caretWidth==3);
    wire={};wire.version=BROOK_DESKTOP_LAYERS_VERSION;
    CHECK(!ExportDesktopLayers(layers,wire,false,sink,current));
    CHECK(wire.layers[1].flags==BROOK_DESKTOP_LAYER_LEGACY_OPAQUE);
    wire.layers[0].pixels=reinterpret_cast<uint64_t>(outputA);wire.layers[0].capacity=sizeof(outputA);
    wire.layers[1].pixels=reinterpret_cast<uint64_t>(outputB);wire.layers[1].capacity=sizeof(outputB);
    CHECK(!ExportDesktopLayers(layers,wire,true,sink,current));
    const unsigned caretOffset=(64+5)*4;
    CHECK(outputB[caretOffset]==0xff && outputB[caretOffset+1]==0xff && outputB[caretOffset+2]==0xff &&
          outputB[caretOffset+3]==0 && inputB[caretOffset]==0);
    owner->fbBacking=&backingA;CHECK(!WmDesktopLayersCurrent(layers));owner->fbBacking=&backingB;
    CHECK(WmDesktopLayersCurrent(layers));
    ProcessFramebufferMarkDirty(owner);CHECK(!WmDesktopLayersCurrent(layers));
    WmReleaseDesktopLayers(&layers);CHECK(backingA.references==1 && backingB.references==1);
    caretPid=0;
    owner->fbPublicationGeneration=~uint64_t{0};CHECK(!WmAcquireDesktopLayers(&layers));
    CHECK(!layers.count && backingA.references==1 && backingB.references==1);
    owner->fbBacking=nullptr;owner->fbVirtual=nullptr;window.vfbBacking=nullptr;
    std::free(owner);std::free(stranger);
    std::puts("WINDOW_SURFACES PASS: stale slot, GL/SHM transition, destruction, reuse, owned geometry and chrome");
}
