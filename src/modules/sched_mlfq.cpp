// sched_mlfq.cpp — Multi-Level Feedback Queue scheduler module
//
// 4 priority levels with per-priority FIFO queues.
// Timeslice-expired demotion, voluntary-yield boost, anti-starvation.
// Higher-priority wakeups preempt without renewing consumed service budgets.
// Compiles as .mod (kernel) or .so (host tests).

#include "sched_ops.h"

namespace {

using namespace brook;

static constexpr uint8_t  MLFQ_NUM_PRIORITIES    = 4;
static constexpr uint8_t  MLFQ_PRIORITY_HIGH     = 1;
static constexpr uint8_t  MLFQ_PRIORITY_NORMAL   = 2;
static constexpr uint8_t  MLFQ_PRIORITY_LOW      = 3;

static constexpr uint64_t MLFQ_TIMESLICE[MLFQ_NUM_PRIORITIES] = {
    5, 10, 20, 40,
};

static constexpr uint64_t MLFQ_BOOST_INTERVAL_MS = 1000;

static constexpr uint16_t NONE = SCHED_PID_NONE;

struct MlfqProcInfo {
    uint64_t usedNs = 0;
    bool     active = false;
    bool     queued = false;
    bool     serviceSeen = false;
    bool     earlyPreemption = false;
    uint8_t  priority = MLFQ_PRIORITY_NORMAL;
    uint8_t  basePriority = MLFQ_PRIORITY_NORMAL;
    uint16_t nextPid = NONE;
    uint16_t prevPid = NONE;
};

struct MlfqQueue {
    uint16_t head;
    uint16_t tail;
    uint32_t count;
};

struct MlfqState {
    MlfqProcInfo procs[SCHED_MAX_PIDS];
    MlfqQueue    queues[MLFQ_NUM_PRIORITIES];
    uint64_t     lastBoostMs;
    uint32_t     totalReady;
    uint64_t     serviceNs=0,inactiveServiceNs=0,saturations=0;
};
static_assert(sizeof(MlfqState) <= SCHED_POLICY_STATE_CAPACITY,
              "MLFQ state exceeds kernel policy storage");

// ---------------------------------------------------------------------------
// Queue helpers (operate on pid-indexed linked lists)
// ---------------------------------------------------------------------------

static void QueuePush(MlfqState* s, MlfqQueue* q, uint16_t pid) {
    auto& p = s->procs[pid];
    if (p.queued) return;
    p.queued  = true;
    p.nextPid = NONE;
    p.prevPid = q->tail;
    if (q->tail != NONE)
        s->procs[q->tail].nextPid = pid;
    else
        q->head = pid;
    q->tail = pid;
    q->count++;
    s->totalReady++;
}

static uint16_t QueuePop(MlfqState* s, MlfqQueue* q) {
    if (q->head == NONE) return NONE;
    uint16_t pid = q->head;
    auto& p = s->procs[pid];
    q->head = p.nextPid;
    if (q->head != NONE)
        s->procs[q->head].prevPid = NONE;
    else
        q->tail = NONE;
    p.queued  = false;
    p.nextPid = NONE;
    p.prevPid = NONE;
    q->count--;
    s->totalReady--;
    return pid;
}

static void QueueRemove(MlfqState* s, MlfqQueue* q, uint16_t pid) {
    auto& p = s->procs[pid];
    if (!p.queued) return;
    if (p.prevPid != NONE) s->procs[p.prevPid].nextPid = p.nextPid;
    else                   q->head = p.nextPid;
    if (p.nextPid != NONE) s->procs[p.nextPid].prevPid = p.prevPid;
    else                   q->tail = p.prevPid;
    p.queued  = false;
    p.nextPid = NONE;
    p.prevPid = NONE;
    q->count--;
    s->totalReady--;
}

static void ChangePriority(MlfqState* s,uint16_t pid,uint8_t priority) {
    auto& p=s->procs[pid];if(p.priority==priority)return;
    const bool queued=p.queued;
    if(queued)QueueRemove(s,&s->queues[p.priority],pid);
    p.priority=priority;
    if(queued)QueuePush(s,&s->queues[p.priority],pid);
}

static void AddService(MlfqState* s,uint64_t& counter,uint64_t amount) {
    if(amount>~uint64_t(0)-counter){
        counter=~uint64_t(0);if(s->saturations!=~uint64_t(0))++s->saturations;
    }else counter+=amount;
}

// ---------------------------------------------------------------------------
// SchedOps implementation
// ---------------------------------------------------------------------------

static void MlfqInit(void* state) {
    auto* s = static_cast<MlfqState*>(state);
    for (uint32_t i = 0; i < SCHED_MAX_PIDS; i++)
        s->procs[i] = {};
    for (uint8_t i = 0; i < MLFQ_NUM_PRIORITIES; i++)
        s->queues[i] = {NONE, NONE, 0};
    s->lastBoostMs = 0;
    s->totalReady  = 0;
    s->serviceNs=s->inactiveServiceNs=s->saturations=0;
}

static void MlfqInitProcess(void* state, uint16_t pid, uint8_t priority) {
    auto* s = static_cast<MlfqState*>(state);
    if (pid >= SCHED_MAX_PIDS) return;
    if (priority >= MLFQ_NUM_PRIORITIES) priority = MLFQ_PRIORITY_NORMAL;
    // A reused PID may still be linked under its old priority. Unlink before
    // overwriting either the priority or neighbour pointers, as RR does.
    if (s->procs[pid].queued)
        QueueRemove(s, &s->queues[s->procs[pid].priority], pid);
    s->procs[pid] = {};
    s->procs[pid].active       = true;
    s->procs[pid].queued       = false;
    s->procs[pid].priority     = priority;
    s->procs[pid].basePriority = priority;
    s->procs[pid].nextPid      = NONE;
    s->procs[pid].prevPid      = NONE;
}

static void MlfqEnqueue(void* state, uint16_t pid) {
    auto* s = static_cast<MlfqState*>(state);
    if (pid >= SCHED_MAX_PIDS || !s->procs[pid].active) return;
    QueuePush(s, &s->queues[s->procs[pid].priority], pid);
}

static uint16_t MlfqPickNext(void* state) {
    auto* s = static_cast<MlfqState*>(state);
    for (uint8_t i = 0; i < MLFQ_NUM_PRIORITIES; i++) {
        uint16_t pid = QueuePop(s, &s->queues[i]);
        if (pid != NONE) return pid;
    }
    return NONE;
}

static void MlfqRemove(void* state, uint16_t pid) {
    auto* s = static_cast<MlfqState*>(state);
    if (pid >= SCHED_MAX_PIDS || !s->procs[pid].active) return;
    QueueRemove(s, &s->queues[s->procs[pid].priority], pid);
}

static void MlfqTimesliceExpired(void* state, uint16_t pid) {
    auto* s = static_cast<MlfqState*>(state);
    if (pid >= SCHED_MAX_PIDS || !s->procs[pid].active) return;
    auto& p = s->procs[pid];
    // The mechanism calls this callback for every positive ShouldPreempt,
    // including higher-priority wakeups. Such an interruption must preserve
    // priority and all consumed budget, even when no eligible peer is picked.
    if(p.earlyPreemption){p.earlyPreemption=false;return;}
    const uint64_t quantum=MLFQ_TIMESLICE[p.priority]*1000000;
    p.usedNs=p.usedNs>=quantum?p.usedNs-quantum:0;
    if (p.priority < MLFQ_PRIORITY_LOW)ChangePriority(s,pid,p.priority+1);
}

static void MlfqVoluntaryYield(void* state, uint16_t pid) {
    auto* s = static_cast<MlfqState*>(state);
    if (pid >= SCHED_MAX_PIDS || !s->procs[pid].active) return;
    auto& p = s->procs[pid];
    // Retain the existing voluntary-yield/I/O reward. Preventing deliberate
    // yield/block gaming is a separate policy change, not hidden in this one.
    p.usedNs=0;p.earlyPreemption=false;
    if (p.priority > MLFQ_PRIORITY_HIGH)ChangePriority(s,pid,p.priority-1);
}

static void MlfqTick(void* state, uint64_t nowMs) {
    auto* s = static_cast<MlfqState*>(state);
    if (nowMs - s->lastBoostMs < MLFQ_BOOST_INTERVAL_MS)
        return;
    s->lastBoostMs = nowMs;
    // Boost: move all processes from lower queues to HIGH.
    for (uint8_t i = MLFQ_PRIORITY_NORMAL; i < MLFQ_NUM_PRIORITIES; i++) {
        uint16_t pid;
        while ((pid = QueuePop(s, &s->queues[i])) != NONE) {
            s->procs[pid].priority = MLFQ_PRIORITY_HIGH;
            s->procs[pid].usedNs=0;s->procs[pid].earlyPreemption=false;
            QueuePush(s, &s->queues[MLFQ_PRIORITY_HIGH], pid);
        }
    }
}

static uint64_t MlfqTimeslice(void* state, uint16_t pid) {
    auto* s = static_cast<MlfqState*>(state);
    if (pid >= SCHED_MAX_PIDS) return MLFQ_TIMESLICE[MLFQ_PRIORITY_NORMAL];
    return MLFQ_TIMESLICE[s->procs[pid].priority];
}

static uint32_t MlfqReadyCount(void* state) {
    return static_cast<MlfqState*>(state)->totalReady;
}

static void MlfqAccountService(void* state, uint16_t pid, uint64_t ns) {
    if(!ns)return;
    auto* s=static_cast<MlfqState*>(state);AddService(s,s->serviceNs,ns);
    if(pid>=SCHED_MAX_PIDS || !s->procs[pid].active){AddService(s,s->inactiveServiceNs,ns);return;}
    auto& p=s->procs[pid];p.serviceSeen=true;AddService(s,p.usedNs,ns);
}
static void MlfqSetRunnable(void*, uint16_t, bool) {}
static bool MlfqShouldPreempt(void* state, uint16_t pid, uint64_t elapsedMs) {
    auto* s=static_cast<MlfqState*>(state);
    if(pid>=SCHED_MAX_PIDS || !s->procs[pid].active)return false;
    auto& p=s->procs[pid];p.earlyPreemption=false;
    // Legacy callers without service observations retain wall-slice semantics.
    // Native accounting includes partial dispatches and subtracts reported
    // steal. No early preemption, migration or enqueue resets this budget.
    const bool expired=p.serviceSeen?p.usedNs>=MLFQ_TIMESLICE[p.priority]*1000000:
        elapsedMs>=MLFQ_TIMESLICE[p.priority];
    if(expired)return true;
    for(unsigned level=0;level<p.priority;++level)if(s->queues[level].count){
        p.earlyPreemption=true;return true;
    }
    return false;
}

static void MlfqDebugDump(void* state,uint16_t pid,SchedDebugInfo* out) {
    if(!out)return;
    auto* s=static_cast<MlfqState*>(state);*out={};
    out->head=out->tail=out->nextPid=out->prevPid=NONE;out->readyCount=s->totalReady;
    for(unsigned level=0;level<MLFQ_NUM_PRIORITIES;++level){
        if(out->head==NONE && s->queues[level].head!=NONE)out->head=s->queues[level].head;
        if(s->queues[level].tail!=NONE)out->tail=s->queues[level].tail;
        unsigned steps=0;
        for(auto p=s->queues[level].head;p!=NONE && p<SCHED_MAX_PIDS && steps++<SCHED_MAX_PIDS;p=s->procs[p].nextPid)++out->listLen;
    }
    if(pid<SCHED_MAX_PIDS){const auto& p=s->procs[pid];out->active=p.active;out->queued=p.queued;out->nextPid=p.nextPid;out->prevPid=p.prevPid;}
    out->hasServiceInfo=true;out->serviceNs=s->serviceNs;out->inactiveServiceNs=s->inactiveServiceNs;out->saturations=s->saturations;
}

static const SchedOps g_mlfqOps = {
    "mlfq",
    sizeof(MlfqState),
    SCHED_POLICY_ABI_VERSION, sizeof(SchedOps),
    MlfqInit,
    MlfqInitProcess,
    MlfqEnqueue,
    MlfqPickNext,
    MlfqRemove,
    MlfqTimesliceExpired,
    MlfqVoluntaryYield,
    MlfqTick,
    MlfqTimeslice,
    MlfqReadyCount,
    MlfqDebugDump,
    MlfqAccountService, MlfqSetRunnable, MlfqShouldPreempt,
};

} // anonymous namespace

extern "C" const brook::SchedOps* GetSchedOps() {
    return &g_mlfqOps;
}
