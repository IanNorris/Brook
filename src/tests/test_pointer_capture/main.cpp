#include <cstdio>
#include <cstdlib>
#include "../../kernel/src/window.cpp"

// Real window lifecycle, input rings and capture entry points. No renderer or
// device is mocked into claiming physical capture; that remains a VM gate.
namespace {
unsigned lockDepth=0;
uint64_t sequence=10,drops=0;
int32_t mouseX=100,mouseY=100;
uint8_t mouseButtons=0;
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"POINTER_CAPTURE %d: %s\n",__LINE__,#x);std::abort(); } } while(0)
}
namespace brook {
extern "C" void SerialPuts(const char*) {}
extern "C" void SerialPrintf(const char*,...) {}
void KMutexLock(KMutex*) { CHECK(lockDepth++==0); }
void KMutexUnlock(KMutex*) { CHECK(lockDepth--==1); }
extern "C" void CompositorWake() {}
void CompositorMarkDirty() {}
extern "C" void MouseGetPosition(int32_t* x,int32_t* y) { *x=mouseX;*y=mouseY; }
extern "C" void MouseSetPosition(int32_t x,int32_t y) { mouseX=x;mouseY=y; }
extern "C" uint8_t MouseGetButtons() { return mouseButtons; }
extern "C" uint64_t InputNextSequence(bool dropped) { if(dropped)++drops;return ++sequence; }
uint64_t InputLatestSequence() { return sequence; }
uint64_t InputDroppedEvents() { return drops; }
Process* ProcessCurrent() { return nullptr; }
int64_t UserMappingRemoveBacking(Process*,AddressSpaceState*,MappingBacking*) { std::abort(); }
void MappingBackingUnref(MappingBacking* p) { CHECK(!p); }
void AddressSpaceUnref(AddressSpaceState* p) { CHECK(!p); }
void CompositorRetireVfb(CompositorVfbAllocation* p) { CHECK(!p); }
}
extern "C" void KernelPanic(const char*,...) { std::abort(); }

