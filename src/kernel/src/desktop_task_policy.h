#pragma once

#include <stdint.h>

namespace brook {

inline bool DesktopTaskMayEnd(uint32_t callerUid, uint32_t callerPid, uint32_t targetPid,
                              uint32_t targetUid, uint32_t targetEffectiveUid, bool system)
{
    return !system && targetPid > 1 && callerPid != targetPid &&
           callerUid == targetUid && targetUid == targetEffectiveUid;
}

}
