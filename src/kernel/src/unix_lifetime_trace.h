#pragma once
#include <stdint.h>
namespace brook {
enum class UnixLifeEvent : uint32_t {
    Allocate=1, Publish, Retain, Release, RegistryPin, Finalize, DirectFree,
    HeapAttempt, HeapFree, Bind, Listen, SnapshotInstall, CopyRetain, Claim, HeapAllocate, HeapWriteFree
};
struct UnixLifeRecord {
    uint64_t sequence, handle, caller, other, size;
    uint32_t previous, next;
    uint16_t pid, tgid;
    int32_t fd;
    UnixLifeEvent event;
    uint32_t generation;
};
static_assert(sizeof(UnixLifeRecord) == 64, "diagnostic decoder ABI");
struct UnixLifeWatch { uint64_t handle, size; uint32_t generation, active; };
// Every method runs under the recorder lock. No allocations, I/O, or callbacks.
struct UnixLifeCore {
    uint64_t sequence, overwritten, saturated;
    uint32_t enabled, generation;
    UnixLifeWatch watches[128];
    UnixLifeRecord records[4096];
    void Add(UnixLifeRecord record) {
        uint64_t seq = ++sequence;
        if (seq > 4096) ++overwritten;
        auto& dst = records[(seq - 1) % 4096];
        __atomic_store_n(&dst.sequence, 0, __ATOMIC_RELEASE);
        record.sequence = 0;
        dst = record;
        __atomic_store_n(&dst.sequence, seq, __ATOMIC_RELEASE);
    }
    void Event(UnixLifeRecord record) {
        for (const auto& watch : watches)
            if (watch.handle == record.handle) { record.generation = watch.generation; break; }
        Add(record);
    }
    void Watch(UnixLifeRecord record) {
        UnixLifeWatch* available = nullptr;
        for (auto& watch : watches) {
            if (watch.handle == record.handle) { available = &watch; break; }
            if (!watch.active && !available) available = &watch;
        }
        record.generation = ++generation;
        if (available) *available = {record.handle, record.size, record.generation, 1};
        else ++saturated;
        Add(record);
    }
    void Heap(UnixLifeRecord record, bool actual) {
        for (auto& watch : watches) {
            if (!watch.handle) continue;
            bool overlap = record.other == watch.handle;
            if (record.size && record.other < watch.handle + watch.size &&
                watch.handle < record.other + record.size) overlap = true;
            if (!overlap) continue;
            record.handle = watch.handle;
            record.generation = watch.generation;
            record.previous = watch.active;
            record.next = actual ? 0 : watch.active;
            Add(record);
            // Keep tombstone until this watch slot is reused, so stale frees
            // still have a generation even after legitimate deallocation.
            if (actual) watch.active = 0;
        }
    }
};
void UnixLifeControl(bool enabled);
void UnixLifeHeap(void* address, uint64_t size, UnixLifeEvent event, uint64_t caller);
struct FdEntry;
void UnixLifePublished(int fd, const FdEntry& entry);
struct FdClaimResult;
void UnixLifeClaim(int fd, const FdClaimResult& claim);
}
