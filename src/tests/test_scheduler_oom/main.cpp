#include <cassert>
#include <mutex>
#include <cstdio>
#include <cstring>
#include "../../kernel/src/process.h"
#include "../../kernel/src/scheduler.h"
#include "../../kernel/src/memory/address_space.h"
namespace brook {
std::mutex g_allProcLock;
Process* g_allProcesses[MAX_PROCESSES]{};uint32_t g_processCount=0;
uint64_t SchedLockAcquire(std::mutex& lock){lock.lock();return 0;}
void SchedLockRelease(std::mutex& lock,uint64_t){lock.unlock();}
void AddressSpaceRef(AddressSpaceState* space){++space->references;}
void AddressSpaceUnref(AddressSpaceState* space){assert(space->references);--space->references;}
unsigned fatalRequests=0;
void PosixTimersCleanup(Process*,bool) {}
void SchedulerMarkTerminated(Process* process,int status) {
    assert(status==9);process->state=ProcessState::Terminated;++fatalRequests;
}
void ReleaseSchedulerReference(Process* process,bool){ProcessUnref(process);}
static bool RequestFatalSignalImpl(Process*,int,AddressSpaceState*,uint64_t);
void ProcessSignalInfoToUser(Process*,SignalInfo*) {}
uint64_t PosixTimerPending(Process*){return 0;}
bool PosixTimerDequeue(Process*,uint64_t,SignalInfo*){return false;}
#include "../../kernel/src/signal_pending.inc"
std::mutex g_readyLock;
uint32_t ThisCpu(){return 0;}
uint64_t RuntimeNow(uint32_t){return 0;}
void RuntimeChargeLocked(uint32_t,uint64_t){}
void TransferChildSubreaperExecLocked(Process*){}
void ReleaseThreadAddressSpaceLocked(Process*){}
struct {void Rebind(uint16_t,uint64_t,uint16_t,uint64_t){}} g_runtime;
#include "../../kernel/src/scheduler_exec_identity.inc"
#include "../../kernel/src/scheduler_oom.inc"
#include "../../kernel/src/scheduler_fatal.inc"
}
using namespace brook;
int main() {
    Process anchor{},peer{};AddressSpaceState space{},replacement{};
    anchor.pid=anchor.tgid=10;anchor.incarnation=100;anchor.refCount=1;
    anchor.addressSpace=&space;anchor.state=ProcessState::Ready;space.references=1;space.identity=5;
    g_allProcesses[0]=&anchor;g_processCount=1;
    Process* referenced=nullptr;AddressSpaceState* pinned=nullptr;
    assert(SchedulerNextOomCandidate(0,&referenced,&pinned));
    assert(referenced==&anchor && pinned==&space && anchor.refCount==2 && space.references==2);
    ProcessUnref(referenced);AddressSpaceUnref(pinned);
    anchor.lifecycleRole=ProcessLifecycleRole::Service;
    assert(!SchedulerNextOomCandidate(0,&referenced,&pinned));
    assert(!SchedulerOomCommitVictim(&anchor,&space,100));
    anchor.lifecycleRole=ProcessLifecycleRole::Application;
    peer.pid=11;peer.tgid=11;peer.incarnation=101;peer.addressSpace=&space;peer.state=ProcessState::Ready;
    g_allProcesses[1]=&peer;g_processCount=2;
    assert(!SchedulerNextOomCandidate(0,&referenced,&pinned)); // foreign shared AS
    assert(!SchedulerOomCommitVictim(&anchor,&space,100));
    peer.tgid=10;peer.isThread=true;peer.leaderIncarnation=100;peer.refCount=1;
    peer.lifecycleRole=ProcessLifecycleRole::Service;
    assert(!SchedulerOomCommitVictim(&anchor,&space,100));
    peer.lifecycleRole=ProcessLifecycleRole::Application;
    anchor.incarnation=102;assert(!SchedulerOomCommitVictim(&anchor,&space,100)); // recycled identity
    anchor.incarnation=100;anchor.addressSpace=&replacement;
    assert(!SchedulerOomCommitVictim(&anchor,&space,100)); // exec won before commit
    anchor.addressSpace=&space;
    assert(!space.oomTerminating && !anchor.tgidExiting && !fatalRequests);
    assert(SchedulerOomCommitVictim(&anchor,&space,100));
    assert(space.oomTerminating && anchor.tgidExiting && peer.tgidExiting && fatalRequests==2);
    assert(anchor.refCount==1 && peer.refCount==1); // no refs retained while cleanup waits
    assert(!SchedulerOomCommitVictim(&anchor,&space,100));
    assert(SchedulerOomGroupPresent(10,100));
    assert(anchor.oomEpisode==space.identity && peer.oomEpisode==space.identity);
    // Exact interleaving: a selected sibling was paused before protected exec
    // identity commit. The old AS/group can disappear while this task remains.
    g_allProcesses[0]=&peer;g_processCount=1;peer.addressSpace=&replacement;
    SchedulerCommitExecThreadIdentity(&peer);
    assert(peer.tgid==peer.pid && !peer.isThread && peer.oomEpisode==space.identity);
    assert(!SchedulerOomGroupPresent(10,100));
    assert(SchedulerOomEpisodePresent(space.identity));
    // Final task retirement, not tgid reassignment, ends this episode.
    g_allProcesses[0]=&anchor;anchor.oomEpisode=0;
    assert(!SchedulerOomEpisodePresent(space.identity));
    g_processCount=1;anchor.incarnation=200;anchor.state=ProcessState::Ready;
    assert(!SchedulerOomGroupPresent(10,100)); // reused PID cannot delay old completion
    assert(SchedulerOomGroupPresent(10,200));
    g_processCount=0;assert(!SchedulerOomCommitVictim(&anchor,&space,200));
    std::puts("OOM scheduler: service/shared-AS exclusion, stale identity/exec rejection, atomic latches and async request PASS");
}
