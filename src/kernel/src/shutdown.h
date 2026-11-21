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
enum class AcPowerSource : uint8_t { Unknown, Online, Offline };
struct ShutdownAcStatus {
    AcPowerSource source;
    bool enabled, ready, requested;
    uint64_t sampleAgeMs;
};
// The platform AC driver publishes a new sample once per second. Reports
// must come from the kernel and be independent of USB/network availability.
void ShutdownReportAcSource(AcPowerSource source);
ShutdownAcStatus ShutdownGetAcStatus();
// Request/result identities are retained until reboot. Caller binds the boot.
void ShutdownCommand(const char* id,const char* verb,char state[24],int* error);
void ShutdownDesktopRequest(bool restart);
void ShutdownDesktopCancel();
void ShutdownDesktopForceApps();
// Explicit escalation of already-prompted, still-open app windows.
bool ShutdownCanForceApps(const char* requestId=nullptr);
const char* ShutdownDesktopStatus();
// True after the current shutdown is clean and the machine has no soft-off method.
bool ShutdownReadyForManualPowerOff();
const char* ShutdownRequestStatus(const char* requestId);
// Processes still holding the current request in closing/services, for the
// bridge reply. A request that is not current reports count 0 and its stage.
struct ShutdownBlocker { uint16_t pid; bool window, service; char name[32]; };
struct ShutdownBlockerReport {
    char stage[16];            // windows|waiting|grace|sigterm|sigkill|services|cleanup|none
    uint32_t count;            // total blocking processes (entries holds the first few)
    uint32_t listed;
    uint64_t nextSignalMs;     // until the next SIGTERM/SIGKILL (0: none pending)
    ShutdownBlocker entries[4];
    char status[192];          // same text as ShutdownRequestStatus, copied under lock
};
void ShutdownRequestBlockers(const char* requestId, ShutdownBlockerReport* out);
void ShutdownCompositorPoll();
bool ShutdownLaunchAllowed();
}
