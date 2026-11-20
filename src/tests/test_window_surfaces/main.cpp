#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include "../../kernel/src/window.cpp"
#include "../../kernel/src/chrome_raster.cpp"
#include "../../kernel/src/chrome_frame.cpp"
#include "../../kernel/src/chrome_shell.cpp"
#include "../../kernel/src/chrome_launcher.cpp"
#include "../../kernel/src/launcher_model.cpp"
#include "../../kernel/src/desktop_shell_surfaces.h"
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
#define TEST_FONT_SET {{&g_desktopFont100,&g_desktopFont100,&g_desktopFont100,&g_desktopFont100,&g_desktopFont100,&g_desktopFont100,&g_desktopFont100,&g_desktopFont100}}
extern const ChromeFontSet g_chromeFonts100 TEST_FONT_SET;
extern const ChromeFontSet g_chromeFonts150 TEST_FONT_SET;
extern const ChromeFontSet g_chromeFonts200 TEST_FONT_SET;
const char* BuildGitHash(){return "test";}
const char* BuildDate(){return "2026-11-19";}
bool CompositorGlassActive() { return false; }
uint32_t CompositorMaterialOpacity() { return 255; }
Terminal* TerminalFindByProcess(Process*) { return nullptr; }
TerminalResizeResult TerminalResizeForProcess(uint16_t,uint64_t,uint32_t,uint32_t) { return TerminalResizeResult::NotOwned; }
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
// Launcher save runs the real worker step against a minimal owned home.
static Vnode layoutFile{}, layoutDirectory{};
static char layoutText[LauncherLayoutLimit];
static unsigned layoutAttempts=0, layoutSyncs=0;
static bool layoutWritable=false, layoutRenamed=false;
bool ShutdownLaunchAllowed() { return true; }
const char* ShutdownDesktopStatus() { return "Applications"; }
void ShutdownDesktopRequest(bool) { std::abort(); }
void ShutdownDesktopCancel() { std::abort(); }
Process* KernelThreadCreate(const char*, KernelThreadFn, void*, uint8_t) { std::abort(); }
void SchedulerAddProcess(Process*) { std::abort(); }
extern "C" void SchedulerSleepMs(uint32_t) { std::abort(); }
void KLogCat(LogCat, LogLevel, const char*, ...) {}
void KLogCatConsole(LogCat, LogLevel, const char*, ...) {}
extern "C" void* kmalloc(uint64_t) { std::abort(); }
extern "C" void kfree(void*) { std::abort(); }
extern "C" Vnode* VfsOpen(const char*, int) { std::abort(); }
extern "C" int VfsRead(Vnode*, void*, uint64_t, uint64_t*) { std::abort(); }
extern "C" int VfsReaddir(Vnode*, DirEntry*, uint32_t*) { std::abort(); }
extern "C" int VfsStatPath(const char*, VnodeStat*) { std::abort(); }
int VfsPathRoot(VfsPath*) { ++layoutAttempts; return layoutWritable ? 0 : -30; }
int VfsPathResolve(const VfsPath&, const VfsPath&, const char*, VfsPath*, bool, bool) { return 0; }
void VfsPathRelease(VfsPath) {}
int VfsMkdirAt(const VfsPath&, const VfsPath&, const char*, uint16_t, uint16_t) { std::abort(); }
int VfsChownAt(const VfsPath&, const VfsPath&, const char*, uint32_t, uint32_t, bool) { std::abort(); }
int VfsRemoveAt(const VfsPath&, const VfsPath&, const char* name, bool directory) {
    CHECK(!strcmp(name, "launcher.ini.tmp") && !directory);
    return -2;
}
Vnode* VfsOpenAt(const VfsPath&, const VfsPath&, const char* name, int flags,
                 uint16_t mode, uint16_t, int*, bool followFinal) {
    CHECK(!strcmp(name, "launcher.ini.tmp") && !followFinal && flags == (VFS_O_WRITE|VFS_O_CREATE|VFS_O_EXCL));
    CHECK(mode == 0600);
    return &layoutFile;
}
Vnode* VfsPathOpen(const VfsPath&, int flags, int*) {
    CHECK(flags == VFS_O_READ && layoutRenamed);
    return &layoutDirectory;
}
extern "C" int VfsWrite(Vnode* file, const void* text, uint64_t bytes, uint64_t* offset) {
    CHECK(file == &layoutFile && !*offset && bytes < sizeof(layoutText));
    memcpy(layoutText, text, bytes); layoutText[bytes] = 0;
    *offset += bytes;
    return static_cast<int>(bytes);
}
extern "C" void VfsClose(Vnode* node) { CHECK(node == &layoutFile || node == &layoutDirectory); }
int VfsChown(Vnode* node, uint32_t uid, uint32_t gid) {
    CHECK(node == &layoutFile && uid == DesktopUid && gid == DesktopGid);
    return 0;
}
int VfsFsync(Vnode* node) {
    CHECK(node == (layoutRenamed ? &layoutDirectory : &layoutFile));
    ++layoutSyncs;
    return 0;
}
int VfsRenameAt(const VfsPath&, const VfsPath&, const char* source, const VfsPath&, const char* target) {
    CHECK(!strcmp(source, "launcher.ini.tmp") && !strcmp(target, "launcher.ini") && layoutSyncs == 1);
    layoutRenamed = true;
    return 0;
}
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
// Shell-surface layout reads the clock, thermal state and boot summary age.
uint64_t CompositorDesktopReadyTick() { return 0; }
uint64_t RtcNow() { return 0; }
char* RtcFormatTaskbar(char* buf,uint64_t,bool) { buf[0]='0';buf[1]=0;return buf; }
bool ThermalReadSnapshot(ThermalSnapshot&) { return false; }
uint32_t testScreenW=1024,testScreenH=768;
extern "C" void CompositorGetPhysDims(uint32_t* w,uint32_t* h) { if(w)*w=testScreenW;if(h)*h=testScreenH; }
}
extern "C" void KernelPanic(const char*,...) { std::abort(); }

