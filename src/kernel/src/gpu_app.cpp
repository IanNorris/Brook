// gpu_app.cpp — userspace GPU (virgl) access registration.
//
// Holds the pointer to the active app-GPU ops (registered by a 3D display
// driver). The syscall layer queries GpuAppGet() to give apps a private virgl
// context. See gpu_app.h for the contract.

#include "gpu_app.h"
#include "serial.h"

namespace brook {

static const GpuAppOps* g_gpuApp = nullptr;

extern "C" void GpuAppRegister(const GpuAppOps* ops)
{
    if (!ops) return;
    g_gpuApp = ops;
    SerialPrintf("GPU-APP: registered '%s'\n", ops->name ? ops->name : "?");
}

const GpuAppOps* GpuAppGet()
{
    return g_gpuApp;
}

} // namespace brook
