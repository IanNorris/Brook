#include "sched_policy.h"

namespace brook {

// ---------------------------------------------------------------------------
// Queue helpers
// ---------------------------------------------------------------------------

static void QueueInit(SchedQueue* q)
{
    q->head  = nullptr;
    q->tail  = nullptr;
    q->count = 0;
}

static void QueuePushBack(SchedQueue* q, SchedProcess* proc)
{
    proc->next = nullptr;
    proc->prev = q->tail;
    if (q->tail) q->tail->next = proc;
    else         q->head = proc;
    q->tail = proc;
    q->count++;
}

static SchedProcess* QueuePopFront(SchedQueue* q)
{
    SchedProcess* p = q->head;
    if (!p) return nullptr;
    q->head = p->next;
    if (q->head) q->head->prev = nullptr;
    else         q->tail = nullptr;
    p->next = nullptr;
    p->prev = nullptr;
    q->count--;
    return p;
}

static void QueueRemove(SchedQueue* q, SchedProcess* proc)
{
    if (proc->prev) proc->prev->next = proc->next;
    else            q->head = proc->next;
    if (proc->next) proc->next->prev = proc->prev;
    else            q->tail = proc->prev;
    proc->next = nullptr;
    proc->prev = nullptr;
    q->count--;
}

// ---------------------------------------------------------------------------
// Policy API
// ---------------------------------------------------------------------------

void SchedPolicyInit(SchedPolicyState* state)
{
    for (uint8_t i = 0; i < SCHED_NUM_PRIORITIES; ++i)
        QueueInit(&state->queues[i]);
    state->lastBoostMs = 0;
    state->totalReady  = 0;
}

void SchedPolicyInitProcess(SchedProcess* proc, uint8_t basePriority)
{
    proc->sched.priority        = basePriority;
    proc->sched.basePriority    = basePriority;
    proc->sched.cpuTimeMs       = 0;
    proc->sched.lastScheduledMs = 0;
    proc->sched.yieldCount      = 0;
    proc->sched.preemptCount    = 0;
    proc->next = nullptr;
    proc->prev = nullptr;
}

void SchedPolicyEnqueue(SchedPolicyState* state, SchedProcess* proc)
{
    QueuePushBack(&state->queues[proc->sched.priority], proc);
    state->totalReady++;
}

void SchedPolicyRemove(SchedPolicyState* state, SchedProcess* proc)
{
    QueueRemove(&state->queues[proc->sched.priority], proc);
    state->totalReady--;
}

SchedProcess* SchedPolicyPickNext(SchedPolicyState* state)
{
    // Scan from highest to lowest priority.
    for (uint8_t i = 0; i < SCHED_NUM_PRIORITIES; ++i)
    {
        SchedProcess* p = QueuePopFront(&state->queues[i]);
        if (p) {
            state->totalReady--;
            return p;
        }
    }
    return nullptr;
}

void SchedPolicyTimesliceExpired(SchedProcess* proc)
{
    proc->sched.preemptCount++;
    // Demote: move down one priority (clamped to lowest).
    if (proc->sched.priority < SCHED_PRIORITY_LOW)
        proc->sched.priority++;
}

void SchedPolicyVoluntaryYield(SchedProcess* proc)
{
    proc->sched.yieldCount++;
    // Boost: move up one priority (clamped to highest non-realtime).
    if (proc->sched.priority > SCHED_PRIORITY_HIGH)
        proc->sched.priority--;
}

void SchedPolicyBoostAll(SchedPolicyState* state, uint64_t nowMs)
{
    if (nowMs - state->lastBoostMs < SCHED_BOOST_INTERVAL_MS)
        return;

    state->lastBoostMs = nowMs;

    // Move all processes from lower queues up to SCHED_PRIORITY_HIGH.
    for (uint8_t i = SCHED_PRIORITY_NORMAL; i < SCHED_NUM_PRIORITIES; ++i)
    {
        SchedProcess* p;
        while ((p = QueuePopFront(&state->queues[i])) != nullptr)
        {
            p->sched.priority = SCHED_PRIORITY_HIGH;
            QueuePushBack(&state->queues[SCHED_PRIORITY_HIGH], p);
        }
    }
}

uint64_t SchedPolicyTimeslice(const SchedProcess* proc)
{
    return SCHED_TIMESLICE[proc->sched.priority];
}

uint32_t SchedPolicyReadyCount(const SchedPolicyState* state)
{
    return state->totalReady;
}

} // namespace brook
