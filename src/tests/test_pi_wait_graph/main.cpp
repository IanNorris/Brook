#include <cassert>
#include <cstdio>
#include <random>
#include "sync/pi_wait_graph.h"

using namespace brook;
using Graph = PiWaitGraph<32>;
static PiIdentity Task(unsigned pid, unsigned incarnation = 1) {
    return {static_cast<uint16_t>(pid), incarnation};
}
static constexpr PiKey A{17, 64}, B{17, 128}, C{18, 64};

static void CheckEmpty(const Graph& graph) {
    for (unsigned pid = 0; pid < 32; ++pid) {
        assert(!graph.Get(Task(pid)));
        assert(graph.TaskAt(pid).references == 0);
        assert(graph.Effective(Task(pid)) == Graph::NoDonation);
    }
}
static void OwnershipAndRaces() {
    Graph graph{};
    assert(graph.Enqueue(A, Task(2), 1, Task(1), 3) == PiGraphResult::Ok);
    assert(graph.Effective(Task(1)) == 1 && graph.Donation(Task(1)) == 1);
    assert(graph.Enqueue(A, Task(3), 0, Task(1), 3) == PiGraphResult::Ok);
    assert(graph.Next(A, Task(1))->waiter == Task(3));
    assert(!graph.Next(C, Task(1))); // Same offset, different shared object.
    assert(!graph.Next(A, Task(1, 2))); // Recycled PID is not the owner.
    assert(graph.Handoff(A, Task(1), Task(2)) == PiGraphResult::Invalid);
    assert(graph.Get(Task(2))->owner == Task(1)); // Failed grant changes nothing.
    assert(graph.Handoff(A, Task(1), Task(3)) == PiGraphResult::Ok);
    assert(!graph.Cancel(Task(3), -4)); // Completed grant wins signal/timeout.
    int result = 9;
    assert(graph.Consume(Task(3), &result) && result == 0);
    assert(graph.Next(A, Task(3))->waiter == Task(2));
    assert(graph.Effective(Task(1)) == Graph::NoDonation); // Old donation removed.
    assert(graph.TaskAt(3).references == 1); // New owner pinned by remaining waiter.
    assert(graph.Cancel(Task(2), -110));
    assert(!graph.Cancel(Task(2), -4)); // First terminal outcome wins.
    assert(graph.Consume(Task(2), &result) && result == -110);
    assert(!graph.Consume(Task(2)));
    CheckEmpty(graph);
}

static void ChainsAndMergedDonors() {
    Graph graph{};
    assert(graph.Enqueue(A, Task(2), 3, Task(1), 3) == PiGraphResult::Ok);
    assert(graph.Enqueue(B, Task(3), 1, Task(2), 3) == PiGraphResult::Ok);
    assert(graph.Effective(Task(1)) == 1 && graph.Effective(Task(2)) == 1);
    assert(graph.Enqueue(C, Task(4), 0, Task(2), 3) == PiGraphResult::Ok);
    assert(graph.Effective(Task(1)) == 0 && graph.Effective(Task(2)) == 0);
    assert(graph.Enqueue({19, 0}, Task(1), 3, Task(4), 0) == PiGraphResult::Deadlock);
    assert(graph.Enqueue({19, 0}, Task(1), 3, Task(1), 3) == PiGraphResult::Deadlock);
    assert(!graph.Get(Task(1)));
    assert(graph.Cancel(Task(4), -4) && graph.Consume(Task(4)));
    assert(graph.Effective(Task(1)) == 1); // Other donor survives cancellation.
    assert(graph.SetBase(Task(3), 2));
    assert(graph.Effective(Task(1)) == 2 && graph.Effective(Task(2)) == 2);
    assert(!graph.SetBase(Task(3, 99), 0));
    assert(graph.Handoff(B, Task(2), Task(3)) == PiGraphResult::Ok);
    assert(graph.Consume(Task(3)));
    assert(graph.Effective(Task(1)) == 3 && graph.Donation(Task(1)) == Graph::NoDonation);
    assert(graph.Cancel(Task(2), -110) && graph.Consume(Task(2)));
    CheckEmpty(graph);
}

static void FifoAndDeathTransfer() {
    Graph graph{};
    for (unsigned pid : {7u, 2u, 5u})
        assert(graph.Enqueue(A, Task(pid), 2, Task(1), 3) == PiGraphResult::Ok);
    assert(graph.Next(A, Task(1))->waiter == Task(7)); // FIFO, not PID order.
    assert(graph.Handoff(A, Task(1), Task(7), -130) == PiGraphResult::Ok);
    int result;
    assert(graph.Consume(Task(7), &result) && result == -130);
    assert(graph.Next(A, Task(7))->waiter == Task(2));
    assert(graph.SetBase(Task(5), 0));
    assert(graph.Next(A, Task(7))->waiter == Task(5));
    assert(graph.Handoff(A, Task(7), Task(5)) == PiGraphResult::Ok);
    assert(graph.Consume(Task(5)));
    assert(graph.Handoff(A, Task(5), Task(2)) == PiGraphResult::Ok);
    assert(graph.Consume(Task(2)));
    CheckEmpty(graph);
}

