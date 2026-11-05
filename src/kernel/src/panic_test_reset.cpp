#include "panic_test_reset.h"
#include "panic_reset_timer.h"
#include "platform_power.h"
#include "acpi.h"
#include "fw_cfg.h"
#include "portio.h"
#include "serial.h"
#include "string.h"

namespace brook {
namespace {
bool enabled = false;
bool headless = false;
#if defined(BROOK_SSD_AUTO_RECOVERY)
bool recoveryArmed = false;
#endif
void PanicRaw(const char* text) {
    // Exception handling intentionally suppresses normal SerialPuts.
    while (*text) SerialPanicPutChar(*text++);
}
struct Pit {
    uint8_t readGate() { return inb(0x61); }
    void writeGate(uint8_t value) { outb(0x61,value); }
    void writeControl(uint8_t value) { outb(0x43,value); }
    void writeCounter(uint8_t value) { outb(0x42,value); }
    void pause() { __asm__ volatile("pause"); }
    void issueReset() { (void)PlatformPowerIssue(true); }
};
struct PmTimer {
    uint16_t port;
    uint32_t readTimer() { return inl(port); }
    void pause() { __asm__ volatile("pause":::"memory"); }
    void issueReset() { (void)PlatformPowerIssue(true); }
};
}
// Kept separate so failure of the PIT path can be exercised without altering
// the firmware-timer fallback or the panic-time reset policy.
__attribute__((noinline)) PanicResetResult PanicPitDelay(uint32_t intervals) {
    Pit pit;return PanicResetAfterIntervals(pit,intervals,10000000);
}
__attribute__((noinline)) PanicResetResult PanicPmDelay(uint32_t intervals,uint16_t port) {
    PmTimer timer{port};return PanicResetAfterPmIntervals(timer,intervals,10000000);
}
void PanicTestResetConfigure() {
    // Both settings come only from QEMU fw_cfg, read before scheduling starts.
    // The ordinary physical firmware reset register cannot enable this path.
    char request[16]{};
    const auto size=FwCfgReadFile("opt/brook/panic-test-reset",request,sizeof(request));
    const auto& power=PlatformPowerGetPlan();
    enabled=size==7 && !memcmp(request,"test-1s",7) && power.q35Reset &&
            !power.resetDisabledByLaunch;
    char headlessRequest[8]{};
    const auto headlessSize=FwCfgReadFile("opt/brook/panic-test-headless",headlessRequest,sizeof(headlessRequest));
    headless=enabled && headlessSize==3 && !memcmp(headlessRequest,"yes",3);
    if(enabled) SerialPuts("PANIC_TEST_RESET_ARMED 1s disposable QEMU only\n");
}
void PanicTestResetAfterDump() {
    bool recovery=false;
#if defined(BROOK_SSD_AUTO_RECOVERY)
    recovery=__atomic_load_n(&recoveryArmed,__ATOMIC_ACQUIRE);
#endif
    if(!enabled && !recovery)return;
    PanicRaw(enabled?"PANIC_TEST_RESET_WAIT\n":"PANIC_RECOVERY_RESET_WAIT\n");
    auto result=PanicPitDelay(enabled?20:100);
    if(result==PanicResetResult::TimerFailed && AcpiGetPmTimerInfo().available) {
        PanicRaw("PANIC_RESET_PM_FALLBACK\n");
        result=PanicPmDelay(enabled?20:100,AcpiGetPmTimerInfo().port);
    }
    if(result==PanicResetResult::TimerFailed) {
        PanicRaw(enabled?"PANIC_TEST_RESET_TIMER_FAILED\n":"PANIC_RECOVERY_RESET_TIMER_FAILED\n");
#if defined(BROOK_SSD_AUTO_RECOVERY)
        // Crash capture has been attempted and recovery has explicitly armed reset.
        // The bounded polling loops failed, so no clock remains to enforce the
        // display delay. Attempt the authorized reset rather than halt forever.
        if(recovery && !enabled) {
            PanicRaw("PANIC_RECOVERY_RESET_TIMERLESS_ATTEMPT\n");
            (void)PlatformPowerIssue(true);
            PanicRaw("PANIC_RECOVERY_RESET_RETURNED\n");
        }
#endif
    }
    else if(result==PanicResetResult::ResetReturned) PanicRaw(enabled?"PANIC_TEST_RESET_RETURNED\n":"PANIC_RECOVERY_RESET_RETURNED\n");
}
#if defined(BROOK_SSD_AUTO_RECOVERY)
bool PanicRecoveryResetArm() {
    if(!PlatformPowerGetPlan().CanReset())return false;
    __atomic_store_n(&recoveryArmed,true,__ATOMIC_RELEASE);return true;
}
bool PanicRecoveryResetArmed() {return __atomic_load_n(&recoveryArmed,__ATOMIC_ACQUIRE);}
#endif
bool PanicTestResetEnabled() {return enabled;}
bool PanicTestHeadlessEnabled() {return headless;}
}
