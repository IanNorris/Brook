// Actual finalizer body, with resource backends counted instead of freed.
// A failed unpublished clone must never release an existing sibling's AS slot.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
namespace brook {
struct Process {bool isThread=false,isKernelThread=false;void* fds=nullptr;int gpuAppCtx=0;uint64_t kernelStackBase=0;unsigned pid=0,tgid=0;};
struct VirtualAddress {explicit VirtualAddress(uint64_t){}};
constexpr unsigned KERNEL_STACK_PAGES=4;
static unsigned membership=3,removed=0,freedPid=0,closed=0,freed=0,stacks=0,undo=0,windows=0,contexts=0,lastContext=0;
static void PosixTimersCleanup(Process*,bool){}
static void SysvSemExit(Process*){++undo;}
static void ProcessReleaseFileFault(Process*){}
static void ProcessCloseAllFds(Process*){++closed;}
static void kfree(void*){}
static void CompositorUnregisterProcess(Process*){++windows;}
static void WmDestroyWindowForProcess(Process*){}
struct GpuAppOps {void (*CtxDestroy)(int);};
static void DestroyContext(int id){++contexts;lastContext=unsigned(id);}
static const GpuAppOps* GpuAppGet(){static GpuAppOps ops{DestroyContext};return &ops;}
static void VmmFreeKernelStack(VirtualAddress,unsigned){++stacks;}
static void ResetSignalActions(unsigned){}
static void SchedulerRemoveProcess(Process*){++removed;}
static void SchedulerFreePid(unsigned){++freedPid;}
static void SchedulerReleaseThreadAddressSpace(Process*){--membership;}
static void FreeProcessStruct(Process*){++freed;}
#include "../../kernel/src/process_destroy.inc"
}
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(){
    using namespace brook;
    Process child;child.isThread=true;child.pid=9;child.tgid=1;child.kernelStackBase=123;child.fds=&child;
    ProcessDiscardUnpublished(&child);
    CHECK(membership==3 && removed==0 && freedPid==1 && freed==1 && stacks==1 && closed==0 && windows==0 && undo==1 && !child.fds);
    Process publishedChild;publishedChild.isThread=true;publishedChild.pid=8;publishedChild.tgid=1;
    ProcessDestroyNow(&publishedChild);
    CHECK(membership==2 && removed==1 && freedPid==1 && freed==2 && undo==2);
    Process fork;fork.pid=fork.tgid=10;fork.fds=&fork;fork.kernelStackBase=123;fork.gpuAppCtx=77; // borrowed parent context
    ProcessDiscardUnpublished(&fork);
    CHECK(membership==2 && removed==1 && freedPid==2 && freed==3 && closed==1 && windows==0 && contexts==0 && fork.gpuAppCtx==77 && stacks==2 && undo==3);
    Process owner;owner.pid=owner.tgid=11;owner.gpuAppCtx=88;
    ProcessDestroyNow(&owner);CHECK(contexts==1 && lastContext==88 && windows==1);
    ProcessDiscardUnpublished(nullptr);CHECK(freed==4);
    std::puts("Process published/unpublished cleanup: PASS");
}