static void IdentityAndCapacity() {
    Graph graph{};
    assert(graph.Enqueue(A, Task(32), 1, Task(1), 3) == PiGraphResult::Invalid);
    assert(graph.Enqueue(A, Task(2, 0), 1, Task(1), 3) == PiGraphResult::Invalid);
    assert(graph.Enqueue(A, Task(2), 4, Task(1), 3) == PiGraphResult::Invalid);
    for (unsigned pid = 1; pid < 32; ++pid)
        assert(graph.Enqueue(A, Task(pid), 2, Task(0), 3) == PiGraphResult::Ok);
    assert(graph.TaskAt(0).references == 31);
    assert(graph.Enqueue(B, Task(2), 2, Task(0), 3) == PiGraphResult::Busy);
    assert(graph.Enqueue(A, Task(0, 2), 2, Task(1), 3) == PiGraphResult::Stale);
    for (unsigned pid = 1; pid < 32; ++pid)
        assert(graph.Cancel(Task(pid), -4) && graph.Consume(Task(pid)));
    CheckEmpty(graph);
    assert(graph.Enqueue(A, Task(2, 2), 0, Task(1, 2), 3) == PiGraphResult::Ok);
    assert(!graph.Cancel(Task(2), -4));
    assert(!graph.Consume(Task(2)));
    assert(!graph.Get(Task(2)));
    assert(graph.Cancel(Task(2, 2), -4) && graph.Consume(Task(2, 2)));
    CheckEmpty(graph);
}

// Independent slow oracle: donor rank is the minimum base among all tasks
// that can reach an owner by following the blocking relation. Exercise long
// chains in both PID directions, merges, grants and cancellations in 20k steps.
static void RandomizedOracle() {
    Graph graph{};
    std::mt19937 random(0x50494658);
    for (unsigned iteration = 0; iteration < 20000; ++iteration) {
        const unsigned pid = 1 + random() % 31;
        const auto* node = graph.Get(Task(pid));
        if (!node) {
            const unsigned owner = random() % 32;
            (void)graph.Enqueue({100 + owner, 0}, Task(pid), random() % 4,
                                Task(owner), graph.TaskAt(owner).references ? graph.TaskAt(owner).base : random() % 4);
        } else if (node->state != PiWaitState::Waiting) {
            assert(graph.Consume(Task(pid)));
        } else if (random() % 2) {
            assert(graph.Cancel(Task(pid), -110));
        } else {
            const auto* selected = graph.Next(node->key, node->owner);
            assert(selected);
            const auto key = node->key;
            const auto owner = node->owner;
            const auto successor = selected->waiter;
            assert(graph.Handoff(key, owner, successor) == PiGraphResult::Ok);
        }
        unsigned refs[32]{};
        for (unsigned donor = 0; donor < 32; ++donor) {
            const auto* edge = graph.Get(Task(donor));
            if (!edge) continue;
            ++refs[donor];
            if (edge->state == PiWaitState::Waiting) ++refs[edge->owner.pid];
        }
        for (unsigned target = 0; target < 32; ++target) {
            assert(graph.TaskAt(target).references == refs[target]);
            unsigned expected = refs[target] ? graph.TaskAt(target).base : Graph::NoDonation;
            for (unsigned donor = 0; donor < 32; ++donor) {
                unsigned cursor = donor;
                for (unsigned depth = 0; depth < 32; ++depth) {
                    const auto* edge = graph.Get(Task(cursor));
                    if (!edge || edge->state != PiWaitState::Waiting) break;
                    cursor = edge->owner.pid;
                    if (cursor == target) {
                        if (graph.TaskAt(donor).base < expected) expected = graph.TaskAt(donor).base;
                        break;
                    }
                    assert(depth + 1 < 32);
                }
            }
            assert(graph.Effective(Task(target)) == expected);
        }
    }
    for (unsigned pid = 0; pid < 32; ++pid) {
        if (!graph.Get(Task(pid))) continue;
        (void)graph.Cancel(Task(pid), -4);
        assert(graph.Consume(Task(pid)));
    }
    CheckEmpty(graph);
}

int main() {
    OwnershipAndRaces();
    ChainsAndMergedDonors();
    FifoAndDeathTransfer();
    IdentityAndCapacity();
    RandomizedOracle();
    std::puts("test_pi_wait_graph: ALL PASS (including 20000-step oracle)");
}
