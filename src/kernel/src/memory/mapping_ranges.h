#pragma once
#include <stdint.h>
#include "file_mapping_registry.h"

namespace brook {
struct Vnode;
// A descriptor pin is created before publication. Retaining it cannot enter
// filesystem/object code; its final callback must run outside mapping locks.
struct MappingBacking {
    uint32_t references;
    void* object;
    void (*release)(void*);
    FileObject* file = nullptr; // borrowed; object pin retains the live inode
    Vnode* fileVnode = nullptr; // borrowed; the same object pin owns this vnode
};
// Adopts one existing object pin on success; on allocation failure the caller
// still owns that pin. The descriptor's final callback consumes it exactly once.
MappingBacking* MappingBackingCreate(void* object, void (*release)(void*));
void MappingBackingRef(MappingBacking* backing);
void MappingBackingUnref(MappingBacking* backing);

enum class MappingKind : uint8_t { Anonymous, File, MemFd, Device, Image, Stack, Heap, Pageable };
enum class MappingSharing : uint8_t { Private, Shared };
enum class MappingLeafOwnership : uint8_t { Managed, Borrowed };
// madvise fork inheritance (Linux VM_DONTCOPY / VM_WIPEONFORK). Skip leaves
// the range out of a forked child; Wipe gives the child the range without
// its pages (private anonymous only, so the child demand-faults zeroes).
enum : uint8_t { MappingForkSkip = 1, MappingForkWipe = 2 };
enum : uint8_t { MappingSecret = 1, MappingSecretFd = 2 };
struct MappingRange {
    uint64_t begin, end, offset, identity;
    MappingBacking* backing;
    uint32_t protection, maximumProtection;
    MappingKind kind;
    MappingSharing sharing;
    MappingLeafOwnership leafOwnership;
    uint8_t forkFlags = 0; // MappingFork*; copied by splits, fork and exec-free paths
    uint8_t dumpFlags = 0; // MappingSecret*; range-local, independent of fork policy.
};
bool MappingRangeValid(const MappingRange& range);
struct MappingRangeNode {
    MappingRange range;
    MappingRangeNode* next;
    FileMappingRegistration fileRegistration;
    // Intrusive AVL index by range.begin; the list remains the ordered
    // iteration/teardown representation. Both use the same AS mutation lock.
    MappingRangeNode* left = nullptr;
    MappingRangeNode* right = nullptr;
    uint32_t height = 1;
};
struct MappingRangeTable {
    MappingRangeNode* head;
    uint64_t nextIdentity;
    AddressSpaceState* owner = nullptr;
    // Positive metadata lookups only. The same AS mutation lock that protects
    // the list protects this cache; unpublished construction is exclusive.
    // Never copy a table/cache into another address space.
    static constexpr unsigned LookupSlots = 16;
    mutable const MappingRange* lookup[LookupSlots] = {};
    MappingRangeNode* root = nullptr;
};
// First list node whose range ends after address (the containing range, or
// the next one after a hole), found through the AVL index in O(log n). Ranges
// are disjoint and ordered, so end order equals the index's begin order.
// AS mutation (or exclusive construction/teardown) required.
inline MappingRangeNode* MappingRangeLastEndingAtOrBefore(const MappingRangeTable& table,uint64_t address) {
    MappingRangeNode* found=nullptr;
    for(auto* node=table.root;node;) {
        if(node->range.end<=address){found=node;node=node->right;}
        else node=node->left;
    }
    return found;
}
inline MappingRangeNode* MappingRangeFirstEndingAfter(const MappingRangeTable& table,uint64_t address) {
    auto* predecessor=MappingRangeLastEndingAtOrBefore(table,address);
    return predecessor?predecessor->next:table.head;
}
// Callers without prepared per-page content retirement cannot remove even a
// whole node: it may be one fragment whose siblings keep the Region alive.
inline bool MappingRangeRemovalAllowed(const MappingRangeTable& table,uint64_t begin,uint64_t end) {
    for(auto* node=MappingRangeFirstEndingAfter(table,begin);node && node->range.begin<end;node=node->next) {
        const auto& r=node->range;
        if(r.kind==MappingKind::Pageable && r.begin<end && begin<r.end)return false;
    }
    return true;
}

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
    bool CanProtect(const MappingRangeTable& table, uint64_t begin, uint64_t end,
                    uint32_t protection) const;
    // Same coverage/split contract as Protect; replaces forkFlags bits in
    // mask with bits. Leaves, protection and backing are unchanged.
    bool SetForkFlags(MappingRangeTable& table, uint64_t begin, uint64_t end,
                      uint8_t mask, uint8_t bits);
    bool CanSetForkFlags(const MappingRangeTable& table, uint64_t begin, uint64_t end) const;
    bool SetDumpExcluded(MappingRangeTable& table, uint64_t begin, uint64_t end, bool exclude);
    bool Extend(MappingRangeTable& table, uint64_t identity, uint64_t end);
    void Finish(); // after unlock + acknowledgement; idempotent
private:
    MappingRangeNode* Take(const MappingRange& source, uint64_t begin,
                           uint64_t end, MappingRangeTable& table);
    // Split at begin/end and apply to every covered node (preflighted).
    template<typename Apply>
    void SplitApply(MappingRangeTable& table, uint64_t begin, uint64_t end, Apply apply);
    bool CanSplitCovered(const MappingRangeTable& table, uint64_t begin, uint64_t end,
                         uint32_t protection, bool checkProtection) const;
    MappingRangeNode* spare_ = nullptr;
    MappingRangeNode* retired_ = nullptr;
    uint64_t available_ = 0;
    bool prepared_ = false;
};
const MappingRange* MappingRangeFind(const MappingRangeTable& table, uint64_t address);
// Containing range, or the next range after a hole. AS mutation required.
const MappingRange* MappingRangeAtOrAfter(const MappingRangeTable& table,uint64_t address);
uint64_t MappingRangeFindGap(const MappingRangeTable& table, uint64_t hint,
                             uint64_t length, uint64_t floor, uint64_t ceiling);
void MappingRangeDestroy(MappingRangeTable& table); // unlocked, no live users
}
