#pragma once
#include <stdint.h>
namespace brook {
// Completed-only reservation subset. Callers serialize access with the canonical
// resource's mutex. Pending work is never represented by a signaled snapshot.
struct GpuReservation {
    uint32_t pending=0;
    bool uncertain=false,capturing=false;
    uint64_t imports=0,completedWrite=0,capturedWrite=0,captureWrite=0;
    bool Begin() {
        if(uncertain || capturing || pending==~uint32_t{0})return false;
        ++pending;return true;
    }
    bool Finish(bool completed) {
        if(!pending){uncertain=true;return false;}
        --pending;if(!completed)uncertain=true;
        return !uncertain;
    }
    bool Ready() const { return !uncertain && !pending && !capturing; }
    // A retained image is captured once then cloned by Wayland. Reusing the
    // producer allocation for a new frame requires a new imported WRITE fence.
    bool BeginCapture() {
        if(!Ready() || completedWrite==capturedWrite)return false;
        captureWrite=completedWrite;capturing=true;return true;
    }
    void FinishCapture(bool safe,bool captured) {
        if(!capturing){uncertain=true;return;}
        if(!safe)uncertain=true;
        if(captured)capturedWrite=captureWrite;
        capturing=false;captureWrite=0;
    }
    bool ImportCompleted(uint32_t usage) {
        if(!usage || (usage&~3u) || uncertain || imports==~uint64_t{0})return false;
        ++imports;if(usage&2u)completedWrite=imports;
        return true;
    }
};
}
