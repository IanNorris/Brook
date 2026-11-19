#pragma once

#include <stdint.h>

namespace brook {

constexpr uint64_t CoreAutoMaxBytes=2ull*1024*1024*1024;
constexpr uint64_t CoreAutoTotalBytes=8ull*1024*1024*1024;
constexpr uint32_t CoreAutoKeep=5;
constexpr uint64_t CoreAutoTimeoutMs=20000;
constexpr uint64_t CoreAutoIntervalMs=60000;

enum class CoreAutoMode : uint32_t { Uid1000, AllUsers, Off };

constexpr bool CoreAutoSignal(int signal)
{
    switch(signal) {
        case 4: case 5: case 6: case 7: case 8: case 11: case 31:return true;
        default:return false;
    }
}

struct CoreResourceLimits {
    uint64_t soft=~uint64_t{0};
    uint64_t hard=~uint64_t{0};
};

constexpr uint64_t CoreAutoBudget(CoreResourceLimits limits)
{
    return limits.soft<CoreAutoMaxBytes ? limits.soft : CoreAutoMaxBytes;
}

constexpr bool CoreAutoEligible(CoreAutoMode mode,bool kernel,uint32_t uid,uint32_t euid,uint32_t suid,
                                int signal,CoreResourceLimits limits)
{
    return !kernel && mode!=CoreAutoMode::Off && (signal==0 || CoreAutoSignal(signal)) &&
           limits.soft && uid==euid && uid==suid && (uid==1000 || mode==CoreAutoMode::AllUsers);
}

constexpr bool CoreAutoExpired(uint64_t start,uint64_t now)
{
    return now-start>=CoreAutoTimeoutMs;
}

constexpr bool CoreAutoNeedsRotation(uint64_t used,uint32_t count,uint64_t incoming)
{
    return count>=CoreAutoKeep || incoming>CoreAutoTotalBytes || used>CoreAutoTotalBytes-incoming;
}

struct CoreAutoRetention {
    uint64_t used=0,oldestTime=0;
    uint32_t count=0;
    bool add(uint64_t bytes,uint64_t time,bool& oldest) {
        if(bytes>~uint64_t{0}-used)return false;
        used+=bytes;
        oldest=!count++ || time<oldestTime;
        if(oldest)oldestTime=time;
        return true;
    }
    bool needsRotation(uint64_t incoming) const {
        return CoreAutoNeedsRotation(used,count,incoming);
    }
};

// A full table refuses new keys until an entry expires. Evicting a live
// entry would let process churn defeat the per-executable rate cap.
struct CoreAutoRate {
    struct Entry {uint64_t key=0, at=0;} entries[64]{};
    bool Admit(uint64_t key,uint64_t now) {
        Entry* vacant=nullptr;
        for(auto& e:entries){
            const bool live=e.key && now-e.at<CoreAutoIntervalMs;
            if(live && e.key==key)return false;
            if(!live && !vacant)vacant=&e;
        }
        if(!vacant || !key)return false;
        *vacant={key,now};
        return true;
    }
};

}
