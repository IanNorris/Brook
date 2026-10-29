#pragma once
#include "mapping_fault.h"
#include <stdint.h>
namespace brook {
struct Process;
struct AddressSpaceState;
struct MappingRangeTable;
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
    void Commit();
private:
    struct Page;
    Page* pages_=nullptr;
    uint64_t capacity_=0,count_=0;
    bool committed_=false;
    Process* actor_;
};
// Explicit single-task experiment; opt/page-store-device is absent by default.
void PageableMemoryBootInit();
// Bounded healthy-memory reserve replenishment; starts only for an enabled store.
void PageableMemoryStartReserveWorker();
int64_t PageableMemoryControl(Process* actor,uint64_t operation,uint64_t value);
MappingFaultResult PageableMemoryFault(Process* actor,AddressSpaceState* space,uint64_t address,uint32_t access,bool allowReclaim=false);
// Conservative experiment-only raw-uaccess barrier: before unaudited syscalls,
// restore all pageable leaves while no handler lock has yet been acquired.
// This deliberately sacrifices paging benefit across syscalls until raw user
// pointer consumers have proper exclusion. Read/write use lexical leases;
// Scalar paging/mapping/exit operations also skip this barrier.
int PageableMemoryPrepareSyscall(Process* actor,uint64_t number);
}
