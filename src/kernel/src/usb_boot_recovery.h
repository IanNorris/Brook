#pragma once
#include "usb_boot_health.h"
#include "../../drivers/xhci/xhci_controller_reset.h"

namespace brook {
// One boot decision only. A missing/other report is not evidence that the
// specific Dell controller failure occurred.
struct UsbBootRecoveryPolicy {
    bool enabled=false, decided=false;
    bool ShouldShutdown(uint64_t bootMs, bool powerOffAvailable, bool reportAvailable,
                        const UsbBootResetReport& report) {
        if (!enabled || decided || bootMs<60000) return false;
        decided=true;
        return powerOffAvailable && reportAvailable && report.vendor==0x8086 && report.device==0x9cb1 &&
            report.phase==static_cast<uint32_t>(XhciResetPhase::ExistingReset) &&
            report.error==static_cast<uint32_t>(XhciResetError::Timeout) &&
            (report.command&2) && report.statusNowValid && report.elapsedMs>=10000;
    }
};
}
