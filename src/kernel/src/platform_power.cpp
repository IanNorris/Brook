#include "platform_power.h"
#include "acpi.h"
#include "fw_cfg.h"
#include "portio.h"
#include "serial.h"
#ifdef BROOK_HOST_TEST
#include <cstring>
#else
#include "string.h"
#endif
namespace brook {
namespace {
PlatformPowerPlan plan;
bool Write8(void*,uint16_t port,uint8_t value){outb(port,value);return true;}
bool Write16(void*,uint16_t port,uint16_t value){outw(port,value);return true;}
}
void PlatformPowerConfigure() {
    plan={};plan.firmwareReset=AcpiGetResetInfo();
    char mode[24]{};const auto bytes=FwCfgReadFile("opt/brook/power",mode,sizeof(mode)-1);
    if(bytes==9 && !memcmp(mode,"q35-reset",9)){plan.q35Off=true;plan.q35Reset=true;}
    else if(bytes==8 && !memcmp(mode,"q35-stop",8)){plan.q35Off=true;plan.resetDisabledByLaunch=true;}
    SerialPrintf("power: reset=%s poweroff=%s reason=%s\n",plan.ResetMethod(),plan.PowerOffMethod(),plan.PowerOffReason());
}
const PlatformPowerPlan& PlatformPowerGetPlan(){return plan;}
bool PlatformPowerIssue(bool restart){return plan.Issue(restart,{nullptr,Write8,Write16});}
}
