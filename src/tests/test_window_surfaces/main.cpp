#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <utility>
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
const char* BuildGitCommit(){return "0123456789abcdef0123456789abcdef01234567";}
static BootGuardSlot fixtureSlot{};
BootGuardSlot BootTimingHandoffSlot() { return fixtureSlot; }
bool HardwareBuildProvenanceRead(DesktopBuildProvenance& info) { info={};return false; }
const char* BuildDate(){return "2026-11-19";}
bool CompositorGlassActive() { return false; }
uint32_t CompositorMaterialOpacity() { return 255; }
Terminal* TerminalFindByProcess(Process*) { return nullptr; }
TerminalResizeResult TerminalResizeForProcess(uint16_t,uint64_t,uint32_t,uint32_t) { return TerminalResizeResult::NotOwned; }
// The dock's Close button reaches the cooperative close path; no test window
// takes the native-signal branch.
bool TerminalSnapshotGeometry(Process*,TerminalGeometry*) { std::abort(); }
int ProcessSendSignal(Process*,int,const SignalInfo*,const Process*) { std::abort(); }
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
void CompositorMarkSceneChanged() { CompositorMarkDirty(); }
// Launcher save runs the real worker step against a minimal owned home.
static Vnode layoutFile{}, layoutDirectory{};
static char layoutText[LauncherLayoutLimit];
static unsigned layoutAttempts=0, layoutSyncs=0;
static bool layoutWritable=false, layoutRenamed=false;
static bool shutdownPending=false,forceAppsOffered=false;
static unsigned forceCloseCalls=0;
bool ShutdownLaunchAllowed() { return !shutdownPending; }
bool ShutdownCanForceApps(const char*) { return forceAppsOffered; }
void ShutdownDesktopForceApps() { CHECK(forceAppsOffered);++forceCloseCalls; }
static bool shutdownFailed=false;
static unsigned retryCalls=0,forcePowerOffCalls=0;
static constexpr char failedStatus[]="Shutdown failed: could not save data (I/O error). Retry or force power off.";
const char* ShutdownDesktopStatus() { return shutdownFailed?failedStatus:"Applications"; }
ShutdownFailureActions ShutdownDesktopFailureActions() { return {shutdownFailed,shutdownFailed,shutdownFailed}; }
void ShutdownDesktopRetry() { CHECK(shutdownFailed);++retryCalls; }
void ShutdownDesktopForcePowerOff() { CHECK(shutdownFailed);++forcePowerOffCalls; }
void ShutdownDesktopRequest(bool) { std::abort(); }
void ShutdownDesktopCancel() { std::abort(); }
ShutdownRestartStatus ShutdownRestartPromptStatus(const char*) { return {}; }
void ShutdownRestartPromptChoose(uint32_t,bool) { std::abort(); }
void ShutdownRestartPendingNow() { std::abort(); }
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
    CHECK((!strcmp(name, "launcher.ini.tmp") || !strcmp(name, "dock.ini.tmp") || !strcmp(name, "taskbar.ini.tmp")) && !directory);
    return -2;
}
Vnode* VfsOpenAt(const VfsPath&, const VfsPath&, const char* name, int flags,
                 uint16_t mode, uint16_t, int*, bool followFinal) {
    CHECK((!strcmp(name, "launcher.ini.tmp") || !strcmp(name, "dock.ini.tmp") || !strcmp(name, "taskbar.ini.tmp")) && !followFinal &&
          flags == (VFS_O_WRITE|VFS_O_CREATE|VFS_O_EXCL));
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
    CHECK(((!strcmp(source, "launcher.ini.tmp") && !strcmp(target, "launcher.ini")) ||
           (!strcmp(source, "dock.ini.tmp") && !strcmp(target, "dock.ini")) ||
           (!strcmp(source, "taskbar.ini.tmp") && !strcmp(target, "taskbar.ini"))) && layoutSyncs == 1);
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
static CalendarClock fixtureClock{{{2026, 11}, 20}, 5, 57};
CalendarClock RtcReadTaskbarClock() { return fixtureClock; }
static ChromeTrayState fixtureTray{};
static TrayInput fixtureLastTrayInput{};
static unsigned fixtureTrayEvents{};
const ChromeTrayState& ShellServiceTrayState() { return fixtureTray; }
void ShellServiceQueueTrayInput(const TrayInput& event) {
    CHECK(fixtureTray.itemCount);
    ++fixtureTrayEvents;
    fixtureLastTrayInput = event;
}
bool ThermalReadSnapshot(ThermalSnapshot&) { return false; }
ThermalBalanceStatus ThermalBalanceRead() { return {}; }
static uint32_t fixtureDiskVolumes;
void DesktopDiskWorkerStart() {}
static uint32_t fixtureCpuCount = 4;
static DesktopDetailedStats fixtureDetailedStats{};
void DesktopStatsWorkerStart() {}
uint32_t DesktopStatsCoreCount() { return fixtureCpuCount; }
void DesktopStatsRead(DesktopStatsPanel, DesktopDetailedStats& out) { out = fixtureDetailedStats; }
uint32_t DesktopDiskVolumeCount() { return fixtureDiskVolumes; }
uint32_t DesktopDiskReadPage(uint32_t first, uint32_t capacity, DesktopDiskPage& out) {
    out.total = fixtureDiskVolumes;
    out.first = DesktopDiskPageFirst(first, out.total, capacity);
    out.count = out.total - out.first < capacity ? out.total - out.first : capacity;
    out.measured = true;
    return out.first;
}
uint32_t AudioOutputControl() { return BROOK_AUDIO_DEFAULT; }
bool AudioSetUserOutputControl(uint32_t, bool) { std::abort(); }
uint32_t testScreenW=1024,testScreenH=768;
extern "C" void CompositorGetPhysDims(uint32_t* w,uint32_t* h) { if(w)*w=testScreenW;if(h)*h=testScreenH; }
// Multi-output D0: the framebuffer is the one output.
void CompositorGetOutputs(OutputLayout& out) {
    out=OutputLayout{};uint32_t w=0,h=0;CompositorGetPhysDims(&w,&h);
    if(!w||!h)return;
    out.count=1;out.outputs[0].connected=out.outputs[0].enabled=true;out.outputs[0].w=w;out.outputs[0].h=h;
}
bool CompositorGetOutputRect(uint32_t index,OutputRect* rect) {
    OutputLayout l;CompositorGetOutputs(l);if(!l.enabled(index))return false;if(rect)*rect=l.rect(index);return true;
}
OutputRect CompositorPrimaryOutputRect() { OutputLayout l;CompositorGetOutputs(l);return l.rect(0); }
}
extern "C" void KernelPanic(const char*,...) { std::abort(); }

