#include "panic_test_reset.h"
#include "panic_reset_timer.h"
#include "platform_power.h"
#include "fw_cfg.h"
#include "portio.h"
#include "serial.h"
#include "string.h"

namespace brook {
namespace {
bool enabled = false;
void PanicRaw(const char* text) {
    // Exception handling intentionally suppresses normal SerialPuts.
    while (*text) SerialPutChar(*text++);
}
struct Pit {
    uint8_t readGate() { return inb(0x61); }
    void writeGate(uint8_t value) { outb(0x61,value); }
    void writeControl(uint8_t value) { outb(0x43,value); }
    void writeCounter(uint8_t value) { outb(0x42,value); }
    void pause() { __asm__ volatile("pause"); }
    void issueReset() { (void)PlatformPowerIssue(true); }
};
}
void PanicTestResetConfigure() {
    // Both settings come only from QEMU fw_cfg, read before scheduling starts.
    // The ordinary physical firmware reset register cannot enable this path.
    char request[16]{};
    const auto size=FwCfgReadFile("opt/brook/panic-test-reset",request,sizeof(request));
    const auto& power=PlatformPowerGetPlan();
    enabled=size==7 && !memcmp(request,"test-1s",7) && power.q35Reset &&
            !power.resetDisabledByLaunch;
    if(enabled) SerialPuts("PANIC_TEST_RESET_ARMED 1s disposable QEMU only\n");
}
void PanicTestResetAfterDump() {
    if(!enabled)return;
    PanicRaw("PANIC_TEST_RESET_WAIT\n");
    Pit pit;
    const auto result=PanicResetAfterIntervals(pit,20,10000000);
    if(result==PanicResetResult::TimerFailed) PanicRaw("PANIC_TEST_RESET_TIMER_FAILED\n");
    else if(result==PanicResetResult::ResetReturned) PanicRaw("PANIC_TEST_RESET_RETURNED\n");
}
}