int main() {
    using namespace brook;
    for(auto& glyph:badgeGlyphs)glyph={0,0,2,2,0,2,3};
    const uint64_t initialStyle=WmBackdropStyleVersion();
    SetWmActiveLocked(true);CHECK(WmBackdropStyleVersion()==initialStyle+2);
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
    CHECK(geometry.client_x==-26 && geometry.client_y==64);
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
    CHECK(geometry.incarnation!=originalIncarnation && geometry.client_x==12 && geometry.client_y==52);
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
    // WM_POP_INPUT uses two signed v120 fields at offsets 4 and 6,
    // while the size and legacy coordinate/capture fields stay stable.
    InputEvent scroll{};
    scroll.type=InputEventType::MouseScroll;
    scroll.modifiers=1;
    scroll.deltaY=-37;
    scroll.deltaX=83;
    scroll.scrollFlags=BROOK_WM_SCROLL_FINGER;
    CHECK(WmInputPush(&window,scroll,451,129));
    CHECK(WmInputPop(&window,&event,1)==1 && event.x==451 && event.y==129);
    CHECK(event.modifiers==1 && !event.reserved);
    window.fineScroll=true;
    CHECK(WmInputPush(&window,scroll,451,129));
    CHECK(WmInputPop(&window,&event,1)==1);
    const uint8_t expected[]={5,0,0,0x61,0xdb,0xff,83,0,0xc3,1,129,0};
    CHECK(sizeof(event)==sizeof(expected) && !__builtin_memcmp(&event,expected,sizeof(expected)));
    scroll.scrollFlags=0;
    scroll.deltaX=scroll.deltaY=0;
    scroll.scanCode=static_cast<uint8_t>(-2);
    scroll.ascii=1;
    CHECK(WmInputPush(&window,scroll,451,129));
    CHECK(WmInputPop(&window,&event,1)==1 && event.x==-240 && event.y==120);
    CHECK(event.modifiers==(1 | BROOK_WM_SCROLL_FINE));
    window.fineScroll=false;
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
    CHECK(WmSetTheme(BROOK_LEGACY_PALETTE_OPAL));
    CHECK(!WmDesktopLayersCurrent(layers));CHECK(WmSetTheme(BROOK_LEGACY_PALETTE_CLEARWATER));CHECK(!WmDesktopLayersCurrent(layers));
    g_chromeSettingsEpoch=layers.settingsGeneration; // test restores the fixture snapshot
    // Stage 1.5: a backend change re-resolves the theme (Glass needs blur;
    // without it the opaque material stands in) and stales every capture;
    // reporting an unchanged capability does not.
    CHECK(!WmBackendBlur() && (WmResolvedAppearance()&BROOK_APPEARANCE_PERFORMANCE));
    WmSetBackendBlur(false);CHECK(WmDesktopLayersCurrent(layers));
    WmSetBackendBlur(true);CHECK(!WmDesktopLayersCurrent(layers));
    CHECK(WmResolvedAppearance()==WmGetAppearance() && !(WmGetAppearance()&BROOK_APPEARANCE_PERFORMANCE));
    WmSetBackendBlur(false);CHECK(WmResolvedAppearance()==(WmGetAppearance()|BROOK_APPEARANCE_PERFORMANCE));
    g_chromeSettingsEpoch=layers.settingsGeneration;CHECK(WmDesktopLayersCurrent(layers));
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
    {
        // Client keep: unchanged publications copy nothing and are flagged
        // KEPT; a newer generation, a resize or a caret is copied.
        wire.layers[0].pixels=reinterpret_cast<uint64_t>(outputA);wire.layers[0].capacity=8192;
        wire.layers[1].pixels=reinterpret_cast<uint64_t>(outputB);wire.layers[1].capacity=8192;
        CHECK(!ExportDesktopLayers(layers,wire,true,sink,current));
        const BrookDesktopLayer held0=wire.layers[0],held1=wire.layers[1];
        DesktopCaptureCounters counters{};
        __builtin_memset(outputA,0,sizeof(outputA));__builtin_memset(outputB,0,sizeof(outputB));
        CHECK(!ExportDesktopLayers(layers,wire,true,sink,current,65536,0,3,&counters));
        CHECK(outputA[0]==0 && outputB[8191]==0);                           // nothing copied
        CHECK((wire.layers[0].flags&BROOK_DESKTOP_LAYER_KEPT) && (wire.layers[1].flags&BROOK_DESKTOP_LAYER_KEPT));
        CHECK(counters.calls==1 && counters.keeps==2 && !counters.captures && counters.bytesKept==16384 && !counters.bytesCopied);
        // Held metadata older than the publication: copied, not kept.
        wire.layers[0]=held0;wire.layers[0].generation=held0.generation-1;wire.layers[1]=held1;
        wire.layers[0].pixels=reinterpret_cast<uint64_t>(outputA);wire.layers[0].capacity=8192;
        wire.layers[1].pixels=reinterpret_cast<uint64_t>(outputB);wire.layers[1].capacity=8192;
        counters={};
        CHECK(!ExportDesktopLayers(layers,wire,true,sink,current,65536,0,3,&counters));
        CHECK(outputA[0]==0x51 && outputB[8191]==0 && !(wire.layers[0].flags&BROOK_DESKTOP_LAYER_KEPT));
        CHECK(wire.layers[0].generation==held0.generation);                 // returns the current publication
        CHECK(counters.keeps==1 && counters.captures==1 && counters.keepMisses==1 && counters.bytesCopied==8192);
        // Geometry change while generation matches is never kept.
        wire.layers[0]=held0;wire.layers[0].stride=held0.stride+1;wire.layers[1]=held1;
        wire.layers[0].pixels=reinterpret_cast<uint64_t>(outputA);wire.layers[0].capacity=8192;
        wire.layers[1].pixels=reinterpret_cast<uint64_t>(outputB);wire.layers[1].capacity=8192;
        __builtin_memset(outputA,0,sizeof(outputA));
        CHECK(!ExportDesktopLayers(layers,wire,true,sink,current,65536,0,1) && outputA[0]==0x51);
        // A terminal caret is inverted into each copy, so never kept.
        wire.layers[0]=held0;wire.layers[1]=held1;
        wire.layers[0].pixels=reinterpret_cast<uint64_t>(outputA);wire.layers[0].capacity=8192;
        wire.layers[1].pixels=reinterpret_cast<uint64_t>(outputB);wire.layers[1].capacity=8192;
        layers.layers[0].caretWidth=1;layers.layers[0].caretHeight=1;__builtin_memset(outputA,0,sizeof(outputA));
        CHECK(!ExportDesktopLayers(layers,wire,true,sink,current,65536,0,1) && !(wire.layers[0].flags&BROOK_DESKTOP_LAYER_KEPT));
        layers.layers[0].caretWidth=layers.layers[0].caretHeight=0;
        // Masks: keep and skip may not overlap; keep beyond the layer count,
        // or with a query (no capture), is invalid; identities must match.
        wire.layers[0]=held0;wire.layers[1]=held1;
        wire.layers[0].pixels=reinterpret_cast<uint64_t>(outputA);wire.layers[0].capacity=8192;
        wire.layers[1].pixels=reinterpret_cast<uint64_t>(outputB);wire.layers[1].capacity=8192;
        CHECK(ExportDesktopLayers(layers,wire,true,sink,current,65536,1,1)==-22);
        CHECK(ExportDesktopLayers(layers,wire,true,sink,current,65536,0,4)==-22);
        CHECK(ExportDesktopLayers(layers,wire,false,sink,current,65536,0,1)==-22);
        ++wire.layers[1].incarnation;CHECK(ExportDesktopLayers(layers,wire,true,sink,current,65536,0,1)==-11);--wire.layers[1].incarnation;
        // A failing copy publishes no counters.
        auto failAll=[](const WmDesktopLayer&,uint64_t,uint64_t,uint64_t){return false;};
        wire.layers[0].generation=held0.generation-1;counters={};
        CHECK(ExportDesktopLayers(layers,wire,true,failAll,current,65536,0,3,&counters)==-14 && !counters.calls && !counters.captures);
        // A publication racing the keep decision fails the capture (retry).
        wire.layers[0]=held0;wire.layers[1]=held1;
        wire.layers[0].pixels=reinterpret_cast<uint64_t>(outputA);wire.layers[0].capacity=8192;
        wire.layers[1].pixels=reinterpret_cast<uint64_t>(outputB);wire.layers[1].capacity=8192;
        auto captureCurrent0=[](const WmDesktopLayers& snapshot){return WmDesktopLayersCaptureCurrent(snapshot);};
        bool bumped=false;
        auto racingSink=[&](const WmDesktopLayer& l,uint64_t offset,uint64_t address,uint64_t bytes){
            if(!bumped){++window.publishedGeneration;bumped=true;}return sink(l,offset,address,bytes);};
        wire.layers[1].generation=held1.generation-1;   // layer 1 copies, layer 0 keeps
        CHECK(ExportDesktopLayers(layers,wire,true,racingSink,captureCurrent0,65536,0,3)==-11 && bumped);
        --window.publishedGeneration;
        wire.layers[0].flags=wire.layers[1].flags=0;
    }
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
    // One shell-surface list for every compositor path: the taskbar first,
    // then one shared popup slot, the boot toast and the expanded HUD.
    {
        constexpr uint32_t sw=1024,sh=768;
        auto kinds=[&](WmShellSurface (&s)[WM_SHELL_SURFACE_LIMIT]){
            const uint32_t n=WmShellSurfaces(sw,sh,-1,-1,s);
            CHECK(n>=1 && n<=WM_SHELL_SURFACE_LIMIT);
            CHECK(s[0].kind==WmShellSurfaceKind::Taskbar && s[0].material && s[0].x==0 &&
                  s[0].y==int32_t(WmDesktopHeight(sh)) && s[0].w==sw && s[0].h==sh-WmDesktopHeight(sh));
            for(uint32_t i=1;i<n;++i)CHECK(s[i].kind!=WmShellSurfaceKind::Taskbar && s[i].material);
            return n;
        };
        WmShellSurface s[WM_SHELL_SURFACE_LIMIT];
        g_launcherOpen=false;g_volumePopupOpen=false;
        uint32_t n=kinds(s);
        for(uint32_t i=1;i<n;++i)CHECK(s[i].kind==WmShellSurfaceKind::BootSummary);
        g_volumePopupOpen=true;n=kinds(s);
        CHECK(n>=2 && s[1].kind==WmShellSurfaceKind::VolumePopup);
        int32_t x,y;uint32_t w,h;WmVolumePopupGetRect(sw,sh,&x,&y,&w,&h);
        CHECK(s[1].x==x && s[1].y==y && s[1].w==w && s[1].h==h);
        // The slot holds one popup even if both flags were ever set.
        g_launcherOpen=true;n=kinds(s);
        CHECK(n>=2 && s[1].kind==WmShellSurfaceKind::Launcher);
        for(uint32_t i=2;i<n;++i)CHECK(s[i].kind==WmShellSurfaceKind::BootSummary);
        g_launcherOpen=false;g_volumePopupOpen=false;
        const auto tray = GetTaskbarLayout(sw, sh).network;
        CHECK(WmTrayFlyoutPress(tray.x, tray.y, sw, sh));
        CHECK(kinds(s) >= 2 && s[1].kind == WmShellSurfaceKind::TrayFlyout);
        CHECK(WmTrayFlyoutPress(s[1].x + 12, s[1].y + 12, sw, sh) && WmTrayFlyoutVisible());
        CHECK(WmTrayFlyoutPress(tray.x, tray.y, sw, sh) && !WmTrayFlyoutVisible());
        WmCyclePerformanceHud();
        CHECK(GetTaskbarLayout(sw, sh).hud.w > 0);
        WmCyclePerformanceHud();
        n = kinds(s);
        const auto& hud = s[n - 1];
        CHECK(hud.kind == WmShellSurfaceKind::PerformanceHud);
        CHECK(hud.x + hud.w + WmMetric(8) == sw && hud.y + hud.h + WmMetric(8) == WmDesktopHeight(sh));
        WmCyclePerformanceHud();
        CHECK(GetTaskbarLayout(sw, sh).hud.w == 0);
    }
    // In stage 3 the shell renders from this kernel model, and pointer presses
    // still enter through the WM hit targets. A pin should immediately appear
    // in the next shell snapshot and queue exactly one durable save.
    {
        constexpr uint32_t sw = 1024, sh = 768;
        static LauncherSnapshot catalog{};
        catalog.generation = 1;
        catalog.count = 2;
        catalog.items[0].valid = catalog.items[1].valid = true;
        WmStrCopy(catalog.items[0].title, "Settings", sizeof(catalog.items[0].title));
        WmStrCopy(catalog.items[0].appId, "settings", sizeof(catalog.items[0].appId));
        WmStrCopy(catalog.items[1].title, "Snapshot Fixture", sizeof(catalog.items[1].title));
        WmStrCopy(catalog.items[1].appId, "snapshot-fixture", sizeof(catalog.items[1].appId));
        g_launcherActive = &catalog;
        g_launcherModel.Init();
        g_launcherPersistence.load = {"missing", -2};
        testTick = 100;
        LauncherSyncModel(catalog);
        g_launcherLoaded = g_launcherOpen = true;
        g_launcherModel.Open();
        LauncherPersistenceStatus state;
        WmLauncherPersistenceStatus(state);
        CHECK(state.pending && !state.savedEpoch && state.layoutEpoch);
        const uint32_t defaultsEpoch = state.layoutEpoch;
        auto press = [&](LauncherTargetKind kind, uint16_t index, bool secondary) {
            const auto& frame = LauncherFrame(sw, sh, -1, -1);
            for (uint32_t i = 0; i < frame.targetCount; ++i) {
                const auto hit = frame.targets[i];
                if (hit.target.kind == kind && hit.target.index == index)
                    return WmLauncherPress(hit.rect.x + hit.rect.w / 2, hit.rect.y + hit.rect.h / 2,
                                           sw, sh, secondary);
            }
            return false;
        };
        testTick = 200;
        CHECK(press(LauncherTargetKind::Row, 1, true));
        CHECK(g_launcherPersistence.layoutEpoch == defaultsEpoch && g_launcherSaveSince == 100);
        CHECK(press(LauncherTargetKind::MenuItem, 0, false));
        WmLauncherPersistenceStatus(state);
        CHECK(state.pending && state.savedEpoch == 0 && state.layoutEpoch != defaultsEpoch);
        CHECK(state.pinCount && !strcmp(state.pins[state.pinCount - 1], "snapshot-fixture"));
        static ChromeShellModel snapshot{};
        WmCaptureLauncher(sw, sh, -1, -1, snapshot);
        CHECK(snapshot.launcherModel.Pinned(1));
        testTick = 1199;
        LauncherSaveLayout();
        CHECK(!layoutAttempts);
        testTick = 1200;
        LauncherSaveLayout();
        WmLauncherPersistenceStatus(state);
        CHECK(layoutAttempts == 1 && state.save.error == -30 && state.pending && !state.savedEpoch);
        testTick = 6199;
        LauncherSaveLayout();
        CHECK(layoutAttempts == 1);
        testTick = 6200;
        layoutWritable = true;
        LauncherSaveLayout();
        WmLauncherPersistenceStatus(state);
        CHECK(layoutAttempts == 2 && layoutSyncs == 2 && !state.save.error && !state.pending);
        CHECK(state.savedEpoch == state.layoutEpoch);
        static LauncherModel restored;
        restored.Init();
        CHECK(restored.Parse(layoutText));
        CHECK(restored.PinCount() == state.pinCount);
        CHECK(!strcmp(restored.PinKey(restored.PinCount() - 1), "snapshot-fixture"));
        g_launcherOpen = g_launcherLoaded = false;
        g_launcherActive = &g_emptyLauncher;
    }

    // Fullscreen: the client covers the whole output without chrome, and while
    // it is focused the taskbar is neither drawn nor hit-tested (the launcher
    // and volume popup bring it back). Leaving restores geometry, chrome and
    // the maximized state. The Xwayland WM shipped before WM_SET_FULLSCREEN
    // makes a chrome-less output-sized window, which counts as fullscreen.
    {
        constexpr uint32_t sw=1024,sh=768;testScreenW=sw;testScreenH=sh;
        auto* gamer=static_cast<Process*>(std::calloc(1,sizeof(Process)));CHECK(gamer);
        gamer->pid=gamer->tgid=9;gamer->incarnation=99;
        const int game=WmCreateWindow(gamer,40,50,640,480,"Game",1,true);
        const int editor=WmCreateWindow(gamer,60,70,320,240,"Editor",1,true);
        CHECK(game>=0 && editor>=0);
        auto& g=g_windows[game];
        g.vfb=reinterpret_cast<uint32_t*>(0x1000); // API window: resizes notify the client
        WmSetFocus(game);
        auto taskbarShown=[&]{
            WmShellSurface s[WM_SHELL_SURFACE_LIMIT];const uint32_t n=WmShellSurfaces(sw,sh,-1,-1,s);
            bool shown=false;for(uint32_t i=0;i<n;++i)shown|=s[i].kind==WmShellSurfaceKind::Taskbar;
            const int32_t apps=WmTaskbarHitTest(int32_t(WmMetric(10)),int32_t(sh-WmMetric(10)),sw,sh);
            CHECK(shown==(apps!=-1) && shown==!WmTaskbarHidden());
            CHECK(WmTaskbarTop(sh)==(shown?WmDesktopHeight(sh):sh));
            return shown;
        };
        CHECK(taskbarShown());
        WmSetMaximized(game,true);
        CHECK(g.state==WindowState::Maximized && g.clientH<WmDesktopHeight(sh) && taskbarShown());
        Window::WmInputEvent drain[256]{},event{};WmInputPop(&g,drain,256);
        CHECK(WmSetFullscreen(game,true) && g.fullscreen && g.noChrome && g.x==0 && g.y==0);
        CHECK(g.clientW==sw && g.clientH==sh && g.outerWidth()==sw && g.outerHeight()==sh);
        CHECK(WmInputPop(&g,&event,1)==1 && event.type==WM_EVT_RESIZED && event.reserved==WM_RESIZE_FULLSCREEN);
        CHECK(event.x==int16_t(sw) && event.y==int16_t(sh));
        CHECK(!taskbarShown());
        // Maximize requests, moves and decoration changes do not disturb it.
        WmSetMaximized(game,false);WmMoveWindow(game,10,10);
        WmSetClientSideDecoration(game,false);
        CHECK(g.fullscreen && g.noChrome && g.x==0 && g.y==0 && g.clientW==sw && !taskbarShown());
        g_launcherOpen=true;CHECK(taskbarShown());g_launcherOpen=false;
        g_volumePopupOpen=true;CHECK(taskbarShown());g_volumePopupOpen=false;
        CHECK(!taskbarShown());
        WmSetFocus(editor);CHECK(taskbarShown());       // Alt+Tab away
        WmSetFocus(game);CHECK(!taskbarShown());
        WmMinimizeWindow(game);CHECK(taskbarShown());
        WmRestoreWindow(game);WmSetFocus(game);CHECK(!taskbarShown());
        {
            WmDebugWindow snapshots[WM_MAX_WINDOWS]{};uint64_t tick=0;bool seen=false;
            const uint32_t n=WmSnapshotDebugWindows(snapshots,WM_MAX_WINDOWS,&tick);
            for(uint32_t i=0;i<n;++i)if(snapshots[i].id==g.wmId){seen=true;CHECK(snapshots[i].fullscreen && !snapshots[i].maximized);}
            CHECK(seen);
            BrookWindowState state{};CHECK(WmSnapshotOwnedWindowState(gamer,g.wmId,0,&state)==0);
            CHECK((state.flags&BROOK_WINDOW_STATE_FULLSCREEN) && !(state.flags&BROOK_WINDOW_STATE_MAXIMIZED));
            CHECK(state.client_x==0 && state.client_y==0 && state.width==sw && state.height==sh);
        }
        WmInputPop(&g,drain,256);
        CHECK(WmSetFullscreen(game,false) && !g.fullscreen && g.state==WindowState::Maximized);
        // The decoration requested while fullscreen (server-side) applies now.
        CHECK(!g.noChrome && g.clientH<WmDesktopHeight(sh) && taskbarShown());
        CHECK(WmInputPop(&g,&event,1)==1 && event.reserved==WM_RESIZE_MAXIMIZED);
        WmSetMaximized(game,false);
        CHECK(g.state==WindowState::Normal && g.x==40 && g.y==50 && g.clientW==640 && g.clientH==480);
        // Legacy Xwayland WM: chrome-less, placed at the origin, output-sized.
        WmSetClientSideDecoration(game,true);WmMoveWindow(game,0,0);
        CHECK(WmResizeWindow(game,uint16_t(sw),uint16_t(sh)) && !g.fullscreen && !taskbarShown());
        CHECK(WmResizeWindow(game,uint16_t(sw),uint16_t(sh-1)) && taskbarShown());
        g.vfb=nullptr;WmDestroyWindowById(gamer,g_windows[editor].wmId);WmDestroyWindowById(gamer,g.wmId);
        CHECK(taskbarShown() && !lockDepth);
        std::free(gamer);
    }
    // Web-app names are resolved by app ID on metadata changes and catalog
    // publication. The three chrome consumers share the derived title, while
    // the diagnostic snapshot retains the original string.
    {
        constexpr uint32_t sw = 1024, sh = 768;
        auto* client = static_cast<Process*>(std::calloc(1, sizeof(Process)));
        CHECK(client);
        client->pid = client->tgid = 7;
        client->incarnation = 71;
        static LauncherSnapshot webCatalog{};
        webCatalog.count = 1;
        webCatalog.generation = 2;
        auto& app = webCatalog.items[0];
        app.valid = app.isDesktopEntry = app.isUserDesktopEntry = app.isWebApp = true;
        WmStrCopy(app.title, "Brook Mail", sizeof(app.title));
        WmStrCopy(app.appId, "org.Brook.Mail", sizeof(app.appId));
        g_launcherActive = &webCatalog;

        const int slot = WmCreateWindow(client, 20, 40, 360, 240, "mail.example");
        CHECK(slot >= 0);
        auto& window = g_windows[slot];
        CHECK(!strcmp(WindowDisplayTitle(window), "mail.example"));
        CHECK(WmSetAppIdById(client, window.wmId, app.appId));
        CHECK(!strcmp(WindowDisplayTitle(window), "Brook Mail"));
        WmSetTitleById(client, window.wmId, "Inbox");
        const char formatted[] = "Brook Mail \xe2\x80\x93 Inbox";
        CHECK(!strcmp(WindowDisplayTitle(window), formatted));

        WmDebugWindow info{};
        SnapshotDebugWindowLocked(window, &info);
        CHECK(!strcmp(info.title, formatted) && !strcmp(info.rawTitle, "Inbox"));
        static ChromeShellModel snapshot{};
        WmSetFocus(slot);
        const auto layout = GetTaskbarLayout(sw, sh);
        int row = 0;
        while (row < layout.count && layout.indices[row] != slot) ++row;
        CHECK(row < layout.count);
        WmCaptureTaskbar(sw, sh, 0, -1, -1, snapshot);
        CHECK(!strcmp(snapshot.entries[row].label, formatted));
        const auto button = layout.buttons[row];
        char text[48]{};
        const char* detail = nullptr;
        DesktopRect box{};
        CHECK(WmTrayTooltip(sw, sh, button.x + 2, button.y + 2, text, detail, box));
        CHECK(!strcmp(text, formatted));

        g_launcherPending = &g_emptyLauncher;
        CHECK(WmLauncherApplyPending() && !strcmp(WindowDisplayTitle(window), "Inbox"));
        g_launcherRetired = nullptr;
        WmDestroyWindowById(client, window.wmId);
        std::free(client);
    }

    // One metrics source: the WM constants are DesktopMetrics' bases, and the
    // client offset used outside the WM equals the WM's own arithmetic.
    {
        static_assert(WM_TITLE_BAR_HEIGHT==uint32_t(DesktopMetrics::TitleBase) &&
                      WM_BORDER_WIDTH==uint32_t(DesktopMetrics::BorderBase) &&
                      WM_BUTTON_WIDTH==uint32_t(DesktopMetrics::CaptionButtonBase) &&
                      WM_TASKBAR_HEIGHT==uint32_t(DesktopMetrics::TaskbarBase),"WM metrics");
        for(const auto* metrics:{&DesktopMetric100,&DesktopMetric150,&DesktopMetric200}){
            g_desktopMetrics=metrics;
            CHECK(uint32_t(metrics->clientOffsetX())==WmMetric(WM_BORDER_WIDTH));
            CHECK(uint32_t(metrics->clientOffsetY())==WmMetric(WM_TITLE_BAR_HEIGHT)+WmMetric(WM_BORDER_WIDTH));
            CHECK(uint32_t(metrics->taskbar())==WmMetric(WM_TASKBAR_HEIGHT));
        }
        g_desktopMetrics=&DesktopMetric100;
    }
    // Losing backdrop blur changes the material and preserves the content mode.
    {
        CHECK(WmGetThemeSelection()==BROOK_THEME_SELECTION_DEFAULT);
        WmSetBackendBlur(false);
        CHECK(WmResolvedTheme()==BROOK_THEME_OPAQUE && WmCurrentPalette().taskbar==BROOK_REF_DARK_FLAT);
        CHECK(!(WmGetAppearance()&BROOK_APPEARANCE_DARK) && WmContentColour(BROOK_CONTENT_BACKGROUND)==BROOK_REF_LIGHT_BG);
        WmSetBackendBlur(true);
        CHECK(WmResolvedTheme()==BROOK_THEME_GLASS && WmCurrentPalette().titleActive==BROOK_REF_DARK_TITLE);
        CHECK(WmContentColour(BROOK_CONTENT_SELECTION)==brook_surface_palette(0)->selection);
        const uint32_t a=WmGetAppearance();
        CHECK(WmSetAppearance(a) && WmGetThemeSelection()==BROOK_THEME_SELECTION_DEFAULT);
        CHECK(WmSetAppearance((a&~3u)|BROOK_APPEARANCE_PERFORMANCE));
        CHECK(WmGetThemeSelection()==BROOK_THEME_SELECTION(BROOK_THEME_OPAQUE,BROOK_MODE_MIXED,BROOK_HUE_BLUE));
        CHECK(WmCurrentPalette().taskbar==BROOK_REF_DARK_FLAT && WmContentColour(BROOK_CONTENT_BACKGROUND)==BROOK_REF_LIGHT_BG);
        CHECK(WmSetAppearance((a&~3u)|BROOK_APPEARANCE_DARK));
        CHECK(WmGetThemeSelection()==BROOK_THEME_SELECTION(BROOK_THEME_GLASS,BROOK_MODE_DARK,BROOK_HUE_BLUE));
        CHECK(WmContentColour(BROOK_CONTENT_BACKGROUND)==BROOK_REF_DARK_BG);
        CHECK(WmSetAppearance(a&~3u) && WmGetThemeSelection()==BROOK_THEME_SELECTION(BROOK_THEME_GLASS,BROOK_MODE_LIGHT,BROOK_HUE_BLUE));
        CHECK(WmCurrentPalette().taskbar==BROOK_REF_LIGHT_GLASS);
        WmSetBackendBlur(false);
        CHECK(WmCurrentPalette().taskbar==BROOK_REF_LIGHT_FLAT);
        CHECK(brook_appearance_opacity(WmGetAppearance())==brook_appearance_opacity(a));
        CHECK(!WmSetTheme(3) && !WmSetThemeSelection(BROOK_THEME_SELECTION(0,0,14)));
        const uint64_t magenta=brook_selection_make(BROOK_THEME_GLASS,BROOK_MODE_LIGHT,BROOK_HUE_CUSTOM,0xff00ff);
        CHECK(WmSetThemeSelection(magenta) && WmGetThemeSelection()==magenta);
        const WmPalette snapshot=WmCurrentPalette();
        CHECK(snapshot.accent==0xff00ff && WmContentColour(BROOK_CONTENT_ACCENT)==0xff00ff);
        WmSetPerformanceMode(true);
        CHECK(WmGetThemeSelection()==(magenta|BROOK_THEME_OPAQUE));
        CHECK(WmSetThemeSelection(brook_selection_make(BROOK_THEME_OPAQUE,BROOK_MODE_LIGHT,BROOK_HUE_CUSTOM,0x44ff11)));
        CHECK(WmCurrentPalette().accent==0x44ff11 && snapshot.accent==0xff00ff &&
              WmContentColour(BROOK_CONTENT_ACCENT)==0x44ff11);
        CHECK(WmSetTheme(BROOK_LEGACY_PALETTE_SMOKED) && brook_selection_hue(WmGetThemeSelection())==BROOK_HUE_SLATE);
        CHECK(WmSetThemeSelection(BROOK_THEME_SELECTION_DEFAULT));WmSetBackendBlur(false);
    }
    std::puts("WINDOW_SURFACES PASS: stale slot, GL/SHM transition, destruction, reuse, owned geometry, chrome and fullscreen");
}
