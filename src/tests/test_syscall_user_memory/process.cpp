// clone/vfork (H-07, H-23, H-25): real sys_clone over a fake process layer.
#include "harness.h"
#include "syscall_process.cpp"
#include <stdlib.h>

using namespace harness;
namespace {
unsigned g_forks,g_threads,g_adds,g_blocks;
brook::Process* g_child;
brook::Process* g_vforkParentWhenRunnable;
// What the child does as soon as it is runnable, before the parent blocks.
void (*g_childRunsFirst)(brook::Process*);
brook::Process* NewChild() {
    auto* child=static_cast<brook::Process*>(calloc(1,sizeof(brook::Process)));
    child->magic=brook::PROCESS_MAGIC;child->pid=8;child->tgid=8;
    return child;
}
}
namespace brook {
Process* ProcessFork(Process*,uint64_t,uint64_t,uint64_t,bool) { ++g_forks;return g_child=NewChild(); }
Process* ProcessCreateThread(Process*,uint64_t,uint64_t,uint64_t,uint64_t,bool) { ++g_threads;return g_child=NewChild(); }
int ProcessUserNamespaceCreate(Process*) { return 0; }
UserNamespace* ProcessUserNamespaceAcquire(Process*) { return nullptr; }
bool ProcessCapable(Process*,UserNamespace*,unsigned) { return false; }
int ProcessFsIsolateMounts(Process*) { return 0; }
int SysvSemClone(Process*,Process*,bool) { return 0; }
void SysvSemExit(Process*) {}
void ProcessDiscardUnpublished(Process*) {}
void ResetSignalActions(uint16_t,bool) {}
void SchedulerAddProcess(Process* child) {
    ++g_adds;
    g_vforkParentWhenRunnable=child->vforkParent;
    if(g_childRunsFirst)g_childRunsFirst(child);
}
}
// Overrides the shared stub for this TU's waits: count blocks and give up
// (terminate the parent) if nothing ever releases it.
static void BlockedParent(brook::Process* parent) {
    if(++g_blocks>3)parent->terminalPending=true;
}

static void Begin() {
    Reset();ResetProcess();
    g_forks=g_threads=g_adds=g_blocks=0;g_child=nullptr;g_vforkParentWhenRunnable=nullptr;g_childRunsFirst=nullptr;
    extern void (*g_schedulerBlockHook)(brook::Process*);
    g_schedulerBlockHook=BlockedParent;
}

static void SetTls() {
    Begin();
    constexpr uint64_t thread=0x00000100|0x00000200|0x00000400|0x00000800|0x00010000|0x00080000;
    // Non-canonical and kernel-half TLS bases are refused before any task exists.
    CHECK(brook::sys_clone(thread,UserAddress(PageSize),0,0,0x8000000000000000ULL,0)==-brook::EPERM);
    CHECK(brook::sys_clone(thread,UserAddress(PageSize),0,0,0xffff800000001000ULL,0)==-brook::EPERM);
    CHECK(brook::sys_clone(0x00080000|17,0,0,0,0x8000000000000000ULL,0)==-brook::EPERM);
    CHECK(g_threads==0 && g_forks==0);
    // clone3 delegates to the same check.
    struct { uint64_t flags,pidfd,child_tid,parent_tid,exit_signal,stack,stack_size,tls; } args{};
    args.flags=thread;args.tls=0x8000000000000000ULL;
    __builtin_memcpy(reinterpret_cast<void*>(UserAddress(0)),&args,sizeof(args));
    CHECK(brook::sys_clone3(UserAddress(0),sizeof(args),0,0,0,0)==-brook::EPERM);
    CHECK(g_threads==0);
    // A canonical user base still creates the thread.
    CHECK(brook::sys_clone(thread,UserAddress(PageSize),0,0,UserAddress(2*PageSize),0)==8);
    CHECK(g_threads==1);
    CHECK(brook::sys_arch_prctl(0x1002,0x8000000000000000ULL,0,0,0,0)==-brook::EPERM);
}

static void VforkChildFirst() {
    Begin();
    auto* parent=CurrentProcess();
    // The child becomes runnable with its parent already published, so an
    // exec or exit before the parent blocks still releases it.
    g_childRunsFirst=[](brook::Process* child) { brook::ProcessVforkRelease(child); };
    const int32_t before=parent->refCount;
    CHECK(brook::sys_vfork(0,0,0,0,0,0)==8);
    CHECK(g_vforkParentWhenRunnable==parent);
    CHECK(g_blocks==0);
    CHECK(!parent->terminalPending);
    CHECK(parent->vforkWaiting==0);
    CHECK(g_child->vforkParent==nullptr);
    CHECK(parent->refCount==before);
}

static void VforkParentWaitsForRelease() {
    Begin();
    auto* parent=CurrentProcess();
    // Spurious wakes (any SchedulerUnblock) do not end the wait; only the
    // release does. Here nothing releases, so the wait runs until the
    // parent is terminated rather than returning after the first wake.
    CHECK(brook::sys_vfork(0,0,0,0,0,0)==8);
    CHECK(g_vforkParentWhenRunnable==parent);
    CHECK(g_blocks==4);
    CHECK(parent->vforkWaiting==1);
    // The release is exactly once and drops the child's parent reference.
    const int32_t held=parent->refCount;
    brook::ProcessVforkRelease(g_child);
    brook::ProcessVforkRelease(g_child);
    CHECK(parent->vforkWaiting==0);
    CHECK(parent->refCount==held-1);
}

static void ChildTid() {
    Begin();
    constexpr uint64_t fork=17|0x01000000|0x00200000|0x00100000; // CHILD_SETTID|CHILD_CLEARTID|PARENT_SETTID
    const uint64_t childTid=UserAddress(0),parentTid=UserAddress(64);
    CHECK(brook::sys_clone(fork,0,parentTid,childTid,0,0)==8);
    // Fork-style clones honour all three TID flags.
    CHECK(g_child->clearChildTid==childTid);
    CHECK(g_child->setChildTid==childTid);
    CHECK(*reinterpret_cast<const int32_t*>(parentTid)==8);
    // A bad PARENT_SETTID address on a fork is ignored, as on Linux.
    CHECK(brook::sys_clone(17|0x00100000,0,g_kernel.Address(),0,0,0)==8);
    CHECK(g_kernel.Intact());
}

void RunProcessTests() {
    SetTls();
    VforkChildFirst();
    VforkParentWaitsForRelease();
    ChildTid();
    extern void (*g_schedulerBlockHook)(brook::Process*);
    g_schedulerBlockHook=nullptr;
}
