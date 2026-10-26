#pragma once
#include <stdint.h>

namespace brook {

// The PI dependency graph, independent of user mappings and scheduler locks.
// Caller serializes every operation and pins each task identity for the entire
// lifetime of any referencing node. A successful Handoff must be preceded by
// the caller's atomic user-word ownership transfer under that same lock.
// This component alone does not implement or enable the futex capability.
struct PiIdentity {
    uint16_t pid = 0;
    uint64_t incarnation = 0;
    bool operator==(const PiIdentity& other) const {
        return pid == other.pid && incarnation == other.incarnation;
    }
    bool operator!=(const PiIdentity& other) const { return !(*this == other); }
};
struct PiKey {
    uint64_t domain = 0, offset = 0;
    bool operator==(const PiKey& other) const {
        return domain == other.domain && offset == other.offset;
    }
};
enum class PiGraphResult : uint8_t { Ok, Invalid, Busy, Deadlock, Stale };
enum class PiWaitState : uint8_t { Empty, Waiting, Granted, Cancelled };

template<unsigned Capacity>
class PiWaitGraph {
    static_assert(Capacity > 0 && Capacity <= 65535);
public:
    static constexpr uint8_t NoDonation = 255;
    struct Node {
        PiWaitState state = PiWaitState::Empty;
        PiKey key{};
        PiIdentity waiter{}, owner{};
        uint64_t sequence = 0;
        int result = 0;
    };
    struct Task {
        PiIdentity identity{};
        unsigned references = 0;
        uint8_t base = 3, effective = 3;
    };

    // A task's base rank is Brook's internal 0..3 scheduler hint, not a Linux
    // realtime priority. The scheduler adapter must enforce donated ranks.
    PiGraphResult Enqueue(PiKey key, PiIdentity waiter, uint8_t waiterRank,
                          PiIdentity owner, uint8_t ownerRank) {
        if (!Valid(waiter) || !Valid(owner) || waiterRank > 3 || ownerRank > 3)
            return PiGraphResult::Invalid;
        if (waiter == owner) return PiGraphResult::Deadlock;
        if (!Compatible(waiter) || !Compatible(owner)) return PiGraphResult::Stale;
        if (nodes_[waiter.pid].state != PiWaitState::Empty) return PiGraphResult::Busy;
        for (const auto& node : nodes_)
            if (node.state == PiWaitState::Waiting && node.key == key && node.owner != owner)
                return PiGraphResult::Stale;
        // Each task can block on only one mutex; follow that unique outgoing
        // edge. Capacity is also a defensive bound against corrupt cycles.
        PiIdentity cursor = owner;
        for (unsigned depth = 0; depth < Capacity; ++depth) {
            if (cursor == waiter) return PiGraphResult::Deadlock;
            const Node& node = nodes_[cursor.pid];
            if (node.state != PiWaitState::Waiting || node.waiter != cursor) break;
            cursor = node.owner;
            if (depth + 1 == Capacity) return PiGraphResult::Deadlock;
        }
        // Refuse sequence exhaustion rather than silently reversing FIFO order.
        if (sequence_ == ~uint64_t{0}) return PiGraphResult::Busy;
        AddReference(waiter, waiterRank);
        AddReference(owner, ownerRank);
        nodes_[waiter.pid] = {PiWaitState::Waiting, key, waiter, owner, ++sequence_, 0};
        Recompute();
        return PiGraphResult::Ok;
    }

    const Node* Get(PiIdentity task) const {
        if (!Valid(task)) return nullptr;
        const auto& node = nodes_[task.pid];
        return node.state != PiWaitState::Empty && node.waiter == task ? &node : nullptr;
    }
    uint8_t Effective(PiIdentity task) const {
        return Valid(task) && tasks_[task.pid].references &&
            tasks_[task.pid].identity == task ? tasks_[task.pid].effective : NoDonation;
    }
    uint8_t Donation(PiIdentity task) const {
        const auto rank = Effective(task);
        return rank != NoDonation && rank < tasks_[task.pid].base ? rank : NoDonation;
    }
    bool SetBase(PiIdentity task, uint8_t rank) {
        if (!Valid(task) || rank > 3 || !tasks_[task.pid].references ||
            tasks_[task.pid].identity != task) return false;
        tasks_[task.pid].base = rank;
        Recompute();
        return true;
    }
    const Task& TaskAt(unsigned pid) const { return tasks_[pid]; }
    const Node* FirstOwnedBy(PiIdentity owner) const {
        for (const auto& node : nodes_)
            if (node.state == PiWaitState::Waiting && node.owner == owner) return &node;
        return nullptr;
    }
    bool HasKey(PiKey key) const {
        for (const auto& node : nodes_)
            if (node.state == PiWaitState::Waiting && node.key == key) return true;
        return false;
    }

