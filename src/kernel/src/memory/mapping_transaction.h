#pragma once
#include "address_space.h"
#include "virtual_memory.h"

namespace brook {
struct MappingFaultSnapshot {
    MappingRange range;
    uint64_t addressSpaceIdentity, generation, fileGeneration;
};
enum class MappingFaultCommit { Mapped, Existing, Stale, Failed };
// Prepared outside AS.mutation; commit while locked; retire after unlock and
// shootdown. This operation owns all obligations across blocking preparation
// and cancellation. Legacy range writers still need migration to this API.
class MappingTransaction {
public:
    MappingTransaction(Process* actor, AddressSpaceState* space,MappingBacking* incoming = nullptr,
                       MappingBacking* onlyBacking = nullptr);
    ~MappingTransaction();
    MappingTransaction(const MappingTransaction&) = delete;
    MappingTransaction& operator=(const MappingTransaction&) = delete;

    // Once, before destructive changes. Failure leaves AS metadata/PTEs intact.
    bool Prepare(uint64_t retiredCapacity, uint64_t freshCapacity,
                 uint64_t backingCapacity = 0);
    bool PrepareZeroed(MemTag tag, uint16_t pid);
    // Adopts an existing frame reference on success when managed=true. A
    // borrowed candidate instead remains owned by a retained range backing.
    bool StagePage(uint64_t index, PhysicalAddress physical, bool managed);
    PhysicalAddress PreparedPage(uint64_t index) const;
    bool Lock(); // OOM acquiring the file set leaves metadata unchanged
    void Unlock();
    void Changed(bool requiresShootdown = true);
    // Capture ownership while old metadata is still present; detach before
    // removing or splitting the corresponding memfd metadata.
    bool Detach(VirtualAddress address);
    VmmMapResult Publish(uint64_t index, VirtualAddress address, uint64_t flags,
                         MemTag tag, uint16_t pid, bool accessible = true);
    // Roll back while continuously holding the publication lock. Physical
    // matching does not establish an incarnation against legacy bypass writers.
    void RollbackPublished();
    // Takes an existing backing reference; callback runs after ACK, unlocked.
    bool RetireBacking(void* backing, void (*release)(void*));
    bool RetireFrame(PhysicalAddress physical); // adopts an already detached managed ref
    void RequireShootdown();
    // Range storage is prepared outside the lock. These APIs become the sole
    // source of range metadata as each legacy writer is migrated.
    bool PrepareRanges(uint64_t nodes);
    // Preflights both metadata splits and leaf retirement before any mutation,
    // then detaches old leaves and commits metadata together. New leaves are
    // installed separately while still locked, after preparing all resources.
    bool ReplaceMapping(uint64_t begin, uint64_t end, const MappingRange* replacement = nullptr);
    // Caller provides nonallocating retirement storage and must release captured
    // frame references only after this transaction is destroyed (full TLB ACK).
    // Callback runs under PT spinlock: no allocation, blocking or unref.
    bool RemoveMappingWithRetirer(uint64_t begin,uint64_t end,VmmRetireLeaf retire,void* context);
    bool CanReplaceRange(uint64_t begin, uint64_t end, const MappingRange* replacement = nullptr) const;
    bool RangeLeafCount(uint64_t begin, uint64_t end, uint64_t& count) const;
    bool InsertRange(const MappingRange& range);
    bool ExtendRange(uint64_t identity, uint64_t end);
    bool ProtectRange(uint64_t begin, uint64_t end, uint32_t protection);
    bool ProtectMapping(uint64_t begin, uint64_t end, uint32_t protection);
    // Snapshot backing is retained by this transaction through retirement.
    // One snapshot pin has inline ownership so it can be captured before sizing
    // other preparation. Additional distinct backing pins use prepared slots.
    bool CaptureMapping(uint64_t address, MappingFaultSnapshot& snapshot);
    bool CaptureRange(uint64_t address, uint32_t access, MappingFaultSnapshot& snapshot);
    bool ValidateMapping(const MappingFaultSnapshot& snapshot, uint64_t address) const;
    bool ValidateRange(const MappingFaultSnapshot& snapshot, uint64_t address, uint32_t access) const;
    MappingFaultCommit PublishFault(const MappingFaultSnapshot& snapshot, uint64_t index,
                                    VirtualAddress address, uint32_t access, bool copyOnWrite);
    AddressSpaceState* Space() const { return space_; }

private:
    struct RetiredLeaf { uint64_t raw; bool managed; };
    struct FreshPage { PhysicalAddress physical; VirtualAddress publishedAt; uint64_t publicationEpoch; bool published, settled, managed; };
    struct Backing { void* pointer; void (*release)(void*); };
    bool Borrowed(VirtualAddress address, uint64_t leaf) const;
    bool HoldBacking(void* backing, void (*release)(void*));
    static void CaptureDetachedLeaf(VirtualAddress address, uint64_t leaf, void* context);
    static bool PrivateLeaf(VirtualAddress address, uint64_t leaf, void* context);
    Process* actor_;
    AddressSpaceState* space_;
    RetiredLeaf* retired_ = nullptr;
    FreshPage* fresh_ = nullptr;
    Backing* backings_ = nullptr;
    uint64_t retiredCapacity_ = 0, retiredCount_ = 0;
    uint64_t freshCapacity_ = 0, backingCapacity_ = 0, backingCount_ = 0;
    uint64_t lockEpoch_ = 0;
    bool prepared_ = false, locked_ = false, shootdown_ = false;
    MappingRangePlan ranges_;
    MappingBacking* snapshotBacking_ = nullptr;
    MappingBacking* incoming_;
    MappingBacking* onlyBacking_;
    FileMappingGateSet fileGates_;
};
// Narrow truncate foundation: the caller retains the file/backing and owns its
// file gate through this object's destruction (including ACK). This type cannot
// change range metadata, and never locks other file gates.
// Intervals are page-aligned, half-open FILE offsets, not virtual addresses.
// Resize callers must prepare private restoration before any detach and ACK
// EVERY target AS before restoring. This primitive does not perform filesystem
// resize, flush canonical pages, or recover from unknown resulting EOF.
class FileMappingInvalidation {
public:
    FileMappingInvalidation(Process* actor, AddressSpaceState* space, MappingBacking* backing);
    ~FileMappingInvalidation();
    FileMappingInvalidation(const FileMappingInvalidation&) = delete;
    FileMappingInvalidation& operator=(const FileMappingInvalidation&) = delete;
    bool Prepare(uint64_t leafCapacity); // once, outside AS mutation
    // Optional, once after Prepare and before Detach, outside AS mutation.
    // Reserve one boundary copy per populated managed-private range before
    // destructive work. Resulting EOF need not be known until restoration.
    bool PreparePrivateRestore();
    void LockTargetFileAlreadyHeld();
    void Unlock(); // only releases AS mutation; caller still owns file gate
    bool Count(uint64_t fileBegin, uint64_t fileEnd, uint64_t& count) const;
    // Full preflight before any detach. Metadata/registrations remain intact.
    // Managed PTE refs survive until destructor's full TLB acknowledgement;
    // borrowed PTEs lose map accounting but never transfer a frame reference.
    bool Detach(uint64_t fileBegin, uint64_t fileEnd);
    // Unlock AS and ACK once, retaining all detached references. Seals detach.
    void Acknowledge();
    // Requires preparation, local ACK, target gate and AS lock. Caller must
    // first ACK all target spaces. Restores only noncanonical managed private
    // data below min(old EOF, observed resulting EOF); partial tails use copies.
    // Canonical/shared/borrowed leaves remain detached for lazy reload.
    bool RestorePrivate(uint64_t validPrefix);
    // Exact rollback BEFORE any backend resize attempt (post-ACK flush failed).
    // Includes canonical/shared/borrowed aliases, needs no boundary preparation.
    // Caller must ACK every target space first; preserves original bytes/PTEs.
    bool RestoreAll();
private:
    struct Leaf {
        uint64_t address,offset,raw;
        bool managed,privateData;
    };
    bool Restore(bool all,uint64_t validPrefix);
    static void Capture(VirtualAddress address, uint64_t raw, void* context);
    void RequireTargetGate() const;
    Process* actor_;
    AddressSpaceState* space_;
    MappingBacking* backing_;
    Leaf* leaves_ = nullptr;
    PhysicalAddress* boundaries_ = nullptr;
    uint64_t boundaryCount_ = 0;
    uint64_t capacity_ = 0, count_ = 0;
    bool prepared_ = false, locked_ = false, shootdown_ = false;
    bool restoreRequested_ = false, restorePrepared_ = false;
    bool acknowledged_ = false, restored_ = false;
};

}
