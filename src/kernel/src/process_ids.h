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
    uint16_t owners[Capacity]{}; // debug job tag charged until PID release
    struct Budget { uint32_t limit=0,reserved=0,peak=0,denied=0; bool registered=false; };
    Budget budgets[257]{}; // job tags 1..256; zero means no job
    uint32_t next=1,freeHead=0,freeCount=0,pending=0;
    bool userClosed=false;
public:
    struct BudgetSnapshot { uint32_t limit,reserved,peak,denied; };
    bool RegisterJob(uint64_t tag,uint32_t limit) {
        if(!tag || tag>256 || limit>=Capacity)return false;
        auto& b=budgets[tag];
        if(b.registered)return b.limit==limit;
        b.registered=true;b.limit=limit;return true;
    }
    bool JobSnapshot(uint64_t tag,BudgetSnapshot& out) const {
        if(!tag || tag>256 || !budgets[tag].registered)return false;
        const auto& b=budgets[tag];out={b.limit,b.reserved,b.peak,b.denied};return true;
    }
    uint16_t Allocate(bool user,uint64_t tag=0) {
        if(user&&userClosed)return 0;
        if(tag && (!user || tag>256 || !budgets[tag].registered))return 0;
        if(tag && budgets[tag].limit && budgets[tag].reserved>=budgets[tag].limit) {
            if(budgets[tag].denied!=0xffffffffu)++budgets[tag].denied;
            return 0;
        }
        uint16_t pid;
        // Numeric PIDs escape into userspace (e.g. delayed kill(pid) timers).
        // Prefer never-issued IDs, then recycle in release order rather than
        // immediately aliasing the most recently exited process or thread.
        if(next<Capacity)pid=static_cast<uint16_t>(next++);
        else if(freeCount) {
            pid=freeIds[freeHead];freeHead=(freeHead+1)%Capacity;--freeCount;
        } else return 0;
        flags[pid]=user?3:1;
        owners[pid]=static_cast<uint16_t>(tag);
        if(tag) {auto& b=budgets[tag];++b.reserved;if(b.reserved>b.peak)b.peak=b.reserved;}
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
        if(owners[pid]) {--budgets[owners[pid]].reserved;owners[pid]=0;}
        flags[pid]=0;freeIds[(freeHead+freeCount)%Capacity]=pid;++freeCount;return true;
    }
    uint32_t SetUserClosed(bool closed) {userClosed=closed;return pending;}
    bool UserClosed() const {return userClosed;}
    uint32_t Pending() const {return pending;}
};
}
