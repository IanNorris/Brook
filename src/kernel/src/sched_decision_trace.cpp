#include "sched_decision_trace.h"
namespace brook {
namespace {
SchedDecisionModel model;
uint32_t guardWord=0;
uint64_t publishedEpoch=0,targetPid=0,targetIncarnation=0,contention=0;
struct Guard {
    uint64_t flags=0;bool held=false;
    Guard(){__asm__ volatile("pushfq; pop %0; cli":"=r"(flags)::"memory");uint32_t expected=0;held=__atomic_compare_exchange_n(&guardWord,&expected,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED);}
    ~Guard(){if(held)__atomic_store_n(&guardWord,0,__ATOMIC_RELEASE);if(flags&0x200)__asm__ volatile("sti":::"memory");}
};
void Copy(SchedDecisionSnapshot* output){if(output){*output=model.Snapshot();output->contended=__atomic_load_n(&contention,__ATOMIC_RELAXED);}}
void Publish(){const auto s=model.Snapshot();__atomic_store_n(&targetPid,s.pid,__ATOMIC_RELAXED);__atomic_store_n(&targetIncarnation,s.incarnation,__ATOMIC_RELAXED);__atomic_store_n(&publishedEpoch,s.epoch,__ATOMIC_RELEASE);}
void Drop(){auto old=__atomic_load_n(&contention,__ATOMIC_RELAXED);while(old!=~uint64_t(0) && !__atomic_compare_exchange_n(&contention,&old,old+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}}
}
bool SchedDecisionWatch(uint64_t pid,uint64_t incarnation,SchedDecisionSnapshot* output){
    if(!pid || pid>65535 || !incarnation)return false;Guard guard;if(!guard.held)return false;
    __atomic_store_n(&publishedEpoch,0,__ATOMIC_RELEASE);model.Watch(pid,incarnation);Publish();Copy(output);return true;
}
bool SchedDecisionOff(SchedDecisionSnapshot* output){Guard guard;if(!guard.held)return false;__atomic_store_n(&publishedEpoch,0,__ATOMIC_RELEASE);model.Off();Publish();Copy(output);return true;}
bool SchedDecisionReset(SchedDecisionSnapshot* output){Guard guard;if(!guard.held)return false;__atomic_store_n(&publishedEpoch,0,__ATOMIC_RELEASE);model.Reset();Publish();Copy(output);return true;}
bool SchedDecisionRead(SchedDecisionSnapshot* output){if(!output)return false;Guard guard;if(!guard.held)return false;Copy(output);return true;}
uint64_t SchedDecisionToken(uint64_t pid,uint64_t incarnation){auto epoch=__atomic_load_n(&publishedEpoch,__ATOMIC_ACQUIRE);return (epoch&1) && pid==__atomic_load_n(&targetPid,__ATOMIC_RELAXED) && incarnation==__atomic_load_n(&targetIncarnation,__ATOMIC_RELAXED)?epoch:0;}
void SchedDecisionRecord(const SchedDecisionEvent& event){Guard guard;if(!guard.held){Drop();return;}model.Observe(event);}
}
