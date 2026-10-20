#pragma once

#include <stdint.h>
#include "memory/address.h"

namespace brook {

// Detect and register the KVM paravirt clock source on the BSP.
// Call once, after ApicInit.  Returns true if pvclock is available and
// now live.  No-op on non-KVM hypervisors or bare metal.
bool KvmClockInit();
// On each AP, after its GS CPU identity is installed and before timer/scheduler
// entry. A vCPU must use its own host-updated record, including after VM resume.
bool KvmClockInitOnCpu(uint32_t cpu);
uint64_t KvmClockRegisteredCpuMask();

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
// Reads the current CPU's record, retrying on migration or a concurrent update.
// Never substitute a stale BSP record for an unregistered AP.
uint64_t KvmClockReadNs();

// Bounded coherent read for consumers that require cross-CPU elapsed-time
// bounds. Requires both CPUID stable-clock capability and the snapshot flag.
// Failure leaves *result unchanged; there is no delivered-tick fallback.
bool KvmClockReadStableNs(uint64_t* result);

} // namespace brook
