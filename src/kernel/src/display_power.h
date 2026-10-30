#pragma once
#include "intel_backlight.h"
namespace brook {
struct BootProtocol;
struct DisplayPowerStatus {
    bool mapped=false,available=false,on=true,uncertain=false;
    uint32_t idleSeconds=0;
    uint64_t lastActivityMs=0,restoreAtMs=0;
    const char* reason="No supported backlight device";
    BacklightRegisters original{},current{};
};
// Boot-only, after the PCI inventory and before desktop/input processing.
// Reads a narrowly identified Dell/Broadwell device; never writes PWM at boot.
void DisplayPowerConfigure(const BootProtocol* boot);
DisplayPowerStatus DisplayPowerRead();
bool DisplayPowerSetOn(bool on,uint32_t restoreSeconds=0);
bool DisplayPowerSetIdle(uint32_t seconds);
void DisplayPowerTick(uint64_t now,uint64_t inputSequence);
void DisplayPowerPanicRestore();
}
