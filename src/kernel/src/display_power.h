#pragma once
#include "intel_backlight.h"
namespace brook {
struct BootProtocol;
struct DisplayPowerStatus {
    bool mapped=false,available=false,on=true,uncertain=false;
    // Last idle-inhibitor poll (at most once a second from the compositor tick).
    bool inhibited=false;
    uint64_t inhibitCheckedMs=0;
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
// Polls WmIdleInhibitActive() at most once a second; call without the WM lock.
void DisplayPowerTick(uint64_t now,uint64_t inputSequence);
void DisplayPowerPanicRestore();
}
