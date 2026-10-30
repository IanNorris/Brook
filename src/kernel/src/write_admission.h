#pragma once
#include <stdint.h>
namespace brook {
// Value state; the caller serializes every operation with one owner lock.
// Once closed, only the final sync owner may issue writes. Closure waits for
// all previously admitted writes, including nested partition-device calls.
class WriteAdmission {
    uintptr_t owner=0; uint64_t active=0;
public:
    bool Enter(uintptr_t caller) { if(owner && owner!=caller)return false; ++active;return true; }
    void Leave() { --active; }
    bool Close(uintptr_t caller) { if(!caller || (owner && owner!=caller) || active)return false;owner=caller;return true; }
    bool Reopen(uintptr_t caller) { if(owner!=caller || active)return false;owner=0;return true; }
    uint64_t Active() const {return active;}
    bool Closed() const {return owner!=0;}
    bool OwnedBy(uintptr_t caller) const {return owner && owner==caller;}
};
}
