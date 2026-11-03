#pragma once
#include <stdint.h>
namespace brook {
struct Device;
struct Process;
struct Vnode;
struct ChildWaitSnapshot;
// Boot-owned, one-shot gate. Begin requires a caller-retained live checker
// process. This tracks access only: it does not authenticate the executable,
// observe completion, grant mount/write authority, or release disk leases.
bool GuardCheckerBegin(Device* device,const Process* checker);
// Tracked mode binds a root checker child to the current kernel controller.
// Poll obtains and consumes the child's scheduler wait event itself. A
// successful, fully drained completion may be claimed exactly once by the
// same controller; neither operation grants write or mount authority.
bool GuardCheckerBeginTracked(Device* device,const Process* checker);
int GuardCheckerPollTracked(Device* device,uint16_t pid,uint64_t incarnation,ChildWaitSnapshot* result);
bool GuardCheckerCompletionAvailable(Device* device);
bool GuardCheckerConsumeCompletion(Device* device);
Vnode* GuardCheckerOpen(int flags,int* error);
// Linux open flags for the check-only synthetic path. Only read-only opens
// and explicitly supported descriptor/status flags are admitted.
Vnode* GuardCheckerOpenLinux(uint64_t flags,int* error);
void GuardCheckerAbort();
struct GuardCheckerStatus {bool active;uint32_t views,reads;};
GuardCheckerStatus GuardCheckerGetStatus();
}
