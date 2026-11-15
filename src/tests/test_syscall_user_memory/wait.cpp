// H-26 / M-18: wakeup sources for sync files, pipe POLLOUT and eventfds,
// through the real PollWait, read() and write() paths.
#include "harness.h"
#include "syscall_poll.cpp"
#include "syscall_eventfd.cpp"
// The real pipe endpoints and poll subscriptions (PipePollNotify).
#include "syscall_pipe.cpp"
#include "syscall_handlers.h"

namespace brook { extern bool g_pendingSignals; }
using namespace harness;
namespace {
brook::Process* g_woken[8];
unsigned g_wakes;
void CountWake(brook::Process* p) { if(g_wakes<8)g_woken[g_wakes]=p;++g_wakes; }
bool Woke(brook::Process* p) { for(unsigned i=0;i<g_wakes && i<8;++i)if(g_woken[i]==p)return true;return false; }
uint64_t g_blockTick;
brook::PipeBuffer* g_drainOnBlock;
void RecordBlock(brook::Process* p) {
    g_blockTick=p->wakeupTick;
    if(g_drainOnBlock) { char sink[16];(void)g_drainOnBlock->read(sink,sizeof(sink)); }
    brook::g_pendingSignals=true; // end the wait after one block
}
brook::Process* OtherProcess() {
    static brook::Process other{};other.magic=brook::PROCESS_MAGIC;other.pid=9;return &other;
}
}

// A pending Intel fence: poll must not sleep the default 50 ms safety tick.
static void PollSyncFile() {
    Reset();ResetProcess();auto* proc=CurrentProcess();
    brook::SyncFileData fence{1,0,1,brook::BdwFenceCreate(false)};
    proc->fds[3].type=brook::FdType::SyncFile;proc->fds[3].handle=&fence;
    g_blockInterruptibleHook=RecordBlock;g_blockTick=0;
    brook::pollfd fds[1]{{3,brook::POLLIN,0}};
    CHECK(brook::PollWait(fds,1,1000)==-brook::EINTR);
    // Prompt wakes cannot arm on the host, so the wait polls every tick.
    CHECK(g_blockTick==brook::ApicTickCount()+1);
    // A fence that retires before the wait arms is rechecked, not slept on.
    brook::BdwFenceComplete(fence.intelFence,true);
    g_blockTick=0;brook::g_pendingSignals=false;
    CHECK(brook::PollWait(fds,1,1000)==1 && fds[0].revents==brook::POLLIN);
    CHECK(g_blockTick==0);
    brook::BdwFenceUnref(fence.intelFence);
    // The wait helper: a retirement between snapshot and arm means "recheck".
    brook::SyncFileData pending{1,0,2,brook::BdwFenceCreate(false)};
    {
        brook::SyncFileWait wait;wait.Want(pending);
        brook::BdwGpuEventSignal(brook::BdwGpuEvent::Retire);
        CHECK(wait.Arm());
    }
    {
        brook::SyncFileWait wait;
        CHECK(!wait.Arm() && wait.SafetyMs(50)==50); // nothing pending: unchanged
    }
    brook::BdwFenceUnref(pending.intelFence);
}

// poll(POLLOUT) on a full pipe write end is woken when a reader drains it.
static void PollPipeWriteEnd() {
    Reset();ResetProcess();auto* proc=CurrentProcess();
    auto* pipe=brook::PipeBufferCreate(16);pipe->readers=pipe->writers=1;
    char fill[15];__builtin_memset(fill,'x',sizeof(fill));
    CHECK(pipe->write(fill,sizeof(fill))==15 && pipe->space()==0);
    proc->fds[4].type=brook::FdType::Pipe;proc->fds[4].handle=pipe;proc->fds[4].flags=1;
    g_wakes=0;g_wakeHook=CountWake;g_blockInterruptibleHook=RecordBlock;g_drainOnBlock=pipe;
    brook::pollfd fds[1]{{4,static_cast<short>(4),0}};
    (void)brook::PollWait(fds,1,1000);
    CHECK(Woke(proc));
    g_drainOnBlock=nullptr;
}

// Several waiters on one eventfd all get the wake from one write.
static void EventFdWakesEveryWaiter() {
    Reset();ResetProcess();auto* proc=CurrentProcess();
    CHECK(brook::sys_eventfd2(0,0x800,0,0,0,0)==0);
    auto* efd=static_cast<brook::EventFdData*>(proc->fds[0].handle);
    // Two pollers (as poll and epoll_wait register) and a blocked reader.
    brook::EventFdWaitGroup pollerA,pollerB;
    CHECK(pollerA.add(efd,proc) && pollerB.add(efd,OtherProcess()));
    g_wakes=0;g_wakeHook=CountWake;
    const uint64_t one=1;
    __builtin_memcpy(reinterpret_cast<void*>(UserAddress(0)),&one,8);
    CHECK(brook::sys_write(0,UserAddress(0),8,0,0,0)==8);
    CHECK(Woke(proc) && Woke(OtherProcess()));
    // Reading frees capacity: POLLOUT waiters are woken too.
    g_wakes=0;
    CHECK(brook::sys_read(0,UserAddress(64),8,0,0,0)==8);
    CHECK(Woke(OtherProcess()));
    g_wakeHook=nullptr;
}

void RunWaitTests() {
    PollSyncFile();
    PollPipeWriteEnd();
    EventFdWakesEveryWaiter();
}
