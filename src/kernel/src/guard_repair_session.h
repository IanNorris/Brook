#pragma once
#include <stdint.h>
namespace brook {
struct Device;struct Process;struct Vnode;
// QEMU-only test: a preen session followed by a distinct read-only verifier.
bool GuardRepairBegin(Device*,const Process*,bool writable);
Vnode* GuardRepairOpenLinux(uint64_t flags,int* error);
// Revokes access. A busy result permanently prevents the next phase.
bool GuardRepairEnd();
void GuardRepairAbort();
// After revocation, both open descriptions and admitted I/O must drain before
// changing the underlying adapter's phase.
bool GuardRepairQuiescent();
}
