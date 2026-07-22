// gpu_compositor.cpp — GPU composition registration.
//
// Holds the pointer to the active GPU compositor ops (registered by a 3D
// display driver). The window compositor queries GpuCompositorGet() and, when
// non-null and enabled, composites window content on the GPU. See
// gpu_compositor.h for the contract.

#include "gpu_compositor.h"
#include "serial.h"

namespace brook {

static const GpuCompositorOps* g_gpuCompositor = nullptr;

extern "C" void GpuCompositorRegister(const GpuCompositorOps* ops)
{
    if (!ops) return;
    g_gpuCompositor = ops;
    SerialPrintf("GPU-COMPOSITOR: registered '%s'\n", ops->name ? ops->name : "?");
}

const GpuCompositorOps* GpuCompositorGet()
{
    return g_gpuCompositor;
}

} // namespace brook
