#pragma once
#include "address_space.h"
#include "virtual_memory.h"

namespace brook {
// Prepared outside AS.mutation; commit while locked; retire after unlock and
// shootdown. This operation owns all obligations across blocking preparation
// and cancellation. Legacy range writers still need migration to this API.
class MappingTransaction {
public:
    MappingTransaction(Process* actor, AddressSpaceState* space);
    ~MappingTransaction();
    MappingTransaction(const MappingTransaction&) = delete;
    MappingTransaction& operator=(const MappingTransaction&) = delete;

    // Once, before destructive changes. Failure leaves AS metadata/PTEs intact.
    bool Prepare(uint64_t retiredCapacity, uint64_t freshCapacity,
                 uint64_t backingCapacity = 0);
    bool PrepareZeroed(MemTag tag, uint16_t pid);
    void Lock();
    void Unlock();
    void Changed(bool requiresShootdown = true);
    // Capture ownership while old metadata is still present; detach before
    // removing or splitting the corresponding memfd metadata.
    bool Detach(VirtualAddress address);
    VmmMapResult Publish(uint64_t index, VirtualAddress address, uint64_t flags,
                         MemTag tag, uint16_t pid);
    // Roll back while continuously holding the publication lock. Physical
    // matching does not establish an incarnation against legacy bypass writers.
    void RollbackPublished();
    // Takes an existing backing reference; callback runs after ACK, unlocked.
    bool RetireBacking(void* backing, void (*release)(void*));
    AddressSpaceState* Space() const { return space_; }

private:
    struct RetiredLeaf { uint64_t raw; bool managed; };
    struct FreshPage { PhysicalAddress physical; VirtualAddress publishedAt; uint64_t publicationEpoch; bool published, settled; };
    struct Backing { void* pointer; void (*release)(void*); };
    bool Borrowed(VirtualAddress address, uint64_t leaf) const;
    Process* actor_;
    AddressSpaceState* space_;
    RetiredLeaf* retired_ = nullptr;
    FreshPage* fresh_ = nullptr;
    Backing* backings_ = nullptr;
    uint64_t retiredCapacity_ = 0, retiredCount_ = 0;
    uint64_t freshCapacity_ = 0, backingCapacity_ = 0, backingCount_ = 0;
    uint64_t lockEpoch_ = 0;
    bool prepared_ = false, locked_ = false, shootdown_ = false;
};
}
