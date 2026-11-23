// display_output.cpp — the kernel's output registry (display_output.h).

#include "display_output.h"
#include "spinlock.h"

namespace brook {

namespace {
// Constant-initialised: no constructor runs before the registry is used.
OutputRegistry g_outputs;
SpinLock g_outputRegisterLock;
}

bool OutputRegisterBackend(const OutputBackendOps* ops)
{
    SpinLockAcquire(&g_outputRegisterLock);
    const bool ok = g_outputs.Register(ops);
    SpinLockRelease(&g_outputRegisterLock);
    return ok;
}

bool OutputUnregisterBackend(const OutputBackendOps* ops)
{
    SpinLockAcquire(&g_outputRegisterLock);
    const bool ok = g_outputs.Unregister(ops);
    SpinLockRelease(&g_outputRegisterLock);
    return ok;
}

void OutputNotifyChanged() { g_outputs.NotifyChanged(); }
uint64_t OutputGeneration() { return g_outputs.Generation(); }
void OutputEnumerate(OutputLayout& out) { g_outputs.Enumerate(out); }

} // namespace brook
