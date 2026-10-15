#pragma once
#include <stdint.h>

namespace brook {
// A descriptor pin is created before publication. Retaining it cannot enter
// filesystem/object code; its final callback must run outside mapping locks.
struct MappingBacking {
    uint32_t references;
    void* object;
    void (*release)(void*);
};
// Adopts one existing object pin on success; on allocation failure the caller
// still owns that pin. The descriptor's final callback consumes it exactly once.
MappingBacking* MappingBackingCreate(void* object, void (*release)(void*));
void MappingBackingRef(MappingBacking* backing);
void MappingBackingUnref(MappingBacking* backing);

enum class MappingKind : uint8_t { Anonymous, File, MemFd, Device, Image, Stack };
enum class MappingSharing : uint8_t { Private, Shared };
enum class MappingLeafOwnership : uint8_t { Managed, Borrowed };
struct MappingRange {
    uint64_t begin, end, offset, identity;
    MappingBacking* backing;
    uint32_t protection, maximumProtection;
    MappingKind kind;
    MappingSharing sharing;
    MappingLeafOwnership leafOwnership;
};
struct MappingRangeNode { MappingRange range; MappingRangeNode* next; };
struct MappingRangeTable {
    MappingRangeNode* head;
    uint64_t nextIdentity;
};

// All table access is protected by AS.mutation, except unpublished AS creation
// and last-reference teardown. Plans allocate before locking and release their
// retired descriptors after unlock and TLB acknowledgement. No independent
// permission/extents table is needed for anonymous reservations.
class MappingRangePlan {
public:
    MappingRangePlan() = default;
    ~MappingRangePlan();
    MappingRangePlan(const MappingRangePlan&) = delete;
    MappingRangePlan& operator=(const MappingRangePlan&) = delete;
    bool Prepare(uint64_t nodes);
    // A replacement covers exactly [begin,end); nullptr removes that interval.
    // Failure leaves the original table and all references unchanged.
    bool Replace(MappingRangeTable& table, uint64_t begin, uint64_t end,
                 const MappingRange* replacement = nullptr);
    bool CanReplace(const MappingRangeTable& table, uint64_t begin, uint64_t end,
                    const MappingRange* replacement = nullptr) const;
    bool Insert(MappingRangeTable& table, const MappingRange& range);
    // Requires complete coverage with no holes and permissions <= max on every
    // affected range. Preserves backing bytes/offsets and leaf ownership.
    bool Protect(MappingRangeTable& table, uint64_t begin, uint64_t end,
                 uint32_t protection);
    void Finish(); // after unlock + acknowledgement; idempotent
private:
    MappingRangeNode* Take(const MappingRange& source, uint64_t begin,
                           uint64_t end, MappingRangeTable& table);
    MappingRangeNode* spare_ = nullptr;
    MappingRangeNode* retired_ = nullptr;
    uint64_t available_ = 0;
    bool prepared_ = false;
};
const MappingRange* MappingRangeFind(const MappingRangeTable& table, uint64_t address);
uint64_t MappingRangeFindGap(const MappingRangeTable& table, uint64_t hint,
                             uint64_t length, uint64_t floor, uint64_t ceiling);
void MappingRangeDestroy(MappingRangeTable& table); // unlocked, no live users
}
