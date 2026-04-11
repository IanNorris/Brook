// sched_mlfq.cpp — Multi-Level Feedback Queue scheduler module
//
// 4 priority levels with per-priority FIFO queues.
// Timeslice-expired demotion, voluntary-yield boost, anti-starvation.
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
    bool     active;
    bool     queued;
    uint8_t  priority;
    uint8_t  basePriority;
    uint16_t nextPid;
    uint16_t prevPid;
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
};

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

// ---------------------------------------------------------------------------
// SchedOps implementation
// ---------------------------------------------------------------------------

static void MlfqInit(void* state) {
    auto* s = static_cast<MlfqState*>(state);
    for (uint32_t i = 0; i < SCHED_MAX_PIDS; i++)
        s->procs[i] = {false, false, MLFQ_PRIORITY_NORMAL, MLFQ_PRIORITY_NORMAL, NONE, NONE};
    for (uint8_t i = 0; i < MLFQ_NUM_PRIORITIES; i++)
        s->queues[i] = {NONE, NONE, 0};
    s->lastBoostMs = 0;
    s->totalReady  = 0;
}

static void MlfqInitProcess(void* state, uint16_t pid, uint8_t priority) {
    auto* s = static_cast<MlfqState*>(state);
    if (pid >= SCHED_MAX_PIDS) return;
    if (priority >= MLFQ_NUM_PRIORITIES) priority = MLFQ_PRIORITY_NORMAL;
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
    if (pid >= SCHED_MAX_PIDS) return;
    auto& p = s->procs[pid];
    if (p.priority < MLFQ_PRIORITY_LOW)
        p.priority++;
}

static void MlfqVoluntaryYield(void* state, uint16_t pid) {
    auto* s = static_cast<MlfqState*>(state);
    if (pid >= SCHED_MAX_PIDS) return;
    auto& p = s->procs[pid];
    if (p.priority > MLFQ_PRIORITY_HIGH)
        p.priority--;
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

static const SchedOps g_mlfqOps = {
    "mlfq",
    sizeof(MlfqState),
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
};

} // anonymous namespace

extern "C" const brook::SchedOps* GetSchedOps() {
    return &g_mlfqOps;
}
