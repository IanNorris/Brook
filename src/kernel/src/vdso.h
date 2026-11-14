#pragma once
#include <stdint.h>
namespace brook {
struct AddressSpaceState;
// Called once before any user address spaces are constructed.
void VdsoInit();
// Before any user address space exists: fill the private (non-KVM) clock page
// with pvclock-format records describing the invariant-TSC clock so the
// helper's CLOCK_MONOTONIC fast path works on bare metal. False leaves the page
// zeroed (syscall fallback).
bool VdsoPublishTscClock(uint64_t originTsc, uint64_t originNs, uint32_t mul, int8_t shift);
// Caller must exclusively construct this unpublished AS, after reserving the
// image, interpreter, stack and TLS. Returns AT_SYSINFO_EHDR, or zero on OOM.
// No existing mapping is replaced. Ordinary munmap/mremap can intentionally
// break the helper, as can moving its clock page independently of its code.
uint64_t VdsoInstall(AddressSpaceState* space, uint64_t random);
}
