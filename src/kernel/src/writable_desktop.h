#pragma once
#include "writable_desktop_plan.h"
namespace brook {
enum class WritableDesktopBoot { Absent,Fallback,Ready,Maintenance };
struct WritableDesktopInfo {
    const char* stage="disabled";
    bool configured=false,ready=false,pinned=false,dirty=false,clean=false,stateUncertain=false;
    int error=0,finalError=0;
    uint8_t drive=0;
    uint64_t bytes=0;
};
// Serialized boot only, after USB mounting and before any application mount.
WritableDesktopBoot WritableDesktopBootMount(uint8_t* nextDrive);
WritableDesktopInfo WritableDesktopRead();
// Shutdown worker only, after successful global VFS sync with write admission
// closed/drained and still owned by this thread. Failed admission is never armed.
int WritableDesktopFinalSync();
}
