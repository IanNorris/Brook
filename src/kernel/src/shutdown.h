#pragma once
#include <stdint.h>
namespace brook {
// One coordinator for the desktop and opted-in VM lifecycle channel.
void ShutdownConfigure(); // boot-only fw_cfg read
void ShutdownStart();
// Request/result identities are retained until reboot. Caller binds the boot.
void ShutdownCommand(const char* id,const char* verb,char state[24],int* error);
void ShutdownDesktopRequest(bool restart);
void ShutdownDesktopCancel();
const char* ShutdownDesktopStatus();
void ShutdownCompositorPoll();
bool ShutdownLaunchAllowed();
}
