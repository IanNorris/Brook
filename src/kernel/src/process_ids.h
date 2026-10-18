#pragma once
#include <stdint.h>

namespace brook {
// Value state protected by the scheduler's PID lock. PID zero is reserved.
// User reservations survive allocation -> construction -> registry publication;
// failed construction releases the same reservation when its PID is returned.
template<uint32_t Capacity>
class ProcessIdPool {
    static_assert(Capacity>1&&Capacity<=65536,"PID pool must fit uint16_t");
    uint16_t freeIds[Capacity]{};
    uint8_t flags[Capacity]{}; // bit 0: allocated; bit 1: unpublished user birth
    uint32_t next=1,freeCount=0,pending=0;
    bool userClosed=false;
public:
    uint16_t Allocate(bool user) {
        if(user&&userClosed)return 0;
        uint16_t pid;
        if(freeCount)pid=freeIds[--freeCount];
        else {
            if(next>=Capacity)return 0;
            pid=static_cast<uint16_t>(next++);
        }
        flags[pid]=user?3:1;
        if(user)++pending;
        return pid;
    }
    // Must run while registry/ready locks still prevent dispatch and removal.
    // Calling after publication locks are dropped could clear a reused PID's
    // new reservation if the original child runs and exits immediately.
    bool Publish(uint16_t pid) {
        if(!pid||pid>=Capacity||!(flags[pid]&2))return false;
        flags[pid]&=~uint8_t(2);--pending;return true;
    }
    bool Release(uint16_t pid) {
        if(!pid||pid>=Capacity||!(flags[pid]&1))return false;
        if(flags[pid]&2)--pending;
        flags[pid]=0;freeIds[freeCount++]=pid;return true;
    }
    uint32_t SetUserClosed(bool closed) {userClosed=closed;return pending;}
    bool UserClosed() const {return userClosed;}
    uint32_t Pending() const {return pending;}
};
}
