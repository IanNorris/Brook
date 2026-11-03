#pragma once
#include <stdint.h>
namespace brook {
struct Device;
enum class AhciSystemBoot { Absent, Ready, Maintenance };
// An explicit direct-store selector always requires direct admission; failures
// are maintenance-only rather than selecting a different desktop store.
bool AhciSystemDirectRequested();
bool AhciSystemDirectDesktopMount(uint8_t* nextDrive);
int AhciSystemDirectFinalSync();
// Boot only. An existing selector which fails at any stage is Maintenance.
AhciSystemBoot AhciSystemVolumeMount(uint8_t* nextDrive);
// Boot-only experimental preparation. Reserves the ESP for the returned
// adapter and denies data/GPT writes. Does not mount or expose filesystems.
// Only call before any filesystem, caches or user processes can access disk.
Device* AhciSystemGuardPrepare();
// Until a checker-completion gate exists, this adapter is never eligible for
// a writable private FAT mount.
bool AhciSystemGuardIsReservedDevice(Device* device);
// Optional, read-only boot diagnostic. Does not mount or change the disk.
void AhciSystemDataInspect();
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
// Disposable, identity-gated QEMU fixture only; never part of production boot.
bool AhciSystemQemuLargeDataMount(uint8_t* nextDrive);
bool AhciSystemQemuLargeDesktopRequested();
bool AhciSystemQemuLargeDesktopReady();
int AhciSystemQemuLargeDataFinalSync();
#endif
}
