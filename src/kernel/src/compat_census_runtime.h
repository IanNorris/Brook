#pragma once
// compat_census_runtime.h -- kernel entry points of the compatibility census.
#include <stdint.h>

namespace brook {
// Called by the syscall dispatcher for results in [-4095,-1] only. args holds
// the six syscall arguments; origin is a CompatSyscallOrigin value.
void CompatCensusObserve(uint64_t num, int64_t result, const uint64_t* args, uint64_t origin);
// Debug bridge: one JSON line sorted by count (limit 0 = all).
void CompatCensusReplyJson(uint32_t limit);
void CompatCensusReset();
// Plain-text top-N summary for logs; returns bytes written (NUL excluded).
uint32_t CompatCensusFormatSummary(char* out, uint32_t capacity, uint32_t limit);
// Observations since boot or reset (changes whenever the census changes).
uint64_t CompatCensusTotal();
}
