#include "usb_boot_health.h"
namespace brook {
namespace { uint32_t state=0; UsbBootResetReport saved{}; }
extern "C" void UsbBootReportReset(uint16_t vendor, uint16_t device,
    uint32_t phase, uint32_t error, uint32_t command, uint32_t status,
    uint32_t statusNow, bool statusNowValid, uint64_t elapsedMs) {
    uint32_t expected=0;
    if (!__atomic_compare_exchange_n(&state,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) return;
    saved={vendor,device,phase,error,command,status,statusNow,elapsedMs,statusNowValid};
    __atomic_store_n(&state,2,__ATOMIC_RELEASE);
}
bool UsbBootReadReset(UsbBootResetReport& report) {
    if (__atomic_load_n(&state,__ATOMIC_ACQUIRE)!=2) { report={};return false; }
    report=saved;return true;
}
}
