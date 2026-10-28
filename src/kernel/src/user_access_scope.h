#pragma once
#include "memory/user_access_ranges.h"
namespace brook {
struct Process;
struct AddressSpaceState;
// Lexical eviction exclusion only, NOT a physical/DMA pin or protection against
// raced munmap/mprotect. Initially admitted only for exclusive paging test ASes.
class UserAccessScope {
public:
    explicit UserAccessScope(Process* actor);
    ~UserAccessScope();
    UserAccessScope(const UserAccessScope&)=delete;
    UserAccessScope& operator=(const UserAccessScope&)=delete;
    // No active scope preserves the historical validation-only contract.
    // A scope registers coverage before the caller prefaults or touches bytes.
    static bool PinCurrent(uint64_t address,uint64_t length);
private:
    Process* actor_=nullptr;
    AddressSpaceState* space_=nullptr;
    UserAccessScope* previous_=nullptr;
    UserAccessLease lease_;
};
}
