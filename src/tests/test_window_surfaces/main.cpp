#include <cstdio>
#include <cstdlib>
#include "../../kernel/src/window.cpp"

// Exercise the production slot lifecycle and surface publication entry points.
// No renderer or VFB allocation runs here; their actual GPU behavior is a VM gate.
namespace {
unsigned lockDepth=0, wakes=0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"WINDOW_SURFACES line %d: %s\n",__LINE__,#x); std::abort(); } } while (0)
}
namespace brook {
extern "C" void SerialPuts(const char*) {}
extern "C" void SerialPrintf(const char*,...) {}
void KMutexLock(KMutex*) { CHECK(lockDepth++ == 0); }
void KMutexUnlock(KMutex*) { CHECK(lockDepth-- == 1); }
extern "C" void CompositorWake() { ++wakes; }
void CompositorMarkDirty() {}
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
    g_windows[0].externalGres=65537;g_windows[0].externalW=1280;g_windows[0].externalH=720;
    int slot=WmCreateWindow(owner,0,0,640,480,"GL",1,false);
    CHECK(slot==0);auto& window=g_windows[slot];
    CHECK(!window.externalGres && !window.externalW && !window.externalH);
    const uint32_t id=window.wmId;
    CHECK(WmPresentExternalById(owner,id,65538,1280,720));
    CHECK(window.externalGres==65538 && window.externalW==1280 && window.externalH==720);
    CHECK(window.vfbDirty && wakes==1 && !lockDepth);
    CHECK(!WmPresentExternalById(stranger,id,1,1,1));
    WmSignalDirtyById(stranger,id);
    CHECK(window.externalGres==65538 && wakes==1);
    // A live surface may switch from a GL dmabuf to a CPU-written SHM buffer.
    WmSignalDirtyById(owner,id);
    CHECK(!window.externalGres && !window.externalW && !window.externalH);
    CHECK(window.vfbDirty && wakes==2 && !lockDepth);
    CHECK(WmPresentExternalById(owner,id,65539,800,600));
    WmDestroyWindowById(stranger,id);CHECK(window.proc==owner);
    WmDestroyWindowById(owner,id);
    CHECK(!window.proc && !window.externalGres && !window.externalW && !window.externalH);
    CHECK(!WmPresentExternalById(owner,id,65540,640,480));
    CHECK(WmCreateWindow(stranger,10,10,640,480,"Chromium SHM",1,false)==slot);
    CHECK(!window.externalGres && !window.externalW && !window.externalH);
    CHECK(!WmPresentExternalById(owner,id,65541,640,480));
    WmSignalDirtyById(stranger,id);CHECK(window.vfbDirty);
    CHECK(!lockDepth);
    std::free(owner);std::free(stranger);
    std::puts("WINDOW_SURFACES PASS: stale slot, GL/SHM transition, destruction, reuse, owner checks");
}
