#include "syscall_timing.h"
#include "audio.h"
#include "process.h"
namespace brook {
namespace {
SyscallTimingModel model;
uint32_t guardWord=0;
uint64_t publishedEpoch=0,targetPid=0,targetIncarnation=0,contention=0,seenDrops=0;
// Short atomic-flag sections: no ticket reservation, no waiting, no callbacks.
// Mask local IRQs so asynchronous terminal dispatch cannot strand the guard.
struct Guard {
    uint64_t flags=0;bool held=false;
    Guard(){
        __asm__ volatile("pushfq; pop %0; cli":"=r"(flags)::"memory");
        uint32_t expected=0;held=__atomic_compare_exchange_n(&guardWord,&expected,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED);
    }
    ~Guard(){
        if(held)__atomic_store_n(&guardWord,0,__ATOMIC_RELEASE);
        if(flags&0x200)__asm__ volatile("sti":::"memory");
    }
};
void Drop(){
    auto old=__atomic_load_n(&contention,__ATOMIC_RELAXED);
    while(old!=~uint64_t(0) && !__atomic_compare_exchange_n(&contention,&old,old+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
}
void SyncDrops(){
    const auto count=__atomic_load_n(&contention,__ATOMIC_RELAXED);
    if(count!=seenDrops || count==~uint64_t(0)){model.BreakContinuity(count==~uint64_t(0));seenDrops=count;}
}
void Copy(SyscallTimingSnapshot* output){if(output){SyncDrops();*output=model.Snapshot();output->contended=__atomic_load_n(&contention,__ATOMIC_RELAXED);}}
void Publish(){
    const auto s=model.Snapshot();
    __atomic_store_n(&targetPid,s.pid,__ATOMIC_RELAXED);__atomic_store_n(&targetIncarnation,s.incarnation,__ATOMIC_RELAXED);
    __atomic_store_n(&publishedEpoch,s.epoch,__ATOMIC_RELEASE);
}
}
bool SyscallTimingWatch(uint64_t pid,uint64_t incarnation,SyscallTimingSnapshot* output){
    if(!pid || pid>65535 || !incarnation)return false;
    Guard guard;if(!guard.held)return false;
    __atomic_store_n(&publishedEpoch,0,__ATOMIC_RELEASE);model.Watch(pid,incarnation);seenDrops=__atomic_load_n(&contention,__ATOMIC_RELAXED);Publish();Copy(output);return true;
}
bool SyscallTimingOff(SyscallTimingSnapshot* output){
    Guard guard;if(!guard.held)return false;
    __atomic_store_n(&publishedEpoch,0,__ATOMIC_RELEASE);model.Off();Publish();Copy(output);return true;
}
bool SyscallTimingReset(SyscallTimingSnapshot* output){
    Guard guard;if(!guard.held)return false;
    __atomic_store_n(&publishedEpoch,0,__ATOMIC_RELEASE);model.Reset();seenDrops=__atomic_load_n(&contention,__ATOMIC_RELAXED);Publish();Copy(output);return true;
}
bool SyscallTimingRead(SyscallTimingSnapshot* output){
    if(!output)return false;Guard guard;if(!guard.held)return false;
    SyncDrops();*output=model.Snapshot();output->contended=__atomic_load_n(&contention,__ATOMIC_RELAXED);return true;
}
SyscallTimingTicket SyscallTimingEnter(uint64_t pid,uint64_t incarnation,uint64_t number,uint64_t arg0,SyscallTimingPhase phase){
    const auto epoch=__atomic_load_n(&publishedEpoch,__ATOMIC_ACQUIRE);
    if(!(epoch&1) || pid!=__atomic_load_n(&targetPid,__ATOMIC_RELAXED) ||
        incarnation!=__atomic_load_n(&targetIncarnation,__ATOMIC_RELAXED))return {};
    const auto now=AudioClockNowNs();Guard guard;if(!guard.held){Drop();return {};}
    SyncDrops();return model.Begin(epoch,pid,incarnation,number,arg0,now,phase);
}
void SyscallTimingLeave(SyscallTimingTicket ticket,int64_t result,bool hasResult){
    Process* proc=ProcessCurrent();if(!proc)return;
    const auto now=AudioClockNowNs();Guard guard;if(!guard.held){Drop();return;}
    SyncDrops();if(model.Matches(proc->pid,proc->incarnation))model.End(ticket,now,result,hasResult);
}
}
