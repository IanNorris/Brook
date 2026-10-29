#pragma once
#include "mapping_fault.h"
#include <stdint.h>
namespace brook {
struct PageableBackingStatus {
    bool configured=false, usageValid=false;
    uint64_t logicalCapacityBytes=0, logicalUsedBytes=0;
};
// Never waits for store I/O; configured usage can be unknown while busy.
PageableBackingStatus PageableMemoryBackingStatus();
struct Process;
struct AddressSpaceState;
struct MappingRangeTable;
struct UserMappingRequest;
struct MappingBacking;
struct MappingRange;
// Prepare empty replacement metadata outside AS mutation. TransferLocked moves
// existing entry ownership only after metadata/PTE commit; no backing I/O or
// token/reference duplication is needed. Declare before the transaction.
class PageableMemoryGrowth {
public:
    PageableMemoryGrowth(Process*,AddressSpaceState*);
    ~PageableMemoryGrowth();
    PageableMemoryGrowth(const PageableMemoryGrowth&) = delete;
    PageableMemoryGrowth& operator=(const PageableMemoryGrowth&) = delete;
    bool Prepare(uint64_t bytes);
    bool ValidateLocked(const MappingRange&) const;
    bool DestinationDisjointLocked(const MappingRange&,uint64_t begin,uint64_t end) const;
    void TransferLocked(const MappingRange&);
    MappingBacking* Backing() const{return replacement_;}
private:
    Process* actor_;
    AddressSpaceState* space_;
    MappingBacking* replacement_=nullptr;
    unsigned failure_=0;
};
// Outer fork lifetime: source snapshots/pins and child storage outlive the
// final transaction and its parent COW shootdown. No store lock is taken by
// the Locked methods; token retention and Recovery copies happen unlocked.
class PageableMemoryFork {
public:
    PageableMemoryFork(Process*,AddressSpaceState*);
    ~PageableMemoryFork();
    PageableMemoryFork(const PageableMemoryFork&) = delete;
    PageableMemoryFork& operator=(const PageableMemoryFork&) = delete;
    static bool BudgetLocked(const MappingRangeTable&,uint64_t& regions,uint64_t& pages);
    bool Prepare(uint64_t regions,uint64_t pages,uint64_t ranges);
    bool CaptureLocked();
    bool PrepareUnlocked(uint16_t childPid,AddressSpaceState* destination);
    bool ValidateLocked() const;
    MappingBacking* ChildBackingFor(MappingBacking*) const;
    bool FailAfterCow() const{return failure_==5;}
private:
    struct Record;
    struct Span;
    Record* Find(MappingBacking*) const;
    Record* records_=nullptr;
    void* snapshots_=nullptr;
    uint64_t* index_=nullptr;
    Span* spans_=nullptr;
    uint64_t spanCapacity_=0,spanCount_=0;
    uint64_t capacity_=0,count_=0,pageCapacity_=0,pageCount_=0,indexCapacity_=0;
    Process* actor_;
    AddressSpaceState* source_;
    unsigned failure_=0;
};
// Declare before the mapping transaction. Capture/Commit run under AS mutation;
// destruction releases captured slots/recovery refs only after transaction TLB
// retirement. Preparation is fallible and must precede the mutation gate.
class PageableMemoryRetirement {
public:
    explicit PageableMemoryRetirement(Process* actor);
    ~PageableMemoryRetirement();
    PageableMemoryRetirement(const PageableMemoryRetirement&) = delete;
    PageableMemoryRetirement& operator=(const PageableMemoryRetirement&) = delete;
    static uint64_t Count(const MappingRangeTable&,uint64_t begin,uint64_t end);
    bool Prepare(uint64_t pages);
    bool Capture(AddressSpaceState*,uint64_t begin,uint64_t end);
    // resetToZero retains the reservation after MADV_DONTNEED; removal leaves
    // an inaccessible Discarded entry outside all surviving fragments.
    void Commit(bool resetToZero=false);
private:
    struct Page;
    Page* pages_=nullptr;
    uint64_t capacity_=0,count_=0;
    bool committed_=false;
    Process* actor_;
};
// Explicit single-task experiment; opt/page-store-device is absent by default.
// False leaves normal mmap unchanged. True returns an enrolled result/error.
bool PageableMemoryMapAnonymous(Process*,const UserMappingRequest&,uint64_t flags,int64_t& result);
void PageableMemoryBootInit();
// Bounded healthy-memory reserve replenishment; starts only for an enabled store.
void PageableMemoryStartReserveWorker();
int64_t PageableMemoryControl(Process* actor,uint64_t operation,uint64_t value);
MappingFaultResult PageableMemoryFault(Process* actor,AddressSpaceState* space,uint64_t address,uint32_t access,bool allowReclaim=false);
// Conservative experiment-only raw-uaccess barrier: before unaudited syscalls,
// restore all pageable leaves while no handler lock has yet been acquired.
// This deliberately sacrifices paging benefit across syscalls until raw user
// pointer consumers have proper exclusion. Audited read/write, pathname and
// fstat handlers use leases/snapshots; scalar mapping/fd operations skip it.
int PageableMemoryPrepareSyscall(Process* actor,uint64_t number);
}
