#pragma once

#include <stdint.h>

namespace brook {

inline constexpr uint32_t PidNamespaceIdLimit = 1u << 22;

// The owning Process supplies the storage. A number can have reservations
// for its task, thread group, process group and session at the same time.
struct PidIdLease {
    PidIdLease* next = nullptr;
    PidIdLease** link = nullptr;
    uint32_t number = 0;
};

// All operations run under the PID namespace lock. The small hash table
// stores only bucket heads. Memory does not scale with the PID range.
class PidIdAllocator {
public:
    uint32_t Reserve(PidIdLease& lease, bool init)
    {
        if (init) {
            if (Contains(1)) return 0;
            Attach(lease, 1);
            return 1;
        }
        const uint32_t start = cursor;
        do {
            const uint32_t candidate = cursor;
            cursor = candidate + 1 < PidNamespaceIdLimit ? candidate + 1 : 2;
            if (!Contains(candidate)) {
                Attach(lease, candidate);
                return candidate;
            }
        } while (cursor != start);
        return 0;
    }

    void Retain(PidIdLease& lease, uint32_t number)
    {
        if (!number || !Contains(number)) __builtin_trap();
        Attach(lease, number);
    }

    static void Release(PidIdLease& lease)
    {
        if (!lease.link) return;
        *lease.link = lease.next;
        if (lease.next) lease.next->link = lease.link;
        lease = {};
    }

private:
    static constexpr unsigned BucketCount = 32;
    PidIdLease* buckets[BucketCount]{};
    uint32_t cursor = 2;

    bool Contains(uint32_t number) const
    {
        for (auto* node = buckets[number % BucketCount]; node; node = node->next)
            if (node->number == number) return true;
        return false;
    }

    void Attach(PidIdLease& lease, uint32_t number)
    {
        if (lease.link) __builtin_trap();
        auto& head = buckets[number % BucketCount];
        lease = {head, &head, number};
        if (head) head->link = &lease.next;
        head = &lease;
    }
};

}
