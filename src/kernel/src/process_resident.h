#pragma once

#include "memory/resident_pages.h"

namespace brook {

// Process context with a protected kernel operation. Hold no VFS or
// scheduler locks across this identity-bound address-space walk.
int ProcessMeasureResident(uint16_t pid, uint64_t incarnation, ResidentPages& resident);

}