    // Highest effective rank, then FIFO. Ownership and the key are both checked:
    // a recycled PID or a different shared object cannot select these waiters.
    const Node* Next(PiKey key, PiIdentity owner) const {
        const Node* best = nullptr;
        for (const auto& node : nodes_) {
            if (node.state != PiWaitState::Waiting || !(node.key == key) || node.owner != owner)
                continue;
            if (!best || Effective(node.waiter) < Effective(best->waiter) ||
                (Effective(node.waiter) == Effective(best->waiter) && node.sequence < best->sequence))
                best = &node;
        }
        return best;
    }

    // Grant one selected waiter and retarget the remaining dependencies to it.
    // Terminal nodes remain reserved until consumed: a signal/timeout racing a
    // completed grant must observe Granted and must not revoke the acquisition.
    PiGraphResult Handoff(PiKey key, PiIdentity owner, PiIdentity successor, int result = 0) {
        const auto* selected = Next(key, owner);
        if (!selected || selected->waiter != successor) return PiGraphResult::Invalid;
        const auto rank = tasks_[successor.pid].base;
        for (auto& node : nodes_) {
            if (node.state != PiWaitState::Waiting || !(node.key == key) || node.owner != owner)
                continue;
            DropReference(node.owner);
            if (node.waiter == successor) {
                node.owner = {};
                node.state = PiWaitState::Granted;
                node.result = result;
            } else {
                AddReference(successor, rank);
                node.owner = successor;
            }
        }
        Recompute();
        return PiGraphResult::Ok;
    }
    bool Cancel(PiIdentity waiter, int result) {
        const auto* found = Get(waiter);
        if (!found || found->state != PiWaitState::Waiting) return false;
        auto& node = nodes_[waiter.pid];
        DropReference(node.owner);
        node.owner = {};
        node.state = PiWaitState::Cancelled;
        node.result = result;
        Recompute();
        return true;
    }
    bool Consume(PiIdentity waiter, int* result = nullptr) {
        const auto* found = Get(waiter);
        if (!found || found->state == PiWaitState::Waiting) return false;
        if (result) *result = found->result;
        DropReference(waiter);
        nodes_[waiter.pid] = {};
        Recompute();
        return true;
    }

private:
    static bool Valid(PiIdentity task) { return task.pid < Capacity && task.incarnation; }
    bool Compatible(PiIdentity task) const {
        const auto& slot = tasks_[task.pid];
        return !slot.references || slot.identity == task;
    }
    void AddReference(PiIdentity task, uint8_t rank) {
        auto& slot = tasks_[task.pid];
        if (!slot.references) slot = {task, 0, rank, rank};
        ++slot.references;
    }
    void DropReference(PiIdentity task) { --tasks_[task.pid].references; }

    // Worklist relaxation of the highest-priority donor through unique outgoing
    // edges. Four ranks bound each task to at most four queue insertions. Queue
    // storage is a member, not an O(thread-count) interrupt-stack allocation.
    void Recompute() {
        unsigned head = 0, tail = 0, count = 0;
        for (unsigned pid = 0; pid < Capacity; ++pid) {
            queued_[pid] = tasks_[pid].references != 0;
            tasks_[pid].effective = tasks_[pid].base;
            if (queued_[pid]) { queue_[tail] = pid; tail = (tail + 1) % Capacity; ++count; }
        }
        while (count) {
            const auto pid = queue_[head]; head = (head + 1) % Capacity; --count;
            queued_[pid] = false;
            const auto& node = nodes_[pid];
            if (node.state != PiWaitState::Waiting) continue;
            auto& owner = tasks_[node.owner.pid];
            if (owner.effective <= tasks_[pid].effective) continue;
            owner.effective = tasks_[pid].effective;
            if (!queued_[node.owner.pid]) {
                queue_[tail] = node.owner.pid; tail = (tail + 1) % Capacity; ++count;
                queued_[node.owner.pid] = true;
            }
        }
    }
    Node nodes_[Capacity]{};
    Task tasks_[Capacity]{};
    uint16_t queue_[Capacity]{};
    bool queued_[Capacity]{};
    uint64_t sequence_ = 0;
};
} // namespace brook
