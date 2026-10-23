// Execute the actual sys_write pipe branch. Real pipe storage/locking;
// only task blocking, signals and notifications are instrumented.
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include "../../kernel/src/pipe.h"

namespace brook {

enum class FdType { Pipe, Other };
struct FdEntry { FdType type=FdType::Pipe; void *handle=nullptr; uint64_t seekPos=0; unsigned flags=1, statusFlags=1; };
struct Process { uint64_t wakeupTick=0; SpinLock fdLock{}; };
static Process current;
static unsigned blocks, signals, wakes, notifications, processHolds;
static unsigned allocationHolds;
extern "C" void* kmalloc(uint64_t size) {void* p=std::malloc(size);if(p)++allocationHolds;return p;}
extern "C" void kfree(void* p) {if(p){if(!allocationHolds)throw std::runtime_error("bad allocation hold");--allocationHolds;std::free(p);}}
static void ProcessRef(Process*) {++processHolds;}
static void ProcessUnref(Process*) {if(!processHolds)throw std::runtime_error("bad process hold");--processHolds;;}
struct KernelOperationGuard {explicit KernelOperationGuard(Process*) {}};
static bool pending;
static std::function<void()> onBlock;
static Process *ProcessCurrent() { return &current; }
[[maybe_unused]] static SpinLock *ProcessFdLock(Process *p) { return &p->fdLock; }
static void ProcessSendSignal(Process *, int signal) { if (signal==13) ++signals; }
static void WakeProcess(Process *) { ++wakes; }
static void PipeWakeEpoll(PipeBuffer *) { ++notifications; }
static uint64_t ApicTickCount() { return 100; }
static bool HasPendingSignals() { return pending; }
struct UnexpectedBlock {};
static void SchedulerBlock(Process *) {
    ++blocks;
    if (!onBlock) throw UnexpectedBlock{};
    onBlock();
}
#include "../../kernel/src/pipe_poll.inc"
static int64_t writePipe(FdEntry *fde, const char *data, uint64_t count) {
    [[maybe_unused]] Process *proc = &current;
    uint64_t bufAddr = reinterpret_cast<uint64_t>(data);
#include "../../kernel/src/pipe_write.inc"
    return -EBADF;
}
}
using namespace brook;
static unsigned failures;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); ++failures; } } while (0)
int main() {
    char storage[64]{}, samples[128];
    std::memset(samples, 'x', sizeof(samples));
    PipeBuffer pipe;
    FdEntry fd;
    auto reset=[&](bool nonblocking) {
        pipe=PipeBuffer{};pipe.data=storage;pipe.capacity=sizeof(storage);
        pipe.readers=pipe.writers=1;fd.handle=&pipe;fd.statusFlags=1|(nonblocking?0x800:0);
        current=Process{};blocks=signals=wakes=notifications=0;pending=false;onBlock={};
    };
    reset(true);
    CHECK(pipe.write(samples,63)==63);
    const auto head=pipe.head,tail=pipe.tail;
    int64_t result=-999;
    try { result=writePipe(&fd,samples,1); } catch (const UnexpectedBlock&) {}
    CHECK(result==-EAGAIN && blocks==0);
    CHECK(pipe.head==head && pipe.tail==tail && !pipe.writerWaiter && !pipe.fullWriteWaits);
    CHECK(!signals && !notifications);

    // A second nonblocking writer must leave an existing waiter intact.
    Process other;
    pipe.writerWaiter=&other;
    CHECK(writePipe(&fd,samples,1)==-EAGAIN && pipe.writerWaiter==&other && !blocks);

    // The legacy bidirectional pipe representation writes to seekPos, not
    // the read-direction handle. Reject against the full direction only.
    char readStorage[64]{};
    PipeBuffer readPipe;readPipe.data=readStorage;readPipe.capacity=sizeof(readStorage);
    readPipe.readers=readPipe.writers=1;
    fd.flags=2;fd.handle=&readPipe;fd.seekPos=reinterpret_cast<uint64_t>(&pipe);
    CHECK(writePipe(&fd,samples,1)==-EAGAIN && readPipe.empty() && pipe.count()==63);
    CHECK(pipe.writerWaiter==&other && !blocks);
    fd.flags=1;fd.seekPos=0;

    // Prefix admission remains valid for requests larger than PIPE_BUF.
    reset(true);
    CHECK(writePipe(&fd,samples,8192)==63 && pipe.count()==63 && !blocks);
    CHECK(notifications==1);
    reset(true);pipe.readers=0;
    CHECK(writePipe(&fd,samples,1)==-EPIPE && signals==1 && !blocks);
    CHECK(writePipe(&fd,samples,0)==0 && signals==1 && !blocks);

    // Cubeb's control loop publishes two wake bytes and consumes one per
    // ready iteration. Saturation must drop nonblocking wake bytes rather
    // than park the only thread capable of draining this same pipe.
    reset(true);
    unsigned wouldBlock=0;
    for (unsigned i=0;i<128;++i) {
        char byte;
        if (!pipe.empty()) CHECK(pipe.read(&byte,1)==1);
        for (unsigned j=0;j<2;++j) {
            const auto n=writePipe(&fd,samples,1);
            CHECK(n==1 || n==-EAGAIN);
            wouldBlock+=n==-EAGAIN;
        }
    }
    CHECK(wouldBlock>0 && !blocks && pipe.count()==63 && !pipe.writerWaiter);

    // A blocking writer still waits, then preserves bytes once a reader frees
    // capacity. The simulated reader follows the existing notification path.
    reset(false);CHECK(pipe.write(samples,63)==63);
    onBlock=[&] {
        char byte;CHECK(pipe.read(&byte,1)==1 && byte=='x');
        auto *writer=pipe.writerWaiter;pipe.writerWaiter=nullptr;
        if (writer) WakeProcess(writer);
    };
    CHECK(writePipe(&fd,samples,1)==1 && blocks==1 && wakes==1);
    CHECK(pipe.count()==63 && pipe.fullWriteWaits==1 && !pipe.writerWaiter);
    // Named FIFO records up to PIPE_BUF are all-or-nothing under pressure.
    reset(true);fd.flags=1|0x10;CHECK(pipe.write(samples,61)==61);
    CHECK(writePipe(&fd,samples,3)==-EAGAIN && pipe.count()==61 && !blocks);
    reset(false);fd.flags=1|0x10;CHECK(pipe.write(samples,61)==61);
    onBlock=[&] {char byte;CHECK(pipe.read(&byte,1)==1);};
    CHECK(writePipe(&fd,samples,3)==3 && blocks==1 && pipe.count()==63);
    reset(false);fd.flags=1|0x10;CHECK(pipe.write(samples,63)==63);
    onBlock=[&] {pending=true;};
    CHECK(writePipe(&fd,samples,1)==-EINTR && !pipe.pollWaiters && !processHolds && !pipe.writerWaiter);
    // Surviving readers can drain and close without a stale writer registration.
    char byte;CHECK(pipe.read(&byte,1)==1 && !processHolds);
    CHECK(!allocationHolds && !processHolds);
    std::printf("PIPE_WRITE_ACTUAL_BODY %s failures=%u\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
