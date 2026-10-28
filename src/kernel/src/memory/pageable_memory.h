#pragma once
#include "mapping_fault.h"
#include <stdint.h>
namespace brook {
struct Process;
struct AddressSpaceState;
// Explicit single-task experiment; opt/page-store-device is absent by default.
void PageableMemoryBootInit();
int64_t PageableMemoryControl(Process* actor,uint64_t operation,uint64_t value);
MappingFaultResult PageableMemoryFault(Process* actor,AddressSpaceState* space,uint64_t address,uint32_t access,bool allowReclaim=false);
// Conservative experiment-only raw-uaccess barrier: before unaudited syscalls,
// restore all pageable leaves while no handler lock has yet been acquired.
// This deliberately sacrifices paging benefit across syscalls until raw user
// pointer consumers have proper exclusion. Read/write use lexical leases;
// scalar paging/exit/unmap also skip this barrier.
int PageableMemoryPrepareSyscall(Process* actor,uint64_t number);
}
