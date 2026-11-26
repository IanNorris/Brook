#pragma once

#include "device.h"

namespace brook {
struct Device;

extern "C" bool UsbSystemRequested();
void UsbSystemConfigure();
bool UsbSystemBoot(uint8_t* nextDrive,bool scanComplete);
bool UsbSystemAllowsBootUpdates(Device* disk,uint64_t start,uint64_t length);
int UsbSystemFinalSync(int syncError);
// A not-clean BROOK_SYSTEM with the pinned checker available leaves boot
// pending: start the repair thread instead of the boot script. It runs
// e2fsck -p -f, then mounts and runs /boot/INIT.RC, reboots, or keeps the
// refusal and starts the kernel console.
bool UsbSystemRepairPending();
bool UsbSystemStartRepair();
// kernel.cpp: home preparation, repeated once the persistent home is bound.
void UsbSystemPrepareDesktopHome();

struct UsbSystemStatus {
    const char* stage="absent";
    // none, disabled, unavailable, pending, running, repaired, reboot, refused
    const char* fsck="none";
    bool configured=false, parsed=false, window=false, mounted=false;
    bool profileTransferReady=false;
    bool dirty=false, clean=false, uncertain=false, finalAttempted=false;
    int disk=-1, error=0, finalError=0, fsckExit=-1;
    uint64_t firstLba=0, sectors=0, reads=0, writes=0, flushes=0, errors=0;
    char partitionGuid[33]{}, fsUuid[33]{};
};
UsbSystemStatus UsbSystemRead();
bool UsbSystemTiming(UsbStorageTiming* out);
int UsbSystemBenchRead(uint64_t at,void* data,uint32_t bytes);
}