int main() {
    using namespace brook;
    auto* owner=static_cast<Process*>(std::calloc(1,sizeof(Process)));
    auto* stranger=static_cast<Process*>(std::calloc(1,sizeof(Process)));
    CHECK(owner && stranger);owner->pid=7;stranger->pid=8;
    g_wmActive=true;
    uint32_t pixel=0;
    int slot=WmCreateWindow(owner,0,0,640,480,"Capture",1,true);
    CHECK(slot>=0);auto* w=&g_windows[slot];const uint32_t id=w->wmId;
    CHECK(WmPointerCapture(owner,id,1,0)==-11); // Not mapped.
    w->vfb=&pixel;
    CHECK(WmPointerCapture(stranger,id,1,0)==-2);
    CHECK(WmPointerCapture(owner,id,1,1)==-22);
    CHECK(WmPointerCapture(owner,id,3,0)==-22);
    int64_t lease=WmPointerCapture(owner,id,1,0);
    CHECK(lease>0 && WmPointerCaptureActive());
    CHECK(WmPointerCapture(owner,id,0,lease)==lease);
    CHECK(WmPointerCapture(owner,id,2,lease+1)==-116 && WmPointerCaptureActive());
    Window::WmInputEvent queue[256]{};
    WmInputPop(w,queue,256); // Initial focus event.
    InputEvent event{};event.type=InputEventType::MouseRelativeMove;
    event.deltaX=1000;event.deltaY=-800;event.sequence=sequence;
    CHECK(WmRouteCapturedPointer(event) && WmInputPop(w,queue,256)==0); // Before grant.
    event.sequence=++sequence;
    CHECK(WmRouteCapturedPointer(event));
    event.type=InputEventType::MouseButtonDown;event.scanCode=0;event.sequence=++sequence;
    CHECK(WmRouteCapturedPointer(event));
    event.type=InputEventType::MouseRelativeMove;event.deltaX=-123;event.deltaY=321;event.sequence=++sequence;
    CHECK(WmRouteCapturedPointer(event));
    CHECK(WmInputPop(w,queue,256)==3);
    CHECK(queue[0].type==6 && queue[0].x==1000 && queue[0].y==-800);
    CHECK(queue[1].type==3 && queue[2].type==6 && queue[2].x==-123 && queue[2].y==321);
    for(unsigned i=0;i<3;++i)CHECK(queue[i].reserved==lease);
    event.deltaX=1000000;event.deltaY=-999999;event.sequence=++sequence;
    CHECK(WmRouteCapturedPointer(event));
    unsigned count=WmInputPop(w,queue,256);CHECK(count>1);
    int64_t x=0,y=0;
    for(unsigned i=0;i<count;++i) { CHECK(queue[i].type==6 && queue[i].reserved==lease);x+=queue[i].x;y+=queue[i].y; }
    CHECK(x==1000000 && y==-999999); // No int16 wrapping or last-position overwrite.
    event.type=InputEventType::MouseMove;event.sequence=++sequence;
    CHECK(WmRouteCapturedPointer(event) && !WmInputPop(w,queue,256)); // Absolute motion suppressed.
    // Already-published capture-era events must not hit the next window;
    // a button held across revocation remains quarantined through physical up.
    InputEvent queuedMove{};queuedMove.type=InputEventType::MouseRelativeMove;
    queuedMove.deltaX=44;queuedMove.sequence=++sequence;
    InputEvent queuedUp{};queuedUp.type=InputEventType::MouseButtonUp;
    queuedUp.sequence=++sequence;mouseButtons=1;
    mouseX=1919;mouseY=1079;
    CHECK(WmPointerCapture(owner,id,2,lease)==0 && !WmPointerCaptureActive());
    CHECK(mouseX==100 && mouseY==100);
    CHECK(WmPointerSuppressedButtons()==1);
    CHECK(WmRouteCapturedPointer(queuedMove) && WmRouteCapturedPointer(queuedUp));
    CHECK(!WmInputPop(w,queue,256));
    CHECK(WmPointerCapture(owner,id,1,0)==-11);
    InputEvent held{};held.type=InputEventType::MouseButtonDown;held.sequence=++sequence;
    CHECK(WmRouteCapturedPointer(held));
    held.type=InputEventType::MouseButtonUp;held.sequence=++sequence;mouseButtons=0;
    CHECK(WmRouteCapturedPointer(held) && !WmPointerSuppressedButtons());

    // Physical Up can precede draining its event. Refresh must not let that
    // unmatched release reach a new client, even when published after cutoff.
    lease=WmPointerCapture(owner,id,1,0);CHECK(lease>0);mouseButtons=1;
    CHECK(WmPointerCapture(owner,id,2,lease)==0);
    held.type=InputEventType::MouseButtonUp;held.sequence=++sequence;mouseButtons=0;
    WmPointerCaptureRefresh();CHECK(!WmPointerSuppressedButtons());
    CHECK(WmRouteCapturedPointer(held));
    held.sequence=++sequence;CHECK(WmRouteCapturedPointer(held)); // Delayed publication.
    held.type=InputEventType::MouseButtonDown;held.sequence=++sequence;
    CHECK(!WmRouteCapturedPointer(held));
    held.type=InputEventType::MouseButtonUp;held.sequence=++sequence;
    CHECK(!WmRouteCapturedPointer(held)); // New pair remains intact.
    // A genuinely dropped Up also permits reacquisition; its tombstone must
    // not eat the release belonging to the next, fresh press.
    lease=WmPointerCapture(owner,id,1,0);CHECK(lease>0);mouseButtons=1;
    CHECK(WmPointerCapture(owner,id,2,lease)==0);mouseButtons=0;
    WmPointerCaptureRefresh();CHECK(!WmPointerSuppressedButtons());
    held.type=InputEventType::MouseButtonDown;held.sequence=++sequence;
    CHECK(!WmRouteCapturedPointer(held));
    held.type=InputEventType::MouseButtonUp;held.sequence=++sequence;
    CHECK(!WmRouteCapturedPointer(held));

    const auto first=lease;lease=WmPointerCapture(owner,id,1,0);CHECK(lease>first);
    CHECK(!WmPointerCapture(owner,id,0,first));
    CHECK(WmPointerCapture(owner,id,2,first)==-116 && WmPointerCaptureActive());
    const int other=WmCreateWindow(owner,20,20,320,240,"Other",1,true);
    CHECK(other>=0 && !WmPointerCaptureActive() && !WmPointerCapture(owner,id,0,0));
    CHECK(WmPointerCapture(owner,id,1,0)==-11);
    WmSetFocus(slot);lease=WmPointerCapture(owner,id,1,0);CHECK(lease>first);
    WmMinimizeWindow(slot);CHECK(!WmPointerCaptureActive());
    WmRestoreWindow(slot);lease=WmPointerCapture(owner,id,1,0);CHECK(lease>0);
    WmDestroyWindowById(owner,id);CHECK(!WmPointerCaptureActive());
    slot=WmCreateWindow(owner,0,0,640,480,"Reused",1,true);w=&g_windows[slot];
    CHECK(w->wmId==id);w->vfb=&pixel;
    const auto fresh=WmPointerCapture(owner,id,1,0);CHECK(fresh>lease);
    CHECK(WmPointerCapture(owner,id,2,lease)==-116 && WmPointerCaptureActive());
    WmInputPop(w,queue,256);
    event={};event.type=InputEventType::MouseButtonDown;
    for(unsigned i=0;i<255;++i)CHECK(WmInputPush(w,event,0,0));
    event.type=InputEventType::MouseRelativeMove;event.deltaX=1;event.sequence=++sequence;
    CHECK(WmRouteCapturedPointer(event) && !WmPointerCaptureActive()); // Queue loss revokes.
    CHECK(WmInputPop(w,queue,256)==255);
    lease=WmPointerCapture(owner,id,1,0);CHECK(lease>0);
    InputDevice device{};
    event.type=InputEventType::MouseRelativeMove;event.deltaX=123;event.deltaY=-45;
    for(unsigned i=0;i<INPUT_RING_SIZE;++i)InputDevicePush(&device,event);
    CHECK(drops==1);WmPointerCaptureRefresh();CHECK(!WmPointerCaptureActive());
    uint64_t last=0;count=0;
    while(InputDevicePop(&device,&event)) {
        CHECK(event.sequence>last && event.deltaX==123 && event.deltaY==-45);
        last=event.sequence;++count;
    }
    CHECK(count==INPUT_RING_SIZE-1);
    lease=WmPointerCapture(owner,id,1,0);CHECK(lease>0);
    event={};event.type=InputEventType::KeyPress;event.scanCode=1;
    CHECK(!WmRouteCapturedPointer(event)); // Ordinary Escape remains app input.
    event.modifiers=INPUT_MOD_CTRL|INPUT_MOD_ALT;
    CHECK(WmRouteCapturedPointer(event) && !WmPointerCaptureActive());
    CHECK(WmRouteCapturedPointer(event)); // Key-repeat Escape is quarantined too.
    event.type=InputEventType::KeyRelease;CHECK(WmRouteCapturedPointer(event));
    CHECK(!WmRouteCapturedPointer(event));
    lease=WmPointerCapture(owner,id,1,0);CHECK(lease>0);
    mouseButtons=1;
    event={};event.type=InputEventType::MouseRelativeMove;event.sequence=++sequence;
    { VfbGuard guard;SetWmActiveLocked(false); }
    CHECK(!WmPointerCaptureActive() && !g_wmActive); // No further input/query needed.
    CHECK(WmRouteCapturedPointer(event)); // Legacy drain uses the same filter.
    held.type=InputEventType::MouseButtonUp;held.sequence=++sequence;mouseButtons=0;
    WmPointerCaptureRefresh();CHECK(WmRouteCapturedPointer(held));
    held.sequence=++sequence;CHECK(WmRouteCapturedPointer(held));
    held.type=InputEventType::MouseButtonDown;held.sequence=++sequence;
    CHECK(!WmRouteCapturedPointer(held));
    held.type=InputEventType::MouseButtonUp;held.sequence=++sequence;
    CHECK(!WmRouteCapturedPointer(held));
    { VfbGuard guard;SetWmActiveLocked(true); }
    g_nextPointerCaptureToken=0xffffffffu;
    CHECK(WmPointerCapture(owner,id,1,0)==-75 && !WmPointerCaptureActive());
    w->vfb=nullptr;WmDestroyWindowById(owner,id);WmDestroyWindow(other);
    std::free(owner);std::free(stranger);CHECK(!lockDepth);
    std::puts("POINTER_CAPTURE PASS: ownership, ordering, signed totals, focus/death/reuse, overflow, emergency release");
}