// 22 Nov (Dell int88): six large windows (69.2 MB of client buffers) made
// every per-frame query fail with E2BIG under the old fixed 64 MiB total, so
// the renderer retried forever. Synthetic layers; the sink counts bytes and
// never dereferences the fake destinations.
static void FillBudgetLayers(brook::WmDesktopLayers& layers) {
    static const uint32_t sizes[][2]={{880,1360},{1600,1200},{1502,1332},{1776,1376},{2896,1378},{3200,1800}};
    layers={};layers.count=6;
    for(uint32_t i=0;i<6;++i){auto& l=layers.layers[i];
        l.window=i+1;l.incarnation=100+i;l.generation=7;l.width=l.stride=l.clientWidth=sizes[i][0];
        l.height=l.clientHeight=sizes[i][1];l.bytes=uint64_t(l.stride)*l.height*4;l.upscale=1;l.opacity=255;
        l.clientX=int32_t(40*i);l.clientY=int32_t(30*i);}
    // Quake II: fullscreen at the origin, topmost.
    layers.layers[5].clientX=layers.layers[5].clientY=0;
}
static void BudgetDestinations(const brook::WmDesktopLayers& layers,BrookDesktopLayers& wire) {
    uint64_t next=0x100000000ull;
    for(uint32_t i=0;i<layers.count;++i){wire.layers[i].pixels=next;wire.layers[i].capacity=layers.layers[i].bytes;
        next+=(layers.layers[i].bytes+0xfffff)&~0xfffffull;}
}
static void TestDesktopLayerBudget() {
    using namespace brook;
    static WmDesktopLayers layers;FillBudgetLayers(layers);
    uint64_t total=0;for(uint32_t i=0;i<layers.count;++i)total+=layers.layers[i].bytes;
    CHECK(total>64ull*1024*1024 && total<70000000ull);          // the 69.2 MB of the report
    BrookDesktopLayers wire{};wire.version=BROOK_DESKTOP_LAYERS_VERSION;
    uint64_t copied=0;uint32_t touched=0;
    auto counting=[&](const WmDesktopLayer& l,uint64_t,uint64_t,uint64_t bytes){copied+=bytes;touched|=1u<<(l.window-1);return true;};
    auto alwaysCurrent=[](const WmDesktopLayers&){return true;};
    CHECK(!ExportDesktopLayers(layers,wire,false,counting,alwaysCurrent,65536) && wire.count==6 && !copied);
    BudgetDestinations(layers,wire);
    CHECK(!ExportDesktopLayers(layers,wire,true,counting,alwaysCurrent,65536) && copied==total && touched==0x3f);
    // The bound scales with the output: 32 whole 3200x1800 outputs.
    CHECK(DesktopLayerCopyLimit(3200,1800)==3200ull*1800*4*32 && DesktopLayerCopyLimit(0,0)==4096ull*2160*4*32);
    CHECK(DesktopLayerCopyLimit(3200,1800)>10*total);
    const BrookDesktopLayers held=wire;
    // Only copied bytes count: under a 30 MB budget a full copy is E2BIG
    // (nothing copied), while keeping the five idle windows copies only
    // Quake II's 23 MB and succeeds.
    DesktopLayerExportOptions tight{};tight.copyLimit=30000000ull;
    DesktopCaptureCounters counters{};copied=touched=0;
    CHECK(ExportDesktopLayers(layers,wire,true,counting,alwaysCurrent,65536,0,0,&counters,tight)==-7 && !copied && !counters.calls);
    wire=held;
    CHECK(!ExportDesktopLayers(layers,wire,true,counting,alwaysCurrent,65536,0,0x1f,&counters,tight));
    CHECK(copied==layers.layers[5].bytes && touched==0x20 && counters.keeps==5 && counters.captures==1);
    CHECK(counters.bytesKept==total-layers.layers[5].bytes);
    for(uint32_t i=0;i<5;++i)CHECK(wire.layers[i].flags&BROOK_DESKTOP_LAYER_KEPT);
    // Skipped layers (the renderer's degrade path) do not count either.
    wire=held;for(uint32_t i=0;i<5;++i)wire.layers[i].pixels=wire.layers[i].capacity=0;copied=0;
    CHECK(!ExportDesktopLayers(layers,wire,true,counting,alwaysCurrent,65536,0x1f,0,nullptr,tight) && copied==layers.layers[5].bytes);
    // A kept layer that published again is copied and counts.
    wire=held;++layers.layers[0].generation;copied=0;
    CHECK(!ExportDesktopLayers(layers,wire,true,counting,alwaysCurrent,65536,0,0x1f,nullptr,tight));
    CHECK(copied==layers.layers[5].bytes+layers.layers[0].bytes && !(wire.layers[0].flags&BROOK_DESKTOP_LAYER_KEPT));
    --layers.layers[0].generation;
    // Corrupt sizes: beyond the scene maximum, or larger than the backing.
    wire=held;layers.layers[2].stride=GPU_SCENE_MAX_DIMENSION+1;layers.layers[2].bytes=~uint64_t{0}>>8;
    CHECK(ExportDesktopLayers(layers,wire,false,counting,alwaysCurrent,65536)==-7);
    layers.layers[2].stride=layers.layers[2].width;layers.layers[2].height=GPU_SCENE_MAX_DIMENSION+1;
    CHECK(ExportDesktopLayers(layers,wire,false,counting,alwaysCurrent,65536)==-7);
    FillBudgetLayers(layers);layers.layers[3].bytes-=4;
    CHECK(ExportDesktopLayers(layers,wire,false,counting,alwaysCurrent,65536)==-7);
    // The largest legal layer (16384x16384) is accepted by a query.
    FillBudgetLayers(layers);layers.layers[0].width=layers.layers[0].stride=layers.layers[0].height=GPU_SCENE_MAX_DIMENSION;
    layers.layers[0].bytes=DesktopLayerMaxBytes;wire=held;
    CHECK(!ExportDesktopLayers(layers,wire,false,counting,alwaysCurrent,65536) && wire.layers[0].bytes==DesktopLayerMaxBytes);
    std::puts("WINDOW_SURFACES PASS: 69.2 MB desktop layer export, kept/skipped accounting, corrupt sizes");
}
static void TestDesktopLayerOcclusion() {
    using namespace brook;
    static WmDesktopLayers layers;FillBudgetLayers(layers);
    BrookDesktopLayers wire{};wire.version=BROOK_DESKTOP_LAYERS_VERSION;
    auto alwaysCurrent=[](const WmDesktopLayers&){return true;};
    uint32_t touched=0;
    auto counting=[&](const WmDesktopLayer& l,uint64_t,uint64_t,uint64_t){touched|=1u<<(l.window-1);return true;};
    CHECK(!ExportDesktopLayers(layers,wire,false,counting,alwaysCurrent,65536));
    BudgetDestinations(layers,wire);const BrookDesktopLayers request=wire;
    DesktopLayerExportOptions cull{};cull.cullOccluded=true;
    // A native fullscreen window (Quake II) has straight alpha: never occludes.
    CHECK(!ExportDesktopLayers(layers,wire,true,counting,alwaysCurrent,65536,0,0,nullptr,cull) && touched==0x3f);
    for(uint32_t i=0;i<6;++i)CHECK(!(wire.layers[i].flags&BROOK_DESKTOP_LAYER_OCCLUDED));
    // A legacy (always opaque) 1600x900 framebuffer upscaled 2x to cover the
    // 3200x1800 output: every layer beneath is occluded, not copied, not kept.
    auto& top=layers.layers[5];top.legacyOpaque=1;top.width=top.stride=1600;top.height=900;top.upscale=2;
    top.bytes=uint64_t(top.stride)*top.height*4;
    wire=request;wire.layers[5].capacity=top.bytes;touched=0;DesktopCaptureCounters counters{};
    CHECK(!ExportDesktopLayers(layers,wire,true,counting,alwaysCurrent,65536,0,0,&counters,cull) && touched==0x20);
    for(uint32_t i=0;i<5;++i)CHECK((wire.layers[i].flags&BROOK_DESKTOP_LAYER_OCCLUDED) && !(wire.layers[i].flags&BROOK_DESKTOP_LAYER_KEPT));
    CHECK(!(wire.layers[5].flags&BROOK_DESKTOP_LAYER_OCCLUDED) && counters.occluded==5 && counters.captures==1);
    // Occluded is reported instead of KEPT even for a keepable layer.
    const BrookDesktopLayers occludedReply=wire;
    CHECK(!ExportDesktopLayers(layers,wire,true,counting,alwaysCurrent,65536,0,0x1f,nullptr,cull));
    for(uint32_t i=0;i<5;++i)CHECK(wire.layers[i].flags==BROOK_DESKTOP_LAYER_OCCLUDED);
    // Without the option nothing is culled (older renderers).
    wire=request;wire.layers[5].capacity=top.bytes;touched=0;
    CHECK(!ExportDesktopLayers(layers,wire,true,counting,alwaysCurrent,65536) && touched==0x3f);
    CHECK(ExportDesktopLayers(layers,wire,false,counting,alwaysCurrent,65536,0,0,nullptr,cull)==-22);
    // Partial cover: a window extending past the occluder is copied.
    wire=occludedReply;layers.layers[3].clientX=3200-100;touched=0;
    CHECK(!ExportDesktopLayers(layers,wire,true,counting,alwaysCurrent,65536,0,0,nullptr,cull) && touched==(0x20|0x08));
    CHECK(!(wire.layers[3].flags&BROOK_DESKTOP_LAYER_OCCLUDED) && (wire.layers[2].flags&BROOK_DESKTOP_LAYER_OCCLUDED));
    layers.layers[3].clientX=120;
    // Translucent or not upscaled to cover: no occlusion.
    top.opacity=254;wire=occludedReply;touched=0;
    CHECK(!ExportDesktopLayers(layers,wire,true,counting,alwaysCurrent,65536,0,0,nullptr,cull) && touched==0x3f);
    top.opacity=255;top.upscale=1;wire=occludedReply;touched=0;
    CHECK(!ExportDesktopLayers(layers,wire,true,counting,alwaysCurrent,65536,0,0,nullptr,cull) && touched==0x3f);
    // Content wider than the client rectangle is clipped before the test:
    // a 2x buffer clipped to a 1000x1400 client covers only that.
    top.upscale=2;top.clientWidth=1000;top.clientHeight=1400;wire=occludedReply;touched=0;
    CHECK(!ExportDesktopLayers(layers,wire,true,counting,alwaysCurrent,65536,0,0,nullptr,cull));
    CHECK((wire.layers[0].flags&BROOK_DESKTOP_LAYER_OCCLUDED) && !(wire.layers[4].flags&BROOK_DESKTOP_LAYER_OCCLUDED));
    // Only layers above can occlude.
    std::swap(layers.layers[0],layers.layers[5]);wire=request;BudgetDestinations(layers,wire);touched=0;
    CHECK(!ExportDesktopLayers(layers,wire,true,counting,alwaysCurrent,65536,0,0,nullptr,cull) && touched==0x3f);
    std::puts("WINDOW_SURFACES PASS: occluded desktop layers");
}

