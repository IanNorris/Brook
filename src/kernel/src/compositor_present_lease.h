#pragma once
#include <stdint.h>
namespace brook {
// Allow slow GPU frames under contention, but return display control to the
// software compositor within a bounded interval after the last present.
static constexpr uint64_t COMPOSITOR_PRESENT_LEASE_MS=500;
// Caller serializes access with the display mapping lock. A lease merely
// defers software framebuffer copies; it never transfers display ownership.
struct CompositorPresentLease {
    uint64_t owner=0,deadline=0;
    bool Active(uint64_t now) const {return owner && now<deadline;}
    bool Expire(uint64_t now) {if(owner && now>=deadline){owner=deadline=0;return true;}return false;}
    bool CanAcquire(uint64_t candidate,uint64_t now) const {return candidate && (!Active(now) || owner==candidate);}
    bool Acquire(uint64_t candidate,uint64_t now,uint64_t duration) {
        if(!duration || !CanAcquire(candidate,now) || now>~uint64_t{0}-duration)return false;
        owner=candidate;deadline=now+duration;return true;
    }
    bool Release(uint64_t candidate) {if(!candidate || owner!=candidate)return false;owner=deadline=0;return true;}
    bool Clear() {if(!owner)return false;owner=deadline=0;return true;}
};
}
