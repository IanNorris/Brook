#pragma once
#include <stdint.h>
namespace brook {
struct Device;
struct Process;
struct Vnode;
// Boot-owned, one-shot gate. Begin requires a caller-retained live checker
// process. This tracks access only: it does not authenticate the executable,
// observe completion, grant mount/write authority, or release disk leases.
bool GuardCheckerBegin(Device* device,const Process* checker);
Vnode* GuardCheckerOpen(int flags,int* error);
void GuardCheckerAbort();
struct GuardCheckerStatus {bool active;uint32_t views,reads;};
GuardCheckerStatus GuardCheckerGetStatus();
}
