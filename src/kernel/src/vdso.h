#pragma once
#include <stdint.h>
namespace brook {
struct AddressSpaceState;
// Called once before any user address spaces are constructed.
void VdsoInit();
// Caller must exclusively construct this unpublished AS, after reserving the
// image, interpreter, stack and TLS. Returns AT_SYSINFO_EHDR, or zero on OOM.
// No existing mapping is replaced. Ordinary munmap/mremap can intentionally
// break the helper, as can moving its clock page independently of its code.
uint64_t VdsoInstall(AddressSpaceState* space, uint64_t random);
}
