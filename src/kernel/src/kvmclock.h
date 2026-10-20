#pragma once

#include <stdint.h>
#include "memory/address.h"

namespace brook {

// Detect and register the KVM paravirt clock source on the BSP.
// Call once, after ApicInit.  Returns true if pvclock is available and
// now live.  No-op on non-KVM hypervisors or bare metal.
bool KvmClockInit();

// True if pvclock was successfully registered.
bool KvmClockAvailable();
bool KvmClockStableAvailable();

// Register zeroed, permanently pinned 64-byte KVM steal records. BSP first,
// then once on each AP before scheduler entry. No-op when CPUID lacks support.
bool KvmStealInit(uint32_t cpu);
bool KvmStealAvailable(uint32_t cpu);
// Bounded version-checked read; failure leaves output unchanged.
bool KvmStealReadNs(uint32_t cpu, uint64_t* result);

// Sanitized, permanently pinned page, only on stable-clock-capable KVM hosts.
// Borrowed kernel reference; every user PTE must acquire a separate PMM ref.
PhysicalAddress KvmClockPublicPage();

// Read monotonic nanoseconds since VM start.  Returns 0 if pvclock is
// not available (callers must fall back to the LAPIC-tick clock).
//
// Safe to call from any CPU — Brook only reads the BSP's pvti struct after
// KvmClockInit; KVM updates it asynchronously. Hosts with synchronized TSCs (the
// PVCLOCK_TSC_STABLE_BIT flag) produce consistent results across CPUs.
uint64_t KvmClockReadNs();

// Bounded coherent read for consumers that require cross-CPU elapsed-time
// bounds. Requires both CPUID stable-clock capability and the snapshot flag.
// Failure leaves *result unchanged; there is no delivered-tick fallback.
bool KvmClockReadStableNs(uint64_t* result);

} // namespace brook
