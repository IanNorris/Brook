#pragma once
#include <stdint.h>
namespace brook {
// Completed-only reservation subset. Callers serialize access with the canonical
// resource's mutex. Pending work is never represented by a signaled snapshot.
struct GpuReservation {
    uint32_t pending=0;
    bool uncertain=false;
    uint64_t imports=0,completedWrite=0;
    bool Begin() {
        if(uncertain || pending==~uint32_t{0})return false;
        ++pending;return true;
    }
    bool Finish(bool completed) {
        if(!pending){uncertain=true;return false;}
        --pending;if(!completed)uncertain=true;
        return !uncertain;
    }
    bool Ready() const { return !uncertain && !pending; }
    bool ImportCompleted(uint32_t usage) {
        if(!usage || (usage&~3u) || uncertain || imports==~uint64_t{0})return false;
        ++imports;if(usage&2u)completedWrite=imports;
        return true;
    }
};
}
