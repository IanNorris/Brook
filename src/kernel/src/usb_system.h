#pragma once

#include <stdint.h>

namespace brook {
struct Device;

extern "C" bool UsbSystemRequested();
void UsbSystemConfigure();
bool UsbSystemBoot(uint8_t* nextDrive,bool scanComplete);
bool UsbSystemAllowsBootUpdates(Device* disk,uint64_t start,uint64_t length);
int UsbSystemFinalSync(int syncError);

struct UsbSystemStatus {
    const char* stage="absent";
    bool configured=false, parsed=false, window=false, mounted=false;
    bool dirty=false, clean=false, uncertain=false, finalAttempted=false;
    int disk=-1, error=0, finalError=0;
    uint64_t firstLba=0, sectors=0, reads=0, writes=0, flushes=0, errors=0;
    char partitionGuid[33]{}, fsUuid[33]{};
};
UsbSystemStatus UsbSystemRead();
}
