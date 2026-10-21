#include "gpu_completion.h"
#include "spinlock.h"
#include "process.h"
#include "posix_timer.h"
#include "scheduler.h"
#include "kvmclock.h"
#include "apic.h"
#include "idt.h"
#include "module.h"
#include "fw_cfg.h"
namespace brook {
#include "gpu_completion_context.inc"
#include "gpu_completion.inc"
extern "C" bool GpuRecoveryBootOption(uint32_t option) {
    const char* name=option==1?"opt/gpu-late-vertex-recovery":
        option==2?"opt/gpu-inject-vertex-timeout-once":nullptr;
    if(!name)return false;
    char value[2]{};
    return FwCfgReadFile(name,value,sizeof(value))==1 && value[0]=='1';
}
}
