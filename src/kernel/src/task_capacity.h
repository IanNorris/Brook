#pragma once
#include <stdint.h>

namespace brook {
// One shared ceiling for task IDs used by the scheduler and memory owner
// lists. Raising this also requires changing the PTE owner representation;
// virtual_memory.h deliberately asserts that the two remain compatible.
inline constexpr uint32_t TaskIdCapacity=1024;
}
