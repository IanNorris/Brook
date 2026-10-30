#pragma once
#include "acpi_tables.h"
namespace brook {
struct PowerIo {
    void* context;
    bool (*write8)(void*,uint16_t,uint8_t);
    bool (*write16)(void*,uint16_t,uint16_t);
};
struct PlatformPowerPlan {
    AcpiResetInfo firmwareReset{};
    bool q35Off=false,q35Reset=false,resetDisabledByLaunch=false;
    bool CanReset() const {return !resetDisabledByLaunch && (firmwareReset.available || q35Reset);}
    bool CanPowerOff() const {return q35Off;}
    const char* ResetMethod() const {return resetDisabledByLaunch?"disabled":firmwareReset.available?"acpi-io8":q35Reset?"q35-io8":"unavailable";}
    const char* PowerOffMethod() const {return q35Off?"q35-io16":"manual";}
    const char* ResetReason() const {return resetDisabledByLaunch?"Reset disabled by stop-only launch policy":CanReset()?ResetMethod():firmwareReset.reason;}
    const char* PowerOffReason() const {return q35Off?"Explicit Q35 power-off opt-in":"ACPI S5 transition is not implemented";}
    // Select once. In particular, failure of the advertised register never
    // triggers a speculative alternative reset port or register width.
    bool Issue(bool restart,const PowerIo& io) const {
        if(restart) {
            if(resetDisabledByLaunch)return false;
            if(firmwareReset.available)
                return io.write8 && io.write8(io.context,firmwareReset.port,firmwareReset.value);
            if(q35Reset)return io.write8 && io.write8(io.context,0xcf9,6);
        } else if(q35Off)return io.write16 && io.write16(io.context,0x604,0x2000);
        return false;
    }
};
void PlatformPowerConfigure(); // boot-time discovery and explicit emulator opt-in
const PlatformPowerPlan& PlatformPowerGetPlan();
// True means the selected write was issued, not proof that hardware reset/off
// succeeded. The coordinator stays quiescent and reports a returned transition.
bool PlatformPowerIssue(bool restart);
}
