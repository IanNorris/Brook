#include "eevdf_policy.h"
namespace {
using namespace brook;
static EevdfPolicy& Policy(void* state){return *static_cast<EevdfPolicy*>(state);}
static void Init(void* s){Policy(s).Init();}
static void InitProcess(void* s,uint16_t pid,uint8_t priority){Policy(s).InitProcess(pid,priority);}
static void Enqueue(void* s,uint16_t pid){Policy(s).Enqueue(pid);}
static uint16_t PickNext(void* s){return Policy(s).PickNext();}
static void Remove(void* s,uint16_t pid){Policy(s).Remove(pid);}
static void Expired(void* s,uint16_t pid){Policy(s).TimesliceExpired(pid);}
static void Yield(void* s,uint16_t pid){Policy(s).VoluntaryYield(pid);}
static void Tick(void* s,uint64_t now){Policy(s).Tick(now);}
static uint64_t Slice(void* s,uint16_t pid){return Policy(s).Timeslice(pid);}
static uint32_t Ready(void* s){return Policy(s).ReadyCount();}
static void Debug(void* s,uint16_t pid,SchedDebugInfo* out){Policy(s).DebugDump(pid,out);}
static void Charge(void* s,uint16_t pid,uint64_t ns){Policy(s).AccountService(pid,ns);}
static void Runnable(void* s,uint16_t pid,bool active){Policy(s).SetRunnable(pid,active);}
static bool Preempt(void* s,uint16_t pid,uint64_t wall){return Policy(s).ShouldPreempt(pid,wall);}
static const SchedOps operations={"eevdf",sizeof(EevdfPolicy),SCHED_POLICY_ABI_VERSION,sizeof(SchedOps),
    Init,InitProcess,Enqueue,PickNext,Remove,Expired,Yield,Tick,Slice,Ready,Debug,Charge,Runnable,Preempt};
}
extern "C" const brook::SchedOps* GetSchedOps(){return &operations;}
