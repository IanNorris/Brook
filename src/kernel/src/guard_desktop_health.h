#pragma once
#include <stdint.h>

namespace brook {

// A new desktop needs evidence of physical presentation after admission as
// well as time for failures in asynchronously launched services to surface.
constexpr uint64_t GuardDesktopHealthWindowMs = 60000;

inline bool GuardDesktopHealthReady(uint64_t start, uint64_t now,
                                    uint64_t firstCount, uint64_t presents)
{
    return now >= start && now - start >= GuardDesktopHealthWindowMs &&
           presents >= firstCount && presents - firstCount >= 2;
}

}
