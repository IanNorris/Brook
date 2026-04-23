#pragma once

#include <stdint.h>

namespace brook {

// Detect and register the KVM paravirt clock source on the BSP.
// Call once, after ApicInit.  Returns true if pvclock is available and
// now live.  No-op on non-KVM hypervisors or bare metal.
bool KvmClockInit();

// True if pvclock was successfully registered.
bool KvmClockAvailable();

// Read monotonic nanoseconds since VM start.  Returns 0 if pvclock is
// not available (callers must fall back to the LAPIC-tick clock).
//
// Safe to call from any CPU — the BSP's pvti struct is only read, never
// written, after KvmClockInit.  Hosts with synchronized TSCs (the
// PVCLOCK_TSC_STABLE_BIT flag) produce consistent results across CPUs.
uint64_t KvmClockReadNs();

} // namespace brook
