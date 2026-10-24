#include <cstdio>
#include <cstdlib>
#include "../../kernel/src/window.cpp"

// Exercise the production slot lifecycle and surface publication entry points.
// No renderer or VFB allocation runs here; their actual GPU behavior is a VM gate.
namespace {
unsigned lockDepth=0, wakes=0, dirty=0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"WINDOW_SURFACES line %d: %s\n",__LINE__,#x); std::abort(); } } while (0)
}
namespace brook {
extern "C" void SerialPuts(const char*) {}
extern "C" void SerialPrintf(const char*,...) {}
void KMutexLock(KMutex*) { CHECK(lockDepth++ == 0); }
void KMutexUnlock(KMutex*) { CHECK(lockDepth-- == 1); }
extern "C" void CompositorWake() { ++wakes; }
void CompositorMarkDirty() { ++dirty; }
Process* ProcessCurrent() { std::abort(); }
int64_t UserMappingRemoveBacking(Process*,AddressSpaceState*,MappingBacking*) { std::abort(); }
void MappingBackingUnref(MappingBacking*) { std::abort(); }
void AddressSpaceUnref(AddressSpaceState*) { std::abort(); }
void CompositorRetireVfb(CompositorVfbAllocation*) { std::abort(); }
}
extern "C" void KernelPanic(const char*,...) { std::abort(); }

int main() {
    using namespace brook;
    auto* owner=static_cast<Process*>(std::calloc(1,sizeof(Process)));
    auto* stranger=static_cast<Process*>(std::calloc(1,sizeof(Process)));
    CHECK(owner && stranger);owner->pid=owner->tgid=7;stranger->pid=stranger->tgid=8;
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
    std::free(owner);std::free(stranger);
    std::puts("WINDOW_SURFACES PASS: stale slot, GL/SHM transition, destruction, reuse, owned geometry and chrome");
}