int main() {
    using namespace brook;
    TestDesktopLayerBudget();
    TestDesktopLayerOcclusion();
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
    CHECK(WmPushWmEvent(&window,WM_EVT_RESIZED,640,360,WM_RESIZE_RESIZING));
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
        // The launcher still temporarily covers the taskbar popups.
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

        const auto volume = GetTaskbarLayout(sw, sh).volume;
        CHECK(WmVolumeLeftPress(volume.x, volume.y, sw, sh));
        const auto volumePin = WmVolumePopupLayout(sw, sh).pin;
        CHECK(WmVolumeLeftPress(volumePin.x, volumePin.y, sw, sh));
        CHECK(WmTaskbarPopupPinned() && !WmTaskbarPopupDismissible());
        CHECK(WmTaskbarPopupPointerOwned(volumePin.x, volumePin.y, sw, sh));
        CHECK(!WmTaskbarPopupPointerOwned(10, 10, sw, sh));
        CHECK(!WmVolumeLeftPress(10, 10, sw, sh) && WmVolumePopupVisible());
        CHECK(WmTrayFlyoutPress(tray.x, tray.y, sw, sh) && WmTrayFlyoutVisible());
        const auto net = WmPlacedTaskbarPopup(TaskbarPopup::Network, sw, sh);
        const auto placedVolume = WmPlacedTaskbarPopup(TaskbarPopup::Volume, sw, sh);
        CHECK(net.w && placedVolume.w && placedVolume.x + int(placedVolume.w) < net.x);
        CHECK(placedVolume.popupShiftX);
        const auto netPin = ChromeTrayFlyoutPinRect(WmChromeStyle(), {net.x, net.y, int(net.w), int(net.h)});
        CHECK(WmTrayFlyoutPress(netPin.x, netPin.y, sw, sh));
        CHECK(g_taskbarPopupPin.Is(TaskbarPopup::Network) && g_taskbarPopupPin.Is(TaskbarPopup::Volume));
        CHECK(!WmTaskbarPopupDismissible());
        constexpr uint32_t narrow = 800;
        const auto narrowClock = GetTaskbarLayout(narrow, sh).clock;
        CHECK(WmTrayFlyoutPress(narrowClock.x, narrowClock.y, narrow, sh) && WmTaskbarPopupDismissible());
        const auto calendar = WmPlacedTaskbarPopup(TaskbarPopup::Calendar, narrow, sh);
        CHECK(calendar.w && WmPlacedTaskbarPopup(TaskbarPopup::Network, narrow, sh).w);
        CHECK(!WmPlacedTaskbarPopup(TaskbarPopup::Volume, narrow, sh).w); // overflow keeps the pin
        CHECK(g_taskbarPopupPin.Is(TaskbarPopup::Volume));
        WmTaskbarPopupDismiss();
        CHECK(WmPlacedTaskbarPopup(TaskbarPopup::Volume, sw, sh).w);
        const auto activeNet = WmPlacedTaskbarPopup(TaskbarPopup::Network, sw, sh);
        const auto activeNetPin = ChromeTrayFlyoutPinRect(WmChromeStyle(),
            {activeNet.x, activeNet.y, int(activeNet.w), int(activeNet.h)});
        CHECK(WmTrayFlyoutPress(activeNetPin.x, activeNetPin.y, sw, sh));
        WmTaskbarPopupDismiss();
        CHECK(!WmTrayFlyoutVisible() && g_taskbarPopupPin.Is(TaskbarPopup::Volume));
        CHECK(WmVolumePopupVisible());
        g_launcherLoaded = true;
        WmLauncherToggle();
        CHECK(WmLauncherVisible() && WmTaskbarPopupPinned());
        CHECK(kinds(s) >= 2 && s[1].kind == WmShellSurfaceKind::Launcher);
        CHECK(!WmVolumeLeftPress(volumePin.x, volumePin.y, sw, sh));
        WmLauncherToggle();
        g_launcherLoaded = false;
        CHECK(!WmLauncherVisible() && kinds(s) >= 2 && s[1].kind == WmShellSurfaceKind::VolumePopup);
        static ChromeShellModel pinSnapshot{};
        CHECK(WmCaptureShellModel(s[1], sw, sh, 0, -1, -1, pinSnapshot) && pinSnapshot.volume.pinned);
        CHECK(WmVolumeLeftPress(volumePin.x, volumePin.y, sw, sh) && !WmTaskbarPopupPinned());
        CHECK(WmTaskbarPopupDismissible());
        CHECK(WmVolumeLeftPress(10, 10, sw, sh) && !WmVolumePopupVisible());

        for (const auto anchor : {tray, GetTaskbarLayout(sw, sh).metrics[6]}) {
            if (anchor.w == 0) continue; // the synthetic host has no thermal sensor
            CHECK(WmTrayFlyoutPress(anchor.x, anchor.y, sw, sh));
            const auto panel = WmTrayFlyoutLayout(sw, sh, g_trayFlyout);
            const auto pin = ChromeTrayFlyoutPinRect(WmChromeStyle(), panel);
            CHECK(WmTrayFlyoutPress(pin.x, pin.y, sw, sh));
            CHECK(WmTaskbarPopupPinned() && !WmTrayFlyoutPress(10, 10, sw, sh));
            CHECK(!WmTaskbarPopupPointerOwned(10, 10, sw, sh));
            CHECK(kinds(s) >= 2 && WmCaptureShellModel(s[1], sw, sh, 0, -1, -1, pinSnapshot));
            CHECK(pinSnapshot.kind == ChromeShellKind::TrayFlyout && pinSnapshot.popupPinned);
            CHECK(WmTrayFlyoutPress(pin.x, pin.y, sw, sh) && !WmTaskbarPopupPinned());
            CHECK(WmTrayFlyoutPress(10, 10, sw, sh) && !WmTrayFlyoutVisible());
        }
        WmCyclePerformanceHud();
        WmCyclePerformanceHud();
        const auto hudPin = ChromePerformanceHudPinRect(WmChromeStyle(), WmPerformanceHudLayout(sw, sh));
        CHECK(WmPerformanceHudPress(hudPin.x, hudPin.y, sw, sh));
        CHECK(WmTaskbarPopupPinned() && !WmPerformanceHudPress(10, 10, sw, sh));
        WmCyclePerformanceHud();
        CHECK(g_hudMode == WmHudMode::Panel);
        CHECK(WmPerformanceHudPress(hudPin.x, hudPin.y, sw, sh) && !WmTaskbarPopupPinned());
        WmCyclePerformanceHud();

        // The disk icon keeps the old bit mask, scrolls full lists, and
        // uses the same click-through and occlusion rules as other pins.
        const auto metrics = WmGetTaskbarMetrics();
        CHECK(WmSetTaskbarMetrics(metrics | BROOK_TASKBAR_DISK));
        const auto disk = GetTaskbarLayout(sw, sh).metrics[2];
        CHECK(disk.w == WmMetric(44));
        fixtureDiskVolumes = 23;
        CHECK(WmTrayFlyoutPress(disk.x + 1, disk.y + 1, sw, sh) && WmDiskFlyoutSelected());
        const auto diskPopup = WmDiskPopupLayout(sw, sh);
        CHECK(diskPopup.pageRows > 0 && diskPopup.pageRows < fixtureDiskVolumes);
        CHECK(WmTrayFlyoutPress(diskPopup.pin.x, diskPopup.pin.y, sw, sh) && WmTaskbarPopupPinned());
        CHECK(!WmTrayFlyoutPress(10, 10, sw, sh));
        const auto rowX = diskPopup.rows.x, rowY = diskPopup.rows.y;
        CHECK(WmDiskFlyoutScroll(rowX, rowY, -60, sw, sh) && g_diskPopupFirst == 0);
        CHECK(WmDiskFlyoutScroll(rowX, rowY, -60, sw, sh) && g_diskPopupFirst == 1);
        CHECK(WmTrayFlyoutPress(diskPopup.next.x, diskPopup.next.y, sw, sh));
        CHECK(g_diskPopupFirst == 1 + diskPopup.pageRows);
        CHECK(kinds(s) >= 2 && WmCaptureShellModel(s[1], sw, sh, 0, -1, -1, pinSnapshot));
        CHECK(pinSnapshot.diskPopup && pinSnapshot.disk.total == 23 && pinSnapshot.disk.first == g_diskPopupFirst);
        fixtureDiskVolumes = 2; // Removal or resize clamps the scroll offset.
        CHECK(kinds(s) >= 2 && WmCaptureShellModel(s[1], sw, sh, 0, -1, -1, pinSnapshot));
        CHECK(pinSnapshot.disk.count == 2 && pinSnapshot.disk.first == 0);
        CHECK(WmSetTaskbarMetrics(metrics)); // The old anchor can disappear.
        const auto fallback = WmDiskPopupLayout(sw, sh);
        CHECK(fallback.panel.w > 0 && WmTrayFlyoutPress(fallback.pin.x, fallback.pin.y, sw, sh));
        CHECK(!WmTaskbarPopupPinned() && WmTrayFlyoutPress(10, 10, sw, sh) && !WmDiskFlyoutSelected());
        fixtureDiskVolumes = 0;

        constexpr unsigned statMetrics[] = {0, 1, 4, 5};
        constexpr DesktopStatsPanel statPanels[] = {DesktopStatsPanel::CpuMemory, DesktopStatsPanel::CpuMemory,
            DesktopStatsPanel::Compositor, DesktopStatsPanel::App};
        const uint32_t detailMask = BROOK_TASKBAR_CPU | BROOK_TASKBAR_MEMORY | BROOK_TASKBAR_COMPOSITOR | BROOK_TASKBAR_APP;
        CHECK(WmSetTaskbarMetrics(detailMask));
        CHECK(!WmGetProMode());
        fixtureDetailedStats.tasksMeasured = true;
        fixtureDetailedStats.tasks[0].ticks = 40;
        fixtureDetailedStats.tasks[1].ticks = 2;
        for (unsigned i = 0; i < 4; ++i) {
            fixtureCpuCount = DetailedCoreLimit;
            const auto anchor = GetTaskbarLayout(sw, sh).metrics[statMetrics[i]];
            CHECK(anchor.w > 0 && WmTrayFlyoutPress(anchor.x, anchor.y, sw, sh));
            CHECK(WmDetailedStatsSelected() == statPanels[i]);
            auto popup = WmStatsPopupLayout(sw, sh, statPanels[i]);
            CHECK(popup.panel.w > 0 && popup.panel.y >= 0);
            CHECK(WmTrayFlyoutPress(popup.pin.x, popup.pin.y, sw, sh));
            CHECK(WmTaskbarPopupPinned() && !WmTaskbarPopupPointerOwned(10, 10, sw, sh));
            CHECK(!WmTrayFlyoutPress(10, 10, sw, sh));
            if (statPanels[i] == DesktopStatsPanel::CpuMemory) {
                CHECK(!popup.expanded && !popup.coreRows && popup.modeToggle.w > 0);
                CHECK(WmTrayFlyoutPress(popup.modeToggle.x, popup.modeToggle.y, sw, sh));
                CHECK(WmTaskbarPopupPinned());
                popup = WmStatsPopupLayout(sw, sh, statPanels[i]);
                CHECK(popup.expanded && popup.coreRows > 0 && popup.next.w > 0 && popup.scrollLimit == 0);
                CHECK(popup.taskRows == 2);
                fixtureDetailedStats.tasks[1].ticks = 0;
                const auto shorter = WmStatsPopupLayout(sw, sh, DesktopStatsPanel::CpuMemory);
                CHECK(shorter.taskRows == 1 && shorter.panel.h + popup.rowHeight == popup.panel.h);
                fixtureDetailedStats.tasks[1].ticks = 2;
                const auto otherAnchor = GetTaskbarLayout(sw, sh).metrics[statMetrics[1 - i]];
                CHECK(WmTrayFlyoutPress(otherAnchor.x, otherAnchor.y, sw, sh));
                CHECK(WmDetailedStatsSelected() == DesktopStatsPanel::CpuMemory && WmTaskbarPopupPinned());
                CHECK(WmStatsFlyoutScroll(popup.body.x, popup.body.y, -60, sw, sh) && g_cpuFirstRow == 0);
                CHECK(WmStatsFlyoutScroll(popup.body.x, popup.body.y, -60, sw, sh) && g_cpuFirstRow == 1);
                CHECK(WmTrayFlyoutPress(popup.next.x, popup.next.y, sw, sh));
                CHECK(g_cpuFirstRow == 1 + popup.coreRows);
                const uint32_t shortHeight = uint32_t(popup.panel.h - int(popup.coreRows + 1) * popup.rowHeight) +
                    (sh - WmDesktopHeight(sh)) + 2 * WmMetric(8);
                const auto shortPopup = WmStatsPopupLayout(sw, shortHeight, DesktopStatsPanel::CpuMemory);
                CHECK(shortPopup.coreRows == 1 && shortPopup.scrollLimit > 0);
                CHECK(shortPopup.panel.y >= 0 && shortPopup.panel.y + shortPopup.panel.h <= int(shortHeight));
                CHECK(WmTrayFlyoutPress(shortPopup.scrollDown.x, shortPopup.scrollDown.y, sw, shortHeight));
                CHECK(shortPopup.scrollCount > 3 && g_statsContentOffset == shortPopup.scrollStops[3]);
                CHECK(WmStatsFlyoutScroll(shortPopup.body.x, shortPopup.body.y, -12000, sw, shortHeight));
                CHECK(g_statsContentOffset == shortPopup.scrollLimit);
                fixtureCpuCount = 4;
            }
            CHECK(kinds(s) >= 2 && WmCaptureShellModel(s[1], sw, sh, 0, -1, -1, pinSnapshot));
            CHECK(pinSnapshot.statsPanel == statPanels[i] && pinSnapshot.popupPinned);
            CHECK(pinSnapshot.firstCoreRow == 0);
            if (statPanels[i] == DesktopStatsPanel::CpuMemory) {
                const auto details = WmStatsPopupLayout(sw, sh, statPanels[i]);
                CHECK(WmTrayFlyoutPress(details.modeToggle.x, details.modeToggle.y, sw, sh));
                CHECK(WmTaskbarPopupPinned() && !WmStatsPopupLayout(sw, sh, statPanels[i]).expanded);
            }
            if (i == 3) CHECK(WmSetTaskbarMetrics(BROOK_TASKBAR_CPU)); // the pinned panel is still reachable
            const auto resized = WmStatsPopupLayout(sw, sh, statPanels[i]);
            CHECK(WmTrayFlyoutPress(resized.pin.x, resized.pin.y, sw, sh) && !WmTaskbarPopupPinned());
            CHECK(WmTrayFlyoutPress(10, 10, sw, sh) && !WmTrayFlyoutVisible());
        }
        WmSetProMode(true);
        CHECK(WmGetProMode());
        const auto cpuAnchor = GetTaskbarLayout(sw, sh).metrics[0];
        CHECK(WmTrayFlyoutPress(cpuAnchor.x, cpuAnchor.y, sw, sh));
        const auto details = WmStatsPopupLayout(sw, sh, DesktopStatsPanel::CpuMemory);
        CHECK(details.expanded);
        CHECK(WmTrayFlyoutPress(details.modeToggle.x, details.modeToggle.y, sw, sh));
        CHECK(!WmStatsPopupLayout(sw, sh, DesktopStatsPanel::CpuMemory).expanded);
        CHECK(WmTrayFlyoutPress(10, 10, sw, sh) && !WmTrayFlyoutVisible());
        CHECK(WmTrayFlyoutPress(cpuAnchor.x, cpuAnchor.y, sw, sh));
        CHECK(WmStatsPopupLayout(sw, sh, DesktopStatsPanel::CpuMemory).expanded);
        CHECK(WmTrayFlyoutPress(10, 10, sw, sh) && !WmTrayFlyoutVisible());
        WmSetProMode(false);

        const auto collapsed = ChromeStatsPopupLayoutFor(WmChromeStyle(), {400, 100, 30, 20}, sw, 100,
                                                            DesktopStatsPanel::CpuMemory, 4, true);
        CHECK(collapsed.panel.w > 0 && collapsed.body.h == 0 && collapsed.pin.w > 0);
        const auto* originalMetrics = g_desktopMetrics;
        for (const auto* scale : {&DesktopMetric100, &DesktopMetric150, &DesktopMetric200}) {
            g_desktopMetrics = scale;
            const auto tall = ChromeStatsPopupLayoutFor(WmChromeStyle(), {400, 2400, 30, 20}, sw, 2400,
                                                        DesktopStatsPanel::CpuMemory, 64, true);
            const int bottom = tall.panel.h - int(tall.coreRows + 1) * tall.rowHeight + 2 * scale->pixels(8);
            const auto layout = ChromeStatsPopupLayoutFor(WmChromeStyle(), {400, bottom, 30, 20}, sw, bottom,
                                                          DesktopStatsPanel::CpuMemory, 64, true, 10000);
            CHECK(layout.coreRows == 1 && layout.scrollLimit > 0 && layout.contentOffset == layout.scrollLimit);
            CHECK(layout.panel.y >= 0 && layout.panel.y + layout.panel.h <= bottom);
            CHECK(layout.body.y + layout.body.h <= layout.scrollDown.y);
            CHECK(layout.body.y - layout.contentOffset + layout.cpuHeight < layout.body.y + layout.body.h);
            CHECK(layout.scrollCount > 3 && layout.scrollStops[0] == 0);
            for (uint32_t i = 1; i < layout.scrollCount; ++i)
                CHECK(layout.scrollStops[i] > layout.scrollStops[i - 1]);
            const auto insideRow = ChromeStatsPopupLayoutFor(WmChromeStyle(), {400, bottom, 30, 20}, sw, bottom,
                                                             DesktopStatsPanel::CpuMemory, 64, true,
                                                             layout.scrollStops[2] + 1);
            CHECK(insideRow.scrollIndex == 2 && insideRow.contentOffset == layout.scrollStops[2]);
        }
        g_desktopMetrics = originalMetrics;
        fixtureDetailedStats = {};
        CHECK(WmSetTaskbarMetrics(metrics));

        const CalendarClock clockAtStart = fixtureClock;
        const auto barClock = GetTaskbarLayout(sw, sh);
        CHECK(strcmp(barClock.clockText, "Fri 05:57") == 0 && strcmp(barClock.clockDate, "20 Nov") == 0);
        fixtureClock = {{{2026, 12}, 1}, 23, 59};
        const auto nextClock = GetTaskbarLayout(sw, sh);
        CHECK(strcmp(nextClock.clockText, "Tue 23:59") == 0 && strcmp(nextClock.clockDate, "01 Dec") == 0);
        CHECK(nextClock.clock.x == barClock.clock.x && nextClock.clock.w == barClock.clock.w);
        const auto clock = nextClock.clock;
        CHECK(WmTrayFlyoutPress(clock.x + 2, clock.y + 2, sw, sh) && WmTrayFlyoutVisible());
        auto cal = WmCalendarPopupLayout(sw, sh, fixtureClock.date);
        CHECK(cal.pin.w > 0 && cal.next.w > 0 && cal.grid.w > 0);
        CHECK(WmTrayFlyoutPress(cal.next.x, cal.next.y, sw, sh));
        CHECK(kinds(s) >= 2 && WmCaptureShellModel(s[1], sw, sh, 0, -1, -1, pinSnapshot));
        CHECK(pinSnapshot.calendar && (pinSnapshot.calendarMonth == CalendarMonth{2027, 1}));
        CHECK(WmTrayFlyoutPress(cal.previous.x, cal.previous.y, sw, sh));
        CHECK(!CalendarMonthValid(g_calendarMonth)); // returning to today follows midnight rollover
        fixtureClock = {{{2027, 1}, 1}, 0, 0};
        CHECK(kinds(s) >= 2 && WmCaptureShellModel(s[1], sw, sh, 0, -1, -1, pinSnapshot));
        CHECK((pinSnapshot.calendarMonth == CalendarMonth{2027, 1}) && pinSnapshot.calendarToday.day == 1);
        CHECK(WmTrayFlyoutPress(cal.pin.x, cal.pin.y, sw, sh) && WmTaskbarPopupPinned());
        CHECK(!WmTrayFlyoutPress(10, 10, sw, sh) && WmTrayFlyoutVisible());
        g_launcherLoaded = true;
        WmLauncherToggle();
        CHECK(WmLauncherVisible() && kinds(s) >= 2 && s[1].kind == WmShellSurfaceKind::Launcher);
        CHECK(!WmTrayFlyoutPress(cal.pin.x, cal.pin.y, sw, sh) && WmTaskbarPopupPinned());
        WmLauncherToggle();
        g_launcherLoaded = false;
        CHECK(!WmLauncherVisible() && kinds(s) >= 2 && s[1].kind == WmShellSurfaceKind::CalendarFlyout);
        const auto tiny = WmCalendarPopupLayout(sw, 144, fixtureClock.date);
        CHECK(tiny.pin.w > 0 && !tiny.grid.w);
        CHECK(WmTrayFlyoutPress(tiny.pin.x, tiny.pin.y, sw, 144) && !WmTaskbarPopupPinned());
        CHECK(WmTrayFlyoutPress(10, 10, sw, sh) && !WmTrayFlyoutVisible());
        fixtureClock = {};
        const auto invalidClock = GetTaskbarLayout(sw, sh);
        CHECK(invalidClock.clock.w == barClock.clock.w && strcmp(invalidClock.clockText, "--:--") == 0);
        CHECK(!invalidClock.clockDate[0]);
        CHECK(WmTrayFlyoutPress(clock.x, clock.y, sw, sh) && WmTrayFlyoutVisible());
        cal = WmCalendarPopupLayout(sw, sh, fixtureClock.date);
        CHECK(cal.unavailable.w > 0 && !cal.next.w && !cal.grid.w);
        CHECK(WmTrayFlyoutPress(10, 10, sw, sh) && !WmTrayFlyoutVisible());
        fixtureClock = clockAtStart;

        WmStrCopy(g_buildPayloadId, "test-full-build-payload", sizeof(g_buildPayloadId));
        const char buildMarker[] = "BROOK_BUILD_V1\n"
            "0123456789abcdef0123456789abcdef01234567\n2024-02-29T05:57:01Z\nclock\nclean\n";
        CHECK(ParseDesktopBuildProvenance(buildMarker, sizeof(buildMarker) - 1, g_buildProvenance));
        fixtureSlot.generation = 467;
        auto build = GetTaskbarLayout(sw, sh).build;
        CHECK(build.w > 0 && WmTrayFlyoutPress(build.x, build.y, sw, sh));
        CHECK(kinds(s) >= 2 && WmCaptureShellModel(s[1], sw, sh, 183561000, -1, -1, pinSnapshot));
        CHECK(pinSnapshot.buildPopup && pinSnapshot.buildInfo.generation == 467 &&
              pinSnapshot.buildInfo.uptimeSeconds == 183561 && !strcmp(pinSnapshot.buildInfo.id, g_buildPayloadId));
        CHECK(!strcmp(pinSnapshot.buildInfo.initrd.assembledUtc, "2024-02-29T05:57:01Z"));
        CHECK(!strcmp(pinSnapshot.buildInfo.kernelCommit, BuildGitCommit()));
        const auto buildPopup = pinSnapshot.buildLayout;
        CHECK(WmTrayFlyoutPress(buildPopup.pin.x, buildPopup.pin.y, sw, sh) && WmTaskbarPopupPinned());
        CHECK(!WmTrayFlyoutPress(10, 10, sw, sh) && !WmTaskbarPopupPointerOwned(10, 10, sw, sh));
        const auto shortBuild = WmBuildPopupLayout(sw, 180, WmBuildInformation(0));
        CHECK(shortBuild.next.w > 0 && shortBuild.pageRows < shortBuild.totalRows);
        CHECK(WmTrayFlyoutPress(shortBuild.next.x, shortBuild.next.y, sw, 180) && g_buildPopupFirst > 0);
        CHECK(WmTrayFlyoutPress(shortBuild.previous.x, shortBuild.previous.y, sw, 180) && !g_buildPopupFirst);
        CHECK(WmTrayFlyoutPress(buildPopup.pin.x, buildPopup.pin.y, sw, sh) && !WmTaskbarPopupPinned());
        CHECK(WmTrayFlyoutPress(10, 10, sw, sh) && !WmTrayFlyoutVisible());
        g_buildPayloadId[0] = 0;
        g_buildProvenance = {};
        fixtureSlot = {};

        fixtureTray.itemCount = fixtureTray.menuCount = 1;
        fixtureTray.menuOwner = fixtureTray.items[0].id = 11;
        fixtureTray.menu[0] = {4, "Open", true, false, false};
        WmShellTrayGeometry geometry{};
        WmGetShellTrayGeometry(sw, sh, 10, 10, geometry);
        CHECK(geometry.menu && !geometry.windowMenu);
        const auto menuPin = ChromeTrayMenuPinRect(WmChromeStyle(), geometry.popup);
        const auto row = ChromeTrayMenuRowRect(WmChromeStyle(), geometry.popup, 0, true);
        CHECK(menuPin.y + menuPin.h <= row.y);
        CHECK(WmShellTrayPointer(sw, sh, menuPin.x, menuPin.y, true, 0, 0));
        CHECK(WmTaskbarPopupPinned() && !WmTaskbarPopupDismissible());
        CHECK(!WmShellTrayPointer(sw, sh, 10, 10, true, 0, 0) && fixtureTrayEvents == 0);
        CHECK(WmShellTrayPointer(sw, sh, row.x, row.y, true, 0, 0));
        CHECK(fixtureLastTrayInput.kind == TrayInputKind::MenuSelectPinned && fixtureLastTrayInput.owner == 11);
        fixtureTray.items[0].id = 12; // service owner gone without a matching replacement
        WmGetShellTrayGeometry(sw, sh, 10, 10, geometry);
        CHECK(!geometry.menu && !WmTaskbarPopupPinned());
        CHECK(!WmShellTrayPointer(sw, sh, 10, 10, true, 0, 0) && fixtureTrayEvents == 1);
        fixtureTray = {};
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

        shutdownPending=true;
        CHECK(!press(LauncherTargetKind::ForceApps, 0, false));
        forceAppsOffered=true;
        WmCaptureLauncher(sw, sh, -1, -1, snapshot);
        CHECK(snapshot.shutdownActive && snapshot.forceApps);
        CHECK(press(LauncherTargetKind::ForceApps, 0, true) && !forceCloseCalls);
        CHECK(press(LauncherTargetKind::ForceApps, 0, false) && forceCloseCalls==1);
        shutdownPending=forceAppsOffered=false;

        // A failed shutdown shows its message and offers Retry and Force
        // power off instead of an inert Cancel shutdown.
        shutdownPending=shutdownFailed=true;
        WmCaptureLauncher(sw, sh, -1, -1, snapshot);
        CHECK(snapshot.shutdownActive && snapshot.shutdownFailed && snapshot.retryShutdown && snapshot.forcePowerOff);
        CHECK(!strcmp(snapshot.text, failedStatus));
        {
            const auto& frame = LauncherFrame(sw, sh, -1, -1);
            bool retry=false,force=false,cancel=false;
            for (uint32_t i = 0; i < frame.targetCount; ++i) {
                retry |= frame.targets[i].target.kind == LauncherTargetKind::RetryShutdown;
                force |= frame.targets[i].target.kind == LauncherTargetKind::ForcePowerOff;
                cancel |= frame.targets[i].target.kind == LauncherTargetKind::CancelShutdown;
            }
            CHECK(retry && force && !cancel && frame.notice.h > 0 && frame.notice.w > frame.footer.w / 2);
        }
        CHECK(press(LauncherTargetKind::RetryShutdown, 0, false) && retryCalls==1 && !forcePowerOffCalls);
        CHECK(press(LauncherTargetKind::ForcePowerOff, 0, false) && forcePowerOffCalls==1);
        shutdownPending=shutdownFailed=false;

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

        const auto historyCount = g_launcherHistoryCount;
        const auto historyNext = g_launcherHistoryNext;
        CHECK(historyCount > 0);
        char* history = g_launcherHistory[(historyNext + LauncherUndoLimit - 1) % LauncherUndoLimit];
        const char first = history[0];
        history[0] = 0;
        testTick = 9000;
        LauncherFrame(sw, sh, -1, -1);
        CHECK(!g_launcherModel.UndoVisible()); // Ctrl+Z remains available after the notice expires
        WmLauncherKey(0x2c, 0, INPUT_MOD_CTRL);
        CHECK(g_launcherModel.Pinned(1) && g_launcherHistoryCount == historyCount && g_launcherHistoryNext == historyNext);
        history[0] = first;
        WmLauncherKey(0x2c, 0, INPUT_MOD_CTRL);
        CHECK(!g_launcherModel.Pinned(1) && g_launcherHistoryCount == historyCount - 1);

        // The newly published snapshot has the same visible apps, but its
        // raw item indices changed. Release must queue the app under the
        // pointer using the current guarded snapshot's item mapping.
        DesktopRect appRow{};
        const auto& frame = LauncherFrame(sw, sh, -1, -1);
        for (uint32_t i = 0; i < frame.targetCount; ++i) {
            const auto& target = frame.targets[i];
            if (target.target.kind == LauncherTargetKind::Row && target.target.index == 1) appRow = target.rect;
        }
        CHECK(appRow.w > 0);
        const int mx = appRow.x + appRow.w / 2, my = appRow.y + appRow.h / 2;
        CHECK(WmLauncherPress(mx, my, sw, sh, false) && g_launcherModel.PointerHeld());
        static LauncherSnapshot refreshed{};
        refreshed.generation = 2;
        refreshed.count = 3;
        refreshed.items[1] = catalog.items[0];
        refreshed.items[2] = catalog.items[1];
        WmStrCopy(refreshed.items[1].scriptPath, "/boot/WRONG.ELF", sizeof(refreshed.items[1].scriptPath));
        WmStrCopy(refreshed.items[2].scriptPath, "/boot/EXPECTED.ELF", sizeof(refreshed.items[2].scriptPath));
        g_launcherActive = &refreshed;
        g_launcherWorkerStarted = true; // the host fixture observes the launch queue instead of running it
        CHECK(WmLauncherGesture(mx, my, sw, sh, true));
        CHECK(!g_launcherModel.PointerHeld() && !g_launcherOpen);
        CHECK(g_launches.entries[0].phase == LaunchPhase::Queued);
        CHECK(!strcmp(g_launchRequests[0].command, "/boot/EXPECTED.ELF"));
        g_launcherWorkerStarted = g_launchBusy = false;
        g_launches.entries[0] = {};
        g_taskbarPageTarget = -1;
        g_launcherOpen = g_launcherLoaded = false;
        g_launcherActive = &g_emptyLauncher;
    }

    // Taskbar actions use stable catalog IDs, not the identical window titles
    // or an old window slot whose incarnation has changed.
    {
        constexpr uint32_t sw = 1024, sh = 768;
        static LauncherSnapshot catalog{};
        catalog.count = 2;
        catalog.generation = 12;
        auto& terminal = catalog.items[0];
        terminal.valid = true;
        WmStrCopy(terminal.appId, TerminalAppKey, sizeof(terminal.appId));
        WmStrCopy(terminal.title, "Terminal", sizeof(terminal.title));
        WmStrCopy(terminal.scriptPath, "/boot/SHORTCUTS/TERMINAL.RC", sizeof(terminal.scriptPath));
        auto& other = catalog.items[1];
        other.valid = other.isDesktopEntry = true;
        WmStrCopy(other.appId, "org.brook.Editor", sizeof(other.appId));
        WmStrCopy(other.title, "Editor", sizeof(other.title));
        CHECK(IdentifyLauncherItems(catalog));
        g_launcherModel.Init();
        g_launcherActive = &catalog;
        g_launcherModel.DesktopPins().SetDefault();
        LauncherSyncModel(catalog);
        const auto button = [&](int target) {
            const auto layout = GetTaskbarLayout(sw, sh);
            for (int i = 0; i < layout.count; ++i)
                if (layout.indices[i] == target) return layout.buttons[i];
            return DesktopRect{};
        };
        static ChromeShellModel taskbarModel{};
        const auto captureEntry = [&](int target) {
            const auto layout = GetTaskbarLayout(sw, sh);
            taskbarModel = {};
            WmCaptureTaskbar(sw, sh, 0, -1, -1, taskbarModel);
            CHECK(taskbarModel.taskbar.count == layout.count);
            for (int i = 0; i < layout.count; ++i)
                if (layout.indices[i] == target) return taskbarModel.entries[i];
            std::abort();
        };
        auto pin = button(TaskbarPinBase);
        CHECK(pin.w > 0 && WmTaskbarHitTest(pin.x, pin.y, sw, sh) == TaskbarPinBase);
        CHECK(!captureEntry(TaskbarPinBase).hasWindow);
        CHECK(WmShellTrayPointer(sw, sh, pin.x + 2, pin.y + 2, true, 1, 0));
        CHECK(g_taskbarContextMenu.open && g_taskbarContextMenu.pinned && g_taskbarContextMenu.window < 0);
        WmWindowMenuClose();

        auto* clients = static_cast<Process*>(std::calloc(3, sizeof(Process)));
        CHECK(clients);
        int ids[3]{};
        for (int i = 0; i < 3; ++i) {
            clients[i].pid = uint16_t(50 + i);
            ids[i] = WmCreateWindow(&clients[i], 90, 90, 600, 400, "Two identical titles");
            CHECK(ids[i] >= 0);
            auto& w = g_windows[ids[i]];
            CHECK(WmSetAppIdById(&clients[i], w.wmId, i < 2 ? TerminalAppKey : other.appId));
        }
        WmSetFocus(ids[1]);
        CHECK(button(ids[0]).w && !button(ids[1]).w && button(ids[2]).w);
        const auto pinnedRunning = captureEntry(TaskbarPinBase);
        const auto unpinnedRunning = captureEntry(ids[2]);
        CHECK(pinnedRunning.hasWindow && pinnedRunning.active);
        CHECK(unpinnedRunning.hasWindow && !unpinnedRunning.active);
        WmTaskbarPendingClick(TaskbarPinBase);
        CHECK(g_windows[ids[1]].minimized);
        WmSetFocus(ids[2]); // focus another app, then restore the newest Terminal
        WmTaskbarPendingClick(TaskbarPinBase);
        const int primary = TaskbarPrimaryWindow(catalog, terminal);
        CHECK(primary >= 0 && !g_windows[primary].minimized);

        // An app_id collision must not alias two pins or obscure a window.
        WmStrCopy(other.appId, TerminalAppKey, sizeof(other.appId));
        CHECK(TaskbarPrimaryWindow(catalog, terminal) == -1 && TaskbarPrimaryWindow(catalog, other) == -1);
        WmStrCopy(g_windows[primary].taskbarCatalogId, terminal.catalogId, sizeof(g_windows[primary].taskbarCatalogId));
        CHECK(TaskbarPrimaryWindow(catalog, terminal) == primary && TaskbarPrimaryWindow(catalog, other) == -1);
        WmStrCopy(other.appId, "org.brook.Editor", sizeof(other.appId));

        const auto edit = button(ids[2]);
        CHECK(WmShellTrayPointer(sw, sh, edit.x + 2, edit.y + 2, true, 1, 0));
        CHECK(g_taskbarContextMenu.open && g_taskbarContextMenu.window == ids[2] && !g_taskbarContextMenu.pinned);
        const auto menu = TaskbarContextMenuRect(sw, sh);
        const auto row = ChromeTrayMenuRowRect(WmChromeStyle(), menu, 0);
        const auto original = g_windows[ids[2]].creationIdentity;
        ++g_windows[ids[2]].creationIdentity;
        CHECK(!WmShellTrayPointer(sw, sh, row.x + 4, row.y + 4, true, 0, 0));
        CHECK(!g_taskbarContextMenu.open && g_launcherModel.DesktopPins().Count() == 1);
        g_windows[ids[2]].creationIdentity = original;
        CHECK(WmShellTrayPointer(sw, sh, edit.x + 2, edit.y + 2, true, 1, 0));
        CHECK(WmShellTrayPointer(sw, sh, row.x + 4, row.y + 4, true, 0, 0));
        CHECK(g_launcherModel.DesktopPins().Find(other.catalogId) == 1);
        testTick += 1001;
        layoutSyncs = 0; layoutRenamed = false; layoutWritable = true;
        TaskbarPinsSaveState();
        CHECK(layoutRenamed && layoutSyncs == 2 && g_taskbarPinsSaved == g_taskbarPinsRevision);
        TaskbarPins restored{};
        CHECK(restored.Parse(layoutText) && restored.Find(other.catalogId) == 1);

        for (int id : ids) WmDestroyWindow(id);
        std::free(clients);
        g_launcherActive = &g_emptyLauncher;
        g_launcherModel.Init();
        g_taskbarPageTarget = -1;
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
        testScreenW=1366;testScreenH=900;
        WmApplyDisplaySize(testScreenW,testScreenH);
        CHECK(g.fullscreen && g.clientW==1366 && g.clientH==900 && g.x==0 && g.y==0);
        CHECK(WmInputPop(&g,&event,1)==1 && event.type==WM_EVT_RESIZED &&
              event.reserved==WM_RESIZE_FULLSCREEN && event.x==1366 && event.y==900);
        CHECK(g.fsW==testScreenW-2*WmMetric(WM_BORDER_WIDTH));
        testScreenW=sw;testScreenH=sh;
        WmApplyDisplaySize(sw,sh);
        CHECK(g.fullscreen && g.clientW==sw && g.clientH==sh);
        WmInputPop(&g,drain,256);
        CHECK(WmSetFullscreen(game,false) && !g.fullscreen && g.state==WindowState::Maximized);
        // The decoration requested while fullscreen (server-side) applies now.
        CHECK(!g.noChrome && g.clientH<WmDesktopHeight(sh) && taskbarShown());
        CHECK(WmInputPop(&g,&event,1)==1 && event.reserved==WM_RESIZE_MAXIMIZED);
        WmSetMaximized(game,false);
        CHECK(g.state==WindowState::Normal && g.x==40 && g.y==50 && g.clientW==640 && g.clientH==480);
        CHECK(WmInputPop(&g,&event,1)==1 && event.type==WM_EVT_RESIZED && event.reserved==0);
        WmSetMaximized(game,false,true);
        CHECK(WmInputPop(&g,&event,1)==1 && event.type==WM_EVT_RESIZED && event.reserved==0);
        // Legacy Xwayland WM: chrome-less, placed at the origin, output-sized.
        WmSetClientSideDecoration(game,true);WmMoveWindow(game,0,0);
        CHECK(WmResizeWindow(game,uint16_t(sw),uint16_t(sh)) && !g.fullscreen && !taskbarShown());
        CHECK(WmResizeWindow(game,uint16_t(sw),uint16_t(sh-1)) && taskbarShown());
        WmInputPop(&g,drain,256);
        // The resize release of an API-backed floating window queues a final size.
        CHECK(WmResizeWindow(game,800,600));
        CHECK(WmInputPop(&g,&event,1)==1 && event.type==WM_EVT_RESIZED);
        CHECK(event.x==800 && event.y==600 && !event.reserved);
        CHECK(WmInputPop(&g,&event,1)==0);
        g.vfb=nullptr;WmDestroyWindowById(gamer,g_windows[editor].wmId);WmDestroyWindowById(gamer,g.wmId);
        CHECK(taskbarShown() && !lockDepth);
        std::free(gamer);
    }
    // Side dock (slice D1). Docking hides a window from the taskbar, Alt+Tab
    // and the desktop and places it as the page (sidebar below the strip, no
    // chrome, maximised configure). The dock opens on Super+S, the tab or an
    // icon; Escape, an outside click and the selected tile hide it. Pinning
    // takes the sidebar out of the work area and reflows maximised and
    // floating windows. Returning restores geometry, state and decoration.
    {
        constexpr uint32_t sw=1280,sh=800;testScreenW=sw;testScreenH=sh;
        const int border=int(WmMetric(WM_BORDER_WIDTH)),title=int(WmMetric(WM_TITLE_BAR_HEIGHT));
        const int desktopH=int(WmDesktopHeight(sh)),sideW=int(WmMetric(ChromeDockMetrics::Width));
        const int stripH=int(WmMetric(ChromeDockMetrics::Strip));
        auto* owner=static_cast<Process*>(std::calloc(1,sizeof(Process)));CHECK(owner);
        owner->pid=owner->tgid=13;owner->incarnation=131;
        const int chat=WmCreateWindow(owner,500,100,640,480,"Chat",1,true);
        const int notes=WmCreateWindow(owner,30,60,400,300,"Notes",1,true);
        const int big=WmCreateWindow(owner,600,200,500,400,"Big",1,true);
        CHECK(chat>=0 && notes>=0 && big>=0);
        Window& a=g_windows[chat];Window& b=g_windows[notes];Window& c=g_windows[big];
        for(Window* w:{&a,&b,&c})w->vfb=reinterpret_cast<uint32_t*>(0x1000);
        Window::WmInputEvent drain[256]{};
        auto lastResize=[&](Window& w){
            Window::WmInputEvent events[256]{},last{};
            const uint32_t n=WmInputPop(&w,events,256);
            for(uint32_t i=0;i<n;++i)if(events[i].type==WM_EVT_RESIZED)last=events[i];
            return last;
        };
        auto inTaskbar=[&](int idx){
            const auto l=GetTaskbarLayout(sw,sh);
            for(int i=0;i<l.count;++i)if(l.indices[i]==idx)return true;
            return false;
        };
        auto dockSurface=[&](WmShellSurface& out){
            WmShellSurface s[WM_SHELL_SURFACE_LIMIT];const uint32_t n=WmShellSurfaces(sw,sh,-1,-1,s);
            for(uint32_t i=0;i<n;++i)if(s[i].kind==WmShellSurfaceKind::Dock){out=s[i];return true;}
            return false;
        };
        WmShellSurface dock{};
        WmSetMaximized(big,true);
        CHECK(c.x==0 && c.clientW==sw-2*border && c.clientH==uint16_t(desktopH-title-2*border));
        for(Window* w:{&a,&b,&c})WmInputPop(w,drain,256);
        CHECK(!dockSurface(dock) && WmWorkArea(sw,sh).x==0); // nothing at rest without entries

        // Super+Shift+S on the focused Chat docks it; docking opens the dock
        // on it, focused, so the window stays in view.
        WmSetFocus(chat);WmDockFocusedWindow();
        CHECK(a.docked && !a.minimized && a.noChrome && a.state==WindowState::Maximized && a.focused);
        CHECK(WmDockEntryCount()==1 && WmDockEntryWindow(0)==chat && WmDockOpen());
        CHECK(a.x==0 && a.y==stripH && a.clientW==sideW && a.clientH==desktopH-stripH);
        {const auto e=lastResize(a);CHECK(e.type==WM_EVT_RESIZED && e.reserved==WM_RESIZE_MAXIMIZED && e.x==sideW);}
        CHECK(!inTaskbar(chat) && inTaskbar(notes) && inTaskbar(big));
        // Hidden (Escape), the page leaves focus and the z-order.
        CHECK(WmDockDismiss() && !WmDockOpen() && a.minimized && !a.focused);
        CHECK(WmGetFocusedWindow()!=chat && WmGetFocusedWindow()>=0);
        {int z[WM_MAX_WINDOWS];const uint32_t n=WmGetZOrder(z,WM_MAX_WINDOWS);for(uint32_t i=0;i<n;++i)CHECK(z[i]!=chat);}
        // Requests from the client cannot move, restore or minimise the page.
        WmMoveWindow(chat,300,300);WmSetMaximized(chat,false);WmRestoreWindow(chat);
        CHECK(a.x==0 && a.y==stripH && a.state==WindowState::Maximized && a.minimized);
        // A decoration change while docked is kept for the return.
        WmSetClientSideDecoration(chat,true);CHECK(a.dkNoChrome && a.noChrome);
        WmSetClientSideDecoration(chat,false);CHECK(!a.dkNoChrome && a.noChrome);

        // Closed with an entry: the 5 px tab, vertically centred, no glass.
        CHECK(dockSurface(dock) && dock.x==0 && !dock.material && dock.w==WmMetric(ChromeDockMetrics::Tab));
        CHECK(dock.h==WmMetric(ChromeDockMetrics::TabHeight) && dock.y==int32_t((desktopH-int(dock.h))/2));
        CHECK(WmDockPointerOwned(1,dock.y+5,sw,sh) && !WmDockPointerOwned(40,dock.y+5,sw,sh));
        // The tab opens the dock on the page, focused and drawn.
        CHECK(WmDockPress(1,dock.y+5,sw,sh) && WmDockOpen() && !a.minimized && a.focused);
        CHECK(dockSurface(dock) && dock.material && dock.x==0 && dock.y==0 && int(dock.w)==sideW && int(dock.h)==stripH);
        CHECK(!inTaskbar(chat));
        {int z[WM_MAX_WINDOWS];const uint32_t n=WmGetZOrder(z,WM_MAX_WINDOWS);CHECK(n && z[n-1]==chat);}
        // The shell snapshot: one entry, selected, with the window's title.
        {
            static ChromeShellModel model{};
            CHECK(WmCaptureShellModel(dock,sw,sh,0,-1,-1,model) && model.kind==ChromeShellKind::Dock);
            CHECK(model.dock.open && model.dock.count==1 && model.dock.selected==0 && !strcmp(model.text,"Chat"));
            CHECK(model.mouseX==-1 && !model.dockPinned);
        }
        // Escape hides an open, unpinned dock and focus returns to the desktop.
        CHECK(WmDockDismissible() && WmDockDismiss() && !WmDockOpen() && a.minimized && !a.focused);
        CHECK(WmGetFocusedWindow()>=0 && !g_windows[WmGetFocusedWindow()].docked);
        // Super+S toggles it open on the last page, and closed.
        WmDockToggle();CHECK(WmDockOpen() && !a.minimized && a.focused);
        WmDockToggle();CHECK(!WmDockOpen() && a.minimized);
        // A click outside the open sidebar hides it and is not consumed.
        WmDockToggle();
        CHECK(!WmDockPress(sideW+100,400,sw,sh) && !WmDockOpen());
        // A click inside the page is the client's and keeps it open.
        WmDockToggle();
        CHECK(!WmDockPress(10,stripH+10,sw,sh) && WmDockOpen());

        // A second guest; icons select pages; the selected tile hides it.
        WmSetFocus(notes);WmDockFocusedWindow();
        CHECK(b.docked && !b.minimized && a.minimized && b.focused && WmDockEntryCount()==2 && WmDockOpen());
        {
            const auto n=ChromeDockLayoutFor(WmDesktopMetrics(),sw,uint32_t(desktopH),2,1,true);
            CHECK(WmDockPress(n.cells[0].x+4,n.cells[0].y+4,sw,sh) && !a.minimized && b.minimized && a.focused);
        }
        {
            const auto l=ChromeDockLayoutFor(WmDesktopMetrics(),sw,uint32_t(desktopH),2,0,true);
            CHECK(l.count==2 && l.selected==0 && l.tile.w>l.cells[1].w && l.cells[1].w==int(WmMetric(ChromeDockMetrics::Cell)));
            CHECK(WmDockPress(l.cells[1].x+4,l.cells[1].y+4,sw,sh) && !b.minimized && a.minimized && b.focused);
            const auto m=ChromeDockLayoutFor(WmDesktopMetrics(),sw,uint32_t(desktopH),2,1,true);
            CHECK(WmDockPress(m.title.x+4,m.title.y+4,sw,sh) && !WmDockOpen() && b.minimized);
            CHECK(!inTaskbar(notes) && !inTaskbar(chat));
        }
        // Alt+Tab never reaches a docked window.
        {
            auto* other=static_cast<Process*>(owner);
            const int extra=WmCreateWindow(other,700,300,300,200,"Extra",1,true);CHECK(extra>=0);
            for(int i=0;i<6;++i){const int t=WmCycleFocus();CHECK(t!=chat && t!=notes);}
            CHECK(!g_windows[WmGetFocusedWindow()].docked);
            // With the dock open (unpinned), Alt+Tab closes it first.
            WmDockToggle();CHECK(WmDockOpen());
            const int t=WmCycleFocus();CHECK(t>=0 && !g_windows[t].docked && !WmDockOpen());
            WmDestroyWindowById(owner,g_windows[extra].wmId);
        }

        // Pin: the sidebar leaves the work area. The maximised window
        // reflows beside it, a floating window under it moves out, and new
        // maximise targets start at its inner edge.
        const int floating=WmCreateWindow(owner,40,80,300,200,"Floating",1,true);CHECK(floating>=0);
        Window& f=g_windows[floating];f.vfb=reinterpret_cast<uint32_t*>(0x1000);
        WmDockSetPinned(true);
        CHECK(WmDockPinned() && WmDockOpen() && !b.minimized); // shows the last page
        CHECK(WmWorkArea(sw,sh).x==sideW && WmWorkArea(sw,sh).w==int(sw)-sideW && WmWorkArea(sw,sh).h==desktopH);
        CHECK(c.state==WindowState::Maximized && c.x==sideW && c.clientW==sw-sideW-2*border);
        {const auto e=lastResize(c);CHECK(e.type==WM_EVT_RESIZED && e.reserved==WM_RESIZE_MAXIMIZED);}
        CHECK(f.x==sideW && f.y==80);
        WmMoveWindow(floating,10,90);CHECK(f.x==sideW && f.y==90); // cannot go under it
        WmSetMaximized(floating,true);CHECK(f.x==sideW && f.clientW==sw-sideW-2*border);
        WmSetMaximized(floating,false);CHECK(f.state==WindowState::Normal && f.x==sideW);
        // Pinned, an outside click, Escape and the tile leave it shown.
        CHECK(!WmDockDismissible() && !WmDockDismiss());
        CHECK(!WmDockPress(sideW+100,400,sw,sh) && WmDockOpen());
        WmDockToggle();CHECK(WmDockOpen() && b.focused);
        {
            static ChromeShellModel model{};
            CHECK(dockSurface(dock) && WmCaptureShellModel(dock,sw,sh,0,-1,-1,model) && model.dockPinned);
        }
        // Unpinning reflows back to the full width; the dock stays open.
        {
            const auto l=ChromeDockLayoutFor(WmDesktopMetrics(),sw,uint32_t(desktopH),2,1,true);
            CHECK(WmDockPress(l.pin.x+2,l.pin.y+2,sw,sh) && !WmDockPinned() && WmDockOpen());
        }
        CHECK(WmWorkArea(sw,sh).x==0 && c.x==0 && c.clientW==sw-2*border);
        WmDockSetPinned(true);CHECK(c.x==sideW);

        // Native frames carry the dock button, showing the edge it docks to;
        // docked pages and client-decorated windows do not.
        CHECK(CaptionGeometry(f).dockButton==1 && CaptionGeometry(b).dockButton==0);
        // The other edge: the page, the strip, the work area and the
        // maximised window follow; the floating window clear of it stays.
        WmDockSetRight(true);
        CHECK(WmDockRight() && b.x==int(sw)-sideW && b.clientW==sideW);
        CHECK(dockSurface(dock) && dock.x==int32_t(sw)-sideW && int(dock.w)==sideW);
        CHECK(WmWorkArea(sw,sh).x==0 && WmWorkArea(sw,sh).w==int(sw)-sideW);
        CHECK(c.x==0 && c.clientW==sw-sideW-2*border && f.x==sideW);
        CHECK(CaptionGeometry(f).dockButton==2);
        // A floating window moved under the right sidebar is pushed back out.
        WmMoveWindow(floating,int16_t(sw-100),90);CHECK(f.x+int(f.outerWidth())==int(sw)-sideW);
        WmMoveWindow(floating,int16_t(sideW),80);
        {
            const auto l=ChromeDockLayoutFor(WmDesktopMetrics(),sw,uint32_t(desktopH),2,1,true,{0,true});
            CHECK(l.right && l.sidebar.x==int(sw)-sideW && l.grip.x==l.sidebar.x-int(WmMetric(ChromeDockMetrics::GripOutside)));
            // The strip's side button moves it back.
            CHECK(WmDockPress(l.side.x+2,l.side.y+2,sw,sh) && !WmDockRight());
        }
        CHECK(b.x==0 && dockSurface(dock) && dock.x==0 && WmWorkArea(sw,sh).x==sideW && c.x==sideW);
        // A set width (logical pixels) moves the inner edge.
        const int wide=int(WmMetric(500));
        WmDockSetWidth(500);
        CHECK(WmDockWidth()==500 && b.clientW==wide && WmWorkArea(sw,sh).x==wide && c.x==wide);
        // The grip: pressing the inner edge starts a drag; motion resizes the
        // page interactively; the release settles the work area and saves.
        {
            const auto l=ChromeDockLayoutFor(WmDesktopMetrics(),sw,uint32_t(desktopH),2,1,true,{500,false});
            CHECK(l.grip.x<wide && l.grip.x+l.grip.w>wide);
            CHECK(WmDockPointerOwned(wide+1,400,sw,sh));
            const uint32_t before=g_dockSaveEpoch;
            CHECK(WmDockPress(wide-1,400,sw,sh) && WmDockDragActive());
            WmDockDrag(wide-61,true);
            // Maximised windows reflow once, on the release.
            CHECK(b.clientW==wide-60 && c.x==wide);
            {const auto e=lastResize(b);CHECK(e.type==WM_EVT_RESIZED && e.x==wide-60 && e.reserved==WM_RESIZE_MAXIMIZED);}
            WmDockDrag(wide-61,false);
            CHECK(!WmDockDragActive() && WmDockWidth()==440 && WmWorkArea(sw,sh).x==int(WmMetric(440)));
            CHECK(c.x==int(WmMetric(440)) && g_dockSaveEpoch!=before);
            {const auto e=lastResize(b);CHECK(e.type==WM_EVT_RESIZED && e.x==int(WmMetric(440)));}
            // Clamped to the minimum and to half the screen.
            CHECK(WmDockPress(int(WmMetric(440))-1,400,sw,sh) && WmDockDragActive());
            WmDockDrag(10,false);CHECK(WmDockWidth()==ChromeDockMetrics::MinWidth);
            CHECK(WmDockPress(int(WmMetric(ChromeDockMetrics::MinWidth)),400,sw,sh));
            WmDockDrag(int(sw)-5,false);CHECK(b.clientW==sw/2); // 640 < MaxWidth
        }
        WmDockSetWidth(0);CHECK(b.clientW==sideW && c.x==sideW);
        CHECK(f.x==int(sw)/2); // pushed out by the widest sidebar, and left there
        WmMoveWindow(floating,int16_t(sideW),80);

        // Return Chat (floating when docked): back on the taskbar, focused,
        // with its geometry, state and chrome. Pinned, the dock keeps Notes.
        WmInputPop(&a,drain,256);
        CHECK(WmDockReturnWindow(chat));
        CHECK(!a.docked && !a.minimized && a.focused && !a.noChrome && a.state==WindowState::Normal);
        CHECK(a.x==500 && a.y==100 && a.clientW==640 && a.clientH==480 && inTaskbar(chat));
        {const auto e=lastResize(a);CHECK(e.type==WM_EVT_RESIZED && e.reserved==0 && e.x==640 && e.y==480);}
        CHECK(WmDockEntryCount()==1 && WmDockOpen() && !b.minimized);

        // A maximised window docks and returns maximised at today's work
        // area, and its restore geometry survives.
        WmSetFocus(big);WmDockFocusedWindow();
        CHECK(c.docked && WmDockEntryCount()==2 && c.clientW==sideW);
        WmSetFocus(big);CHECK(c.focused);
        WmDockFocusedWindow(); // from the page: back to the desktop
        CHECK(!c.docked && c.state==WindowState::Maximized && c.x==sideW && c.clientW==sw-sideW-2*border && c.focused);
        WmSetMaximized(big,false);
        CHECK(c.state==WindowState::Normal && c.x==600 && c.y==200 && c.clientW==500 && c.clientH==400);

        // A page asking for fullscreen returns to the desktop first; true
        // fullscreen covers the strip.
        WmSetFocus(big);WmDockFocusedWindow();CHECK(c.docked);
        CHECK(WmSetFullscreen(big,true) && !c.docked && c.fullscreen && c.clientW==sw);
        WmSetFocus(big);CHECK(WmTaskbarHidden() && !dockSurface(dock) && !WmDockPointerOwned(1,10,sw,sh));
        CHECK(WmSetFullscreen(big,false) && !c.fullscreen);

        // Closing a docked window removes its entry; the last one unpins the
        // work area (the setting stays) and the dock disappears.
        CHECK(WmDockEntryCount()==1 && WmDockEntryWindow(0)==notes);
        b.vfb=nullptr;WmDestroyWindowById(owner,b.wmId);
        CHECK(WmDockEntryCount()==0 && !WmDockOpen() && !dockSurface(dock) && WmDockPinned());
        CHECK(WmWorkArea(sw,sh).x==0 && c.x>=0 && f.x==sideW);
        // The pin persists through the kernel-written dock.ini, saved by the
        // launcher worker after each change, and parses back.
        layoutWritable=true;layoutRenamed=false;layoutSyncs=0;
        DockSaveState();
        CHECK(layoutRenamed && !strcmp(layoutText,"[Dock]\npinned=1\nside=left\nwidth=0\n"));
        layoutRenamed=false;layoutSyncs=0;DockSaveState();CHECK(!layoutRenamed); // nothing new to write
        WmDockSetPinned(false);
        DockSaveState();CHECK(layoutRenamed && !strcmp(layoutText,"[Dock]\npinned=0\nside=left\nwidth=0\n"));
        g_dock.right=true;g_dock.width=512;DockQueueSave();layoutRenamed=false;layoutSyncs=0;
        DockSaveState();CHECK(!strcmp(layoutText,"[Dock]\npinned=0\nside=right\nwidth=512\n"));
        {
            char text[]="[Launcher]\npinned=1\n[Dock]\r\nwidth=420\r\npinned=1\r\n";
            DockParseState(text);CHECK(WmDockPinned());
            CHECK(WmDockWidth()==420);
            char off[]="[Dock]\npinned=0\nside=left\nwidth=99999";DockParseState(off);
            CHECK(!WmDockPinned() && !WmDockRight() && WmDockWidth()==0);
            char right[]="[Dock]\nside=right\nwidth=360\n";DockParseState(right);
            CHECK(WmDockRight() && WmDockWidth()==360);
            g_dock.right=false;g_dock.width=0;
        }
        // The window menu: a secondary press on a native title bar opens it at
        // the pointer, in the app tray menu's surface; rows act on the press.
        {
            WmSetFocus(floating);
            const int tx=f.x+20,ty=f.y+border+4;
            CHECK(WmHitTest(tx,ty).windowIndex==floating && WmHitTest(tx,ty).zone==WmHitZone::TitleBar);
            const auto clock=GetTaskbarLayout(sw,sh).clock;
            CHECK(WmTrayFlyoutPress(clock.x,clock.y,sw,sh));
            const auto calendar=WmCalendarPopupLayout(sw,sh,fixtureClock.date);
            CHECK(WmTrayFlyoutPress(calendar.pin.x,calendar.pin.y,sw,sh) && WmTaskbarPopupPinned());
            CHECK(WmShowWindowMenu(floating,tx,ty));
            WmShellTrayGeometry pinnedGeometry{};
            WmGetShellTrayGeometry(sw,sh,tx,ty,pinnedGeometry);
            CHECK(pinnedGeometry.windowMenu && WmTaskbarPopupPinned());
            WmShellSurface layers[WM_SHELL_SURFACE_LIMIT]{};
            const auto layerCount=WmShellSurfaces(sw,sh,tx,ty,layers,pinnedGeometry.popup,
                                                  pinnedGeometry.menu,pinnedGeometry.notice,pinnedGeometry.windowMenu);
            for (unsigned i=0;i<layerCount;++i) CHECK(layers[i].kind!=WmShellSurfaceKind::TrayFlyout);
            WmWindowMenuClose();
            CHECK(WmTrayFlyoutVisible() && WmTaskbarPopupPinned());
            CHECK(WmTrayFlyoutPress(calendar.pin.x,calendar.pin.y,sw,sh) && !WmTaskbarPopupPinned());
            WmTrayFlyoutClose();
            CHECK(!WmShellTrayPointer(sw,sh,tx,ty,true,0,0) && !WmWindowMenuOpen()); // primary: not ours
            CHECK(WmShellTrayPointer(sw,sh,tx,ty,true,1,0) && WmWindowMenuOpen());
            WmShellTrayGeometry geometry{};
            WmGetShellTrayGeometry(sw,sh,tx,ty,geometry);
            CHECK(geometry.menu && geometry.popup.x==tx && geometry.popup.y==ty);
            WmShellSurface menu{};menu.kind=WmShellSurfaceKind::AppTrayMenu;
            menu.x=geometry.popup.x;menu.y=geometry.popup.y;menu.w=uint32_t(geometry.popup.w);menu.h=uint32_t(geometry.popup.h);
            static ChromeShellModel model{};
            CHECK(WmCaptureShellModel(menu,sw,sh,0,-1,-1,model) && model.kind==ChromeShellKind::AppTrayMenu);
            CHECK(model.tray.menuCount==4 && !strcmp(model.tray.menu[0].label,"Move to side dock"));
            CHECK(!strcmp(model.tray.menu[2].label,"Maximise") && !strcmp(model.tray.menu[3].label,"Close"));
            // Motion is the menu's; a press outside closes it and goes no further.
            CHECK(WmShellTrayPointer(sw,sh,5,5,false,0,0) && WmWindowMenuOpen());
            CHECK(WmShellTrayPointer(sw,sh,5,5,true,0,0) && !WmWindowMenuOpen());
            // Escape closes it (global shortcut), and the window's going does too.
            CHECK(WmShowWindowMenu(floating,tx,ty) && WmWindowMenuOpen());
            WmWindowMenuClose();CHECK(!WmWindowMenuOpen());
            // "Move to side dock" docks it; for the page the menu offers the return.
            CHECK(WmShowWindowMenu(floating,tx,ty));
            WmGetShellTrayGeometry(sw,sh,tx,ty,geometry);
            const DesktopRect row=ChromeTrayMenuRowRect(WmChromeStyle(),geometry.popup,0);
            CHECK(WmShellTrayPointer(sw,sh,row.x+4,row.y+4,true,0,0) && !WmWindowMenuOpen() && f.docked);
            CHECK(WmShowWindowMenu(floating,10,10));
            CHECK(WmCaptureShellModel(menu,sw,sh,0,-1,-1,model) && !strcmp(model.tray.menu[0].label,"Return to desktop"));
            CHECK(!model.tray.menu[1].enabled && !model.tray.menu[2].enabled && model.tray.menu[3].enabled);
            WmWindowMenuClose();
            CHECK(WmDockReturnWindow(floating) && !f.docked);
            // Near the screen's corner it flips to stay on the desktop.
            const DesktopRect flipped=ChromeWindowMenuRect(WmChromeStyle(),int(sw)-4,desktopH-4,4,int(sw),desktopH);
            CHECK(flipped.x+flipped.w<=int(sw) && flipped.y+flipped.h<=desktopH && flipped.w>0);
            // A dead window's menu closes on the next event.
            CHECK(WmShowWindowMenu(floating,tx,ty));
            const uint64_t identity=f.creationIdentity;f.creationIdentity=identity+1;
            CHECK(!WmShellTrayPointer(sw,sh,tx,ty,false,0,0) && !WmWindowMenuOpen());
            f.creationIdentity=identity;
        }
        // Drag to dock: a window move over the shut dock's edge shows the
        // "Send to dock" chip (even with no entries) and releasing docks it,
        // opening the dock; returning restores where the move began.
        {
            CHECK(WmDockEntryCount()==0 && !WmDockOpen() && !dockSurface(dock));
            const int edge=int(WmMetric(ChromeDockMetrics::DropEdge));
            WmDockWindowDrag(floating,edge+10,300);CHECK(!dockSurface(dock));
            WmDockWindowDrag(floating,edge-1,300);
            CHECK(dockSurface(dock) && dock.x==0 && dock.material && int(dock.w)==int(WmMetric(ChromeDockMetrics::DropChipW)));
            CHECK(!WmDockPointerOwned(dock.x+4,dock.y+4,sw,sh)); // feedback, not a target
            {
                static ChromeShellModel model{};
                CHECK(WmCaptureShellModel(dock,sw,sh,0,-1,-1,model) && model.dock.drop && !model.dock.open);
            }
            // Below the desktop (over the taskbar) is not the target.
            WmDockWindowDrag(floating,2,desktopH+2);CHECK(!dockSurface(dock));
            CHECK(!WmDockWindowDrop(floating,2,desktopH+2,33,44) && !f.docked);
            WmDockWindowDrag(floating,2,300);
            CHECK(WmDockWindowDrop(floating,2,300,33,44) && f.docked && WmDockOpen() && f.focused && !f.minimized);
            CHECK(dockSurface(dock) && int(dock.h)==stripH); // the strip, the chip gone
            // With the dock open, the whole sidebar is the target.
            WmDockWindowDrag(chat,sideW-10,400);
            {
                static ChromeShellModel model{};
                CHECK(dockSurface(dock) && WmCaptureShellModel(dock,sw,sh,0,-1,-1,model) && model.dock.drop && model.dock.open);
            }
            WmDockWindowDrag(chat,sideW+10,400);
            {
                static ChromeShellModel model{};
                CHECK(WmCaptureShellModel(dock,sw,sh,0,-1,-1,model) && !model.dock.drop);
            }
            WmDockWindowDrag(chat,sideW-10,400);
            CHECK(WmDockWindowDrop(chat,sideW-10,400,500,100) && a.docked && WmDockEntryCount()==2 && a.focused);
            CHECK(WmDockReturnWindow(chat) && a.x==500 && a.y==100);
            CHECK(WmDockReturnWindow(floating) && !f.docked && f.x==33 && f.y==44);
            // On the right, the right edge is the target; a cancel clears it.
            WmDockSetRight(true);
            WmDockWindowDrag(floating,2,300);CHECK(!dockSurface(dock));
            WmDockWindowDrag(floating,int(sw)-2,300);CHECK(dockSurface(dock) && dock.x+int32_t(dock.w)==int32_t(sw));
            WmDockWindowDragCancel();CHECK(!dockSurface(dock));
            CHECK(!WmDockWindowDrop(floating,int(sw)-2,300,33,44) && !f.docked); // no drag in progress
            WmDockSetRight(false);
        }
        for(Window* w:{&a,&c,&f}){w->vfb=nullptr;WmDestroyWindowById(owner,w->wmId);}
        CHECK(!lockDepth);
        std::free(owner);
    }
    // Dock strip layout and hit test: icon cells, the wide selected tile
    // with Return and Close, the pin at the end; the tab when closed.
    {
        const DesktopMetrics& m=DesktopMetric100;
        const auto l=ChromeDockLayoutFor(m,1600,852,4,2,true);
        CHECK(l.open && l.count==4 && l.selected==2 && l.strip.w==420 && l.strip.h==48 && l.page.y==48 && l.page.h==804);
        CHECK(l.cells[0].x==0 && l.cells[0].w==42 && l.cells[1].x==42 && l.tile.x==84);
        CHECK(l.pin.x==382 && l.pin.w==38 && l.side.x==344 && l.tile.w==420-2*38-3*42 && l.cells[3].x==l.tile.x+l.tile.w);
        CHECK(l.grip.x==414 && l.grip.w==18 && l.grip.h==852 && !l.right);
        CHECK(l.close.x+l.close.w==l.tile.x+l.tile.w && l.back.x+l.back.w==l.close.x && l.title.x==l.tile.x+42);
        auto hit=[&](int x,int y){return ChromeDockHitTest(l,x,y);};
        CHECK(hit(5,5).target==ChromeDockTarget::Entry && hit(5,5).index==0);
        CHECK(hit(l.cells[3].x+1,1).target==ChromeDockTarget::Entry && hit(l.cells[3].x+1,1).index==3);
        CHECK(hit(l.title.x+1,1).target==ChromeDockTarget::Tile && hit(l.title.x+1,1).index==2);
        CHECK(hit(l.back.x+1,1).target==ChromeDockTarget::Return && hit(l.close.x+1,47).target==ChromeDockTarget::Close);
        CHECK(hit(l.pin.x+2,47).target==ChromeDockTarget::Pin && hit(5,48).target==ChromeDockTarget::None);
        CHECK(hit(l.side.x+2,4).target==ChromeDockTarget::Side);
        CHECK(hit(419,47).target==ChromeDockTarget::Grip && hit(431,400).target==ChromeDockTarget::Grip);
        CHECK(hit(432,400).target==ChromeDockTarget::None && hit(413,400).target==ChromeDockTarget::None);
        // The right edge mirrors the sidebar, the grip and the tab.
        const auto r=ChromeDockLayoutFor(m,1600,852,4,2,true,{0,true});
        CHECK(r.right && r.sidebar.x==1180 && r.strip.x==1180 && r.page.x==1180 && r.grip.x==1168 && r.grip.w==18);
        CHECK(r.pin.x==1600-38 && r.cells[0].x==1180);
        const auto rc=ChromeDockLayoutFor(m,1600,852,3,1,false,{0,true});
        CHECK(rc.tab.x==1595 && rc.grip.w==0);
        // Width: logical, scaled, clamped to [MinWidth, half the screen].
        CHECK(ChromeDockSidebar(m,1600,852,{360,false}).w==360 && ChromeDockSidebar(m,1600,852,{100,false}).w==360);
        CHECK(ChromeDockSidebar(DesktopMetric150,2400,1200,{360,false}).w==540);
        CHECK(ChromeDockSidebar(m,1600,852,{1000,true}).w==720 && ChromeDockSidebar(m,1600,852,{1000,true}).x==880);
        CHECK(ChromeDockSidebar(m,1000,852,{700,false}).w==500); // half the screen
        CHECK(ChromeDockWidthFor(DesktopMetric150,2400,540)==360 && ChromeDockWidthFor(m,1600,10)==360);
        // At 200% everything scales; the sidebar never exceeds half the screen.
        const auto big=ChromeDockLayoutFor(DesktopMetric200,3200,1704,1,0,true);
        CHECK(big.strip.w==840 && big.strip.h==96 && big.tile.w==840-2*76 && big.grip.w==36);
        CHECK(ChromeDockSidebar(m,600,500).w==300);
        // Many entries: the tile keeps an icon cell and both buttons.
        const auto many=ChromeDockLayoutFor(m,1600,852,20,15,true);
        CHECK(many.count==1+(420-76-42-64)/42 && many.selected==many.count-1 && many.tile.w>=42+64);
        // Closed: the tab only, and the strip is not hit.
        const auto closed=ChromeDockLayoutFor(m,1600,852,3,1,false);
        CHECK(!closed.open && closed.tab.x==0 && closed.tab.w==5 && closed.tab.h==120 && closed.tab.y==(852-120)/2);
        CHECK(ChromeDockHitTest(closed,2,closed.tab.y+1).target==ChromeDockTarget::Tab);
        CHECK(ChromeDockHitTest(closed,5,5).target==ChromeDockTarget::None);
        CHECK(ChromeDockLayoutFor(m,1600,852,0,-1,false).tab.w==0);
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
    // Files focus uses the Wayland app ID even when the title is a folder.
    {
        static Process files{};
        files.pid = files.tgid = 99;
        files.incarnation = 99;
        SetWmActiveLocked(true);
        const int idx = WmCreateWindow(&files, 10, 10, 300, 200, "Downloads");
        CHECK(idx >= 0);
        auto& w = g_windows[idx];
        WmStrCopy(w.appId, LauncherFilesAppId, sizeof(w.appId));
        WmMinimizeWindow(idx);
        CHECK(w.minimized);
        WmOpenFiles();
        CHECK(g_focusedIdx == idx && w.focused && !w.minimized);
        WmDestroyWindowById(&files, w.wmId);
    }
    // Task Manager shares the tray menu surface and the ordinary launch queue.
    {
        constexpr uint32_t sw = 1600, sh = 900;
        const uint32_t saved = WmGetTaskbarMetrics();
        CHECK(WmSetTaskbarMetrics(127));
        SetWmActiveLocked(true);
        g_launcherWorkerStarted = true;
        const auto layout = GetTaskbarLayout(sw, sh);
        for (unsigned metric : {0, 1, 4, 5}) {
            const auto anchor = layout.metrics[metric];
            CHECK(anchor.w > 0 && WmTaskbarStatsContains(anchor.x, anchor.y, sw, sh));
            if (metric == 4) {
                const auto savedHistory = g_desktopCadence;
                const uint64_t savedTick = testTick;
                g_desktopCadence.Reset();
                g_desktopCadence.Record(9400);
                for (uint64_t t = 9800; t < 9900; t += 5) g_desktopCadence.Record(t);
                testTick = 10000;
                char text[48]{};
                const char* detail = nullptr;
                DesktopRect box{};
                CHECK(WmTrayTooltip(sw, sh, anchor.x + anchor.w - 1, anchor.y, text, detail, box));
                CHECK(!strcmp(text, "Compositor frame timing") && detail &&
                      !strcmp(detail, "p80 5 ms  max 400 ms"));
                g_desktopCadence = savedHistory;
                testTick = savedTick;
            }
            CHECK(!WmShellTrayPointer(sw, sh, anchor.x, anchor.y, true, 0, 0));
            CHECK(!WmWindowMenuOpen());
            CHECK(WmShellTrayPointer(sw, sh, anchor.x, anchor.y, true, 1, 0));
            CHECK(WmWindowMenuOpen());
            WmShellTrayGeometry geometry{};
            WmGetShellTrayGeometry(sw, sh, anchor.x, anchor.y, geometry);
            CHECK(geometry.menu && geometry.windowMenu && geometry.popup.w > 0);
            WmShellSurface surface{};
            surface.kind = WmShellSurfaceKind::AppTrayMenu;
            surface.x = geometry.popup.x;
            surface.y = geometry.popup.y;
            surface.w = geometry.popup.w;
            surface.h = geometry.popup.h;
            static ChromeShellModel snapshot{};
            CHECK(WmCaptureShellModel(surface, sw, sh, 0, -1, -1, snapshot));
            CHECK(snapshot.tray.menuCount == 1 && snapshot.tray.menu[0].enabled &&
                  !strcmp(snapshot.tray.menu[0].label, "Task Manager"));
            if (metric == 4) WmWindowMenuClose();
            else if (metric == 5) {
                const auto row = ChromeTrayMenuRowRect(WmChromeStyle(), geometry.popup, 0);
                CHECK(WmShellTrayPointer(sw, sh, row.x + 2, row.y + 2, true, 0, 0));
            } else CHECK(WmShellTrayPointer(sw, sh, 0, 0, true, 0, 0));
            CHECK(!WmWindowMenuOpen());
        }
        const auto disk = layout.metrics[2];
        CHECK(!ChromeTaskbarStatsContains(layout, disk.x, disk.y));
        const auto cpu = layout.metrics[0];
        CHECK(WmTrayFlyoutPress(cpu.x, cpu.y, sw, sh));
        const auto cpuPopup = WmStatsPopupLayout(sw, sh, DesktopStatsPanel::CpuMemory);
        CHECK(WmTrayFlyoutPress(cpuPopup.pin.x, cpuPopup.pin.y, sw, sh));
        CHECK(WmTaskbarPopupPinned());
        CHECK(WmShellTrayPointer(sw, sh, cpu.x, cpu.y, true, 1, 0));
        WmWindowMenuClose();
        CHECK(WmTaskbarPopupPinned() && WmTrayFlyoutVisible());
        CHECK(WmTrayFlyoutPress(cpuPopup.pin.x, cpuPopup.pin.y, sw, sh));
        WmTrayFlyoutClose();

        unsigned admitted = 0;
        uint64_t launchToken = 0;
        CHECK(g_launcherActive == &g_emptyLauncher);
        WmOpenTaskManager();
        for (unsigned i = 0; i < LaunchFeedbackTable::Capacity; ++i) {
            const auto& entry = g_launches.entries[i];
            if (entry.phase != LaunchPhase::Queued) continue;
            ++admitted;
            launchToken = entry.token;
            CHECK(!strcmp(entry.appId, TaskManagerAppId));
            CHECK(g_launchRequests[i].desktop && !g_launchRequests[i].userDesktop);
            CHECK(!strcmp(g_launchRequests[i].command,
                          "--uid 1000 /nix/profile/bin/brook-run /nix/profile/bin/brook-task-manager"));
        }
        CHECK(admitted == 1 && launchToken != 0);
        static Process manager{};
        manager.pid = manager.tgid = 100;
        manager.incarnation = 100;
        const int idx = WmCreateWindow(&manager, 10, 10, 300, 200, "CPU and memory");
        CHECK(idx >= 0);
        auto& window = g_windows[idx];
        WmStrCopy(window.appId, TaskManagerAppId, sizeof(window.appId));
        WmMinimizeWindow(idx);
        CHECK(window.minimized);
        WmOpenTaskManager();
        CHECK(g_focusedIdx == idx && window.focused && !window.minimized);
        CHECK(WmDockWindow(idx) && WmDockDismiss() && !WmDockOpen());
        WmOpenTaskManager();
        CHECK(WmDockOpen() && g_focusedIdx == idx && window.focused && window.docked);
        WmDestroyWindowById(&manager, window.wmId);
        for (auto& entry : g_launches.entries)
            if (entry.token == launchToken) entry = {};
        g_launcherWorkerStarted = g_launchBusy = false;
        g_taskbarPageTarget = -1;
        CHECK(WmSetTaskbarMetrics(saved));
    }
    std::puts("WINDOW_SURFACES PASS: stale slot, GL/SHM transition, destruction, reuse, owned geometry, chrome and fullscreen");
}
