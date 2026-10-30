#pragma once
#include <stdint.h>
namespace brook {
// One coordinator for the desktop and opted-in VM lifecycle channel.
void ShutdownConfigure(); // boot-only validated firmware + explicit fw_cfg opt-in
struct ShutdownPowerCapabilities {
    bool gracefulQuiescence,reset,powerOff;
    const char *resetMethod,*powerOffMethod,*resetReason,*powerOffReason;
};
ShutdownPowerCapabilities ShutdownGetPowerCapabilities();
void ShutdownStart();
// Request/result identities are retained until reboot. Caller binds the boot.
void ShutdownCommand(const char* id,const char* verb,char state[24],int* error);
void ShutdownDesktopRequest(bool restart);
void ShutdownDesktopCancel();
const char* ShutdownDesktopStatus();
const char* ShutdownRequestStatus(const char* requestId);
void ShutdownCompositorPoll();
bool ShutdownLaunchAllowed();
}
