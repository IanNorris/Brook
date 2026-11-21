#include "intel_bdw_wait_queue.h"
#include "intel_bdw_prompt_wake.h"
#include "apic.h"
#include "process.h"
#include "profiler.h"
#include "scheduler.h"
#include "spinlock.h"

namespace brook {
namespace {
// Waiters are threads inside an Intel ioctl, the gpu-submit worker or a
// presenter; each is alive for as long as it stays armed. The lock is a
// leaf except for the scheduler ready lock taken inside SchedulerUnblock.
constexpr unsigned GpuWaitCapacity=32;
struct GpuEventQueue {SpinLock lock{};BdwWaitSlots<Process,GpuWaitCapacity> slots{};};
GpuEventQueue gpuEvents[uint32_t(BdwGpuEvent::Count)];
GpuEventQueue* Queue(uint32_t event){return event<uint32_t(BdwGpuEvent::Count)?&gpuEvents[event]:nullptr;}
}

uint64_t BdwGpuEventGeneration(BdwGpuEvent event){
    auto* q=Queue(uint32_t(event));return q?q->slots.Generation():0;
}

BdwWaitArm BdwGpuEventArm(BdwGpuEvent event,uint64_t seen,BdwGpuWaiter& waiter){
    waiter={};auto* q=Queue(uint32_t(event));auto* self=ProcessCurrent();
    if(!q || !self)return BdwWaitArm::Full;
    // Kill switch: while prompt wakes are off every caller takes its plain
    // timed sleep, exactly the pre-M4a polling behaviour.
    if(!BdwPromptWakeEnabled()){BdwPromptWakeCount(BdwPromptWake().refused);return BdwWaitArm::Full;}
    SpinLockAcquire(&q->lock);
    unsigned slot=GpuWaitCapacity;const auto result=q->slots.Arm(self,seen,slot);
    SpinLockRelease(&q->lock);
    if(result==BdwWaitArm::Armed){waiter.event=uint32_t(event);waiter.slot=slot;waiter.process=self;BdwPromptWakeCount(BdwPromptWake().armed);}
    else if(result==BdwWaitArm::Stale)BdwPromptWakeCount(BdwPromptWake().stale);
    return result;
}

void BdwGpuEventDisarm(BdwGpuWaiter& waiter){
    auto* q=Queue(waiter.event);
    if(q && waiter.process){
        SpinLockAcquire(&q->lock);
        (void)q->slots.Disarm(static_cast<Process*>(waiter.process),waiter.slot);
        SpinLockRelease(&q->lock);
    }
    waiter={};
}

// Wakes are issued under the queue lock: after Disarm returns, this queue
// can no longer unblock the thread, so it cannot leak a wake into a later,
// unrelated sleep (for example a KSemaphore wait on gpu-present).
void BdwGpuEventSignal(BdwGpuEvent event, uint64_t fenceId){
    auto* q=Queue(uint32_t(event));if(!q)return;
    SpinLockAcquire(&q->lock);
    q->slots.Signal([&](Process* p) {
#ifdef BROOK_GPU_SUBMISSION_TIMING
        ProfilerTraceClientWait(p, FrameGpuWake, fenceId, static_cast<uint32_t>(event));
#endif
        SchedulerUnblock(p);
    });
    SpinLockRelease(&q->lock);
}

void BdwGpuEventWaitTick(BdwGpuEvent event,uint64_t seen){
    auto* self=ProcessCurrent();
    if(!self){SchedulerSleepMs(1);return;}
    BdwGpuWaiter waiter{};
    const auto armed=BdwGpuEventArm(event,seen,waiter);
    if(armed==BdwWaitArm::Stale)return;
    if(armed==BdwWaitArm::Full){SchedulerSleepMs(1);return;}
    // Same one-tick deadline as SchedulerSleepMs(1). A signal racing ahead
    // of the block leaves pendingWakeup set, so SchedulerBlock returns.
    self->wakeupTick=ApicTickCount()+1;
    SchedulerBlock(self);
    BdwGpuEventDisarm(waiter);
}
}
