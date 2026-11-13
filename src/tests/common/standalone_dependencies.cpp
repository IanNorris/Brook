// Standalone test ELFs do not load modules or initialize physical devices.
// Their callbacks have static lifetime; hardware observations fail closed.
#include "acpi.h"
#include "cpu.h"
#include "module.h"
#include "intel_bdw_scanout.h"

extern "C" uint8_t __text_start[], __kernel_end[];

namespace brook {
// Standalone memory tests have no GPU owner or hardware execution backend.
bool BdwRunRetainedBcs(BdwBcsExecutionBackend&,uint64_t,BdwBcsRunResult&) {return false;}
bool BdwRunRetainedRender(BdwRenderExecutionBackend&,uint64_t,BdwRenderRunResult&) {return false;}
bool BdwRetireRetainedRenderAfterReset(BdwRenderExecutionBackend&) {return false;}
bool BdwRunRebasedRender(BdwRenderExecutionBackend&,uint64_t,BdwRenderRunResult&) {return false;}
bool BdwCopyRetainedRenderContext(BdwRenderExecutionBackend&,uint32_t (&)[20*1024]) {return false;}
bool BdwCopyRebasedRenderContext(BdwRenderExecutionBackend&,uint32_t (&)[20*1024]) {return false;}
bool ModuleRetainAddresses(const uintptr_t* addresses, uint32_t count)
{
    if (!addresses || !count) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (addresses[i] < reinterpret_cast<uintptr_t>(__text_start) ||
            addresses[i] >= reinterpret_cast<uintptr_t>(__kernel_end)) return false;
    return true;
}

bool AcpiClassifyBootRange(uint64_t, uint32_t, BootResourceClassification& result)
{
    result = {};
    return false;
}
}

extern "C" bool CpuWriteCombiningReady() { return false; }
bool CpuPrepareFramebufferWc(uint64_t, uint64_t) { return false; }
bool CpuObserveMemoryTypes(uint64_t, uint64_t, brook::CpuMemoryTypeObservation& out)
{
    out = {};
    return false;
}
