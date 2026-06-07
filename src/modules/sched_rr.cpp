// sched_rr.cpp — Round-Robin scheduler module
//
// Simple FIFO round-robin: all processes share a single queue,
// fixed timeslice, no priority differentiation.
// Compiles as .mod (kernel) or .so (host tests).

#include "sched_ops.h"

namespace {

using namespace brook;

static constexpr uint64_t RR_TIMESLICE_MS = 10;

struct RrProcInfo {
    bool     active;      // Has been InitProcess'd
    bool     queued;      // Currently in the ready queue
    uint16_t nextPid;     // Next in FIFO (UINT16_MAX = end)
    uint16_t prevPid;     // Prev in FIFO (UINT16_MAX = end)
};

struct RrState {
    RrProcInfo procs[SCHED_MAX_PIDS];
    uint16_t   head;       // First pid in queue (UINT16_MAX = empty)
    uint16_t   tail;       // Last pid in queue
    uint32_t   readyCount;
};

static constexpr uint16_t NONE = SCHED_PID_NONE;

// ---------------------------------------------------------------------------
// Queue helpers
// ---------------------------------------------------------------------------

static void Enqueue(RrState* s, uint16_t pid) {
    if (s->procs[pid].queued) return;
    s->procs[pid].queued  = true;
    s->procs[pid].nextPid = NONE;
    s->procs[pid].prevPid = s->tail;
    if (s->tail != NONE)
        s->procs[s->tail].nextPid = pid;
    else
        s->head = pid;
    s->tail = pid;
    s->readyCount++;
}

static void Remove(RrState* s, uint16_t pid) {
    if (!s->procs[pid].queued) return;
    uint16_t prev = s->procs[pid].prevPid;
    uint16_t next = s->procs[pid].nextPid;
    if (prev != NONE) s->procs[prev].nextPid = next;
    else              s->head = next;
    if (next != NONE) s->procs[next].prevPid = prev;
    else              s->tail = prev;
    s->procs[pid].queued  = false;
    s->procs[pid].nextPid = NONE;
    s->procs[pid].prevPid = NONE;
    s->readyCount--;
}

// ---------------------------------------------------------------------------
// SchedOps implementation
// ---------------------------------------------------------------------------

static void RrInit(void* state) {
    auto* s = static_cast<RrState*>(state);
    for (uint32_t i = 0; i < SCHED_MAX_PIDS; i++) {
        s->procs[i] = {false, false, NONE, NONE};
    }
    s->head = NONE;
    s->tail = NONE;
    s->readyCount = 0;
}

static void RrInitProcess(void* state, uint16_t pid, uint8_t /*priority*/) {
    auto* s = static_cast<RrState*>(state);
    if (pid >= SCHED_MAX_PIDS) return;
    // BRO-176 HANG fix: a pid being (re-)initialized must start from a clean,
    // dequeued state.  pids are reused, and if this pid is still linked in the
    // ready queue (queued==true) from a prior generation, simply clearing the
    // `queued` flag below would DESYNC the three-way invariant between the flag,
    // the intrusive list, and readyCount: the pid stays linked (head/next/prev
    // still point at it) and readyCount still counts it, but the flag says
    // "not queued".  Enqueue/Remove both early-return based on that flag, so the
    // next RrEnqueue(pid) silently drops (flag already inconsistent) — leaving a
    // process marked state==Ready that is NEVER in the queue, or a readyCount
    // that reads 0 while the list is non-empty.  PickNext then never returns it
    // and every CPU goes idle forever (the schedstress reap-stall HANG).  Unlink
    // it properly first (decrements readyCount, fixes neighbour links) so the
    // invariant holds across pid reuse.
    if (s->procs[pid].queued)
        Remove(s, pid);
    s->procs[pid].active  = true;
    s->procs[pid].queued  = false;
    s->procs[pid].nextPid = NONE;
    s->procs[pid].prevPid = NONE;
}

static void RrEnqueue(void* state, uint16_t pid) {
    auto* s = static_cast<RrState*>(state);
    if (pid >= SCHED_MAX_PIDS || !s->procs[pid].active) return;
    Enqueue(s, pid);
}

static uint16_t RrPickNext(void* state) {
    auto* s = static_cast<RrState*>(state);
    if (s->head == NONE) return NONE;
    uint16_t pid = s->head;
    Remove(s, pid);
    return pid;
}

static void RrRemove(void* state, uint16_t pid) {
    auto* s = static_cast<RrState*>(state);
    if (pid >= SCHED_MAX_PIDS) return;
    Remove(s, pid);
}

static void RrTimesliceExpired(void* /*state*/, uint16_t /*pid*/) {
    // RR: no priority change
}

static void RrVoluntaryYield(void* /*state*/, uint16_t /*pid*/) {
    // RR: no priority change
}

static void RrTick(void* /*state*/, uint64_t /*nowMs*/) {
    // RR: nothing to do
}

static uint64_t RrTimeslice(void* /*state*/, uint16_t /*pid*/) {
    return RR_TIMESLICE_MS;
}

static uint32_t RrReadyCount(void* state) {
    return static_cast<RrState*>(state)->readyCount;
}

static void RrDebugDump(void* state, uint16_t pid, SchedDebugInfo* out) {
    auto* s = static_cast<RrState*>(state);
    if (!out) return;
    out->head       = s->head;
    out->tail       = s->tail;
    out->readyCount = s->readyCount;
    if (pid < SCHED_MAX_PIDS) {
        out->queued  = s->procs[pid].queued ? 1 : 0;
        out->active  = s->procs[pid].active ? 1 : 0;
        out->nextPid = s->procs[pid].nextPid;
        out->prevPid = s->procs[pid].prevPid;
    } else {
        out->queued = out->active = 0;
        out->nextPid = out->prevPid = NONE;
    }
    // Walk the actual list from head to measure its true length — if this
    // disagrees with readyCount, the RrState struct was corrupted by a wild
    // write (not a logic desync). Bounded by SCHED_MAX_PIDS to avoid looping
    // forever on a corrupted cyclic list.
    uint32_t len = 0;
    uint16_t cur = s->head;
    while (cur != NONE && len <= SCHED_MAX_PIDS) {
        len++;
        cur = s->procs[cur].nextPid;
    }
    out->listLen = len;
}

static const SchedOps g_rrOps = {
    "rr",
    sizeof(RrState),
    RrInit,
    RrInitProcess,
    RrEnqueue,
    RrPickNext,
    RrRemove,
    RrTimesliceExpired,
    RrVoluntaryYield,
    RrTick,
    RrTimeslice,
    RrReadyCount,
    RrDebugDump,
};

} // anonymous namespace

extern "C" const brook::SchedOps* GetSchedOps() {
    return &g_rrOps;
}
