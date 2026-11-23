#pragma once
#include "writable_desktop_plan.h"
namespace brook {
enum class WritableDesktopBoot { Absent,Fallback,Ready,Maintenance };
enum class WritableDesktopSource { Usb,System };
struct Device;
#if defined(BROOK_SSD_AUTO_RECOVERY)
bool WritableDesktopVerifyRepairedProfile(Device* device);
#endif
// Direct stores use the same namespace and payload checks as image stores.
struct WritableDesktopFailure {
    const char* path=nullptr; // Fixed diagnostic label, never contents from disk.
    const char* check=nullptr;
    int status=0;
};
bool WritableDesktopPrepareDirect(WritableDesktopFailure* failure=nullptr);
enum class WritableDesktopPublish { Ready, SharedProfileRejected, HomeBindFailed };
WritableDesktopPublish WritableDesktopPublishDirect(WritableDesktopFailure* failure=nullptr);
void WritableDesktopMarkDirectReady(uint8_t drive,uint64_t bytes);
void WritableDesktopDirectFinalResult(int error,bool attempted);
struct WritableDesktopInfo {
    const char* stage="disabled";
    bool configured=false,ready=false,pinned=false,dirty=false,clean=false,stateUncertain=false;
    int error=0,finalError=0;
    uint8_t drive=0;
    uint64_t bytes=0;
};
// Serialized boot only. System must be passed only after a separately verified
// system volume is mounted; this never falls back between the two sources.
WritableDesktopBoot WritableDesktopBootMount(uint8_t* nextDrive,WritableDesktopSource source=WritableDesktopSource::Usb);
// Live stick: a verified RAM image (ext2) mounted read-write at /nix with the
// usual payload checks and /data/home bind; RAM only, nothing to finalize.
WritableDesktopBoot WritableDesktopLiveMount(uint8_t* nextDrive,Device* ram,uint64_t bytes);
WritableDesktopInfo WritableDesktopRead();
// Shutdown worker only, after successful global VFS sync with write admission
// closed/drained and still owned by this thread. Failed admission is never armed.
int WritableDesktopFinalSync();
}
