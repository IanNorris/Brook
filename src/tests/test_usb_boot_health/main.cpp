#include "../../kernel/src/usb_boot_health.h"
#include "../../kernel/src/usb_boot_recovery.h"
#include <cassert>
using namespace brook;
int main() {
    UsbBootResetReport report{};
    assert(!UsbBootReadReset(report));
    UsbBootReportReset(0x8086,0x9cb1,8,2,2,0x11,0x11,true,10002);
    assert(UsbBootReadReset(report));
    assert(report.vendor==0x8086 && report.device==0x9cb1 && report.phase==8 && report.error==2);
    assert(report.command==2 && report.status==0x11 && report.statusNow==0x11 && report.statusNowValid && report.elapsedMs==10002);
    UsbBootRecoveryPolicy disabled{};assert(!disabled.ShouldShutdown(60000,true,true,report));
    UsbBootRecoveryPolicy timed{true,false};assert(!timed.ShouldShutdown(59999,true,true,report));
    assert(timed.ShouldShutdown(60000,true,true,report) && !timed.ShouldShutdown(60001,true,true,report));
    auto wrong=report;wrong.phase=0;UsbBootRecoveryPolicy other{true,false};assert(!other.ShouldShutdown(60000,true,true,wrong));
    UsbBootRecoveryPolicy noPower{true,false};assert(!noPower.ShouldShutdown(60000,false,true,report));
    UsbBootRecoveryPolicy missing{true,false};assert(!missing.ShouldShutdown(60000,true,false,report));
    wrong=report;wrong.statusNowValid=false;UsbBootRecoveryPolicy invalid{true,false};assert(!invalid.ShouldShutdown(60000,true,true,wrong));
    UsbBootReportReset(1,2,0,0,0,0,0,false,0);
    assert(UsbBootReadReset(report) && report.vendor==0x8086 && report.error==2);
}
