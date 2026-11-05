#pragma once
#include <stdint.h>
namespace brook {
struct Device;
struct Vnode;
enum class AhciSystemBoot { Absent, Ready, Maintenance };
// An explicit direct-store selector always requires direct admission; failures
// are maintenance-only rather than selecting a different desktop store.
bool AhciSystemDirectRequested();
bool AhciSystemDirectDesktopMount(uint8_t* nextDrive);
int AhciSystemDirectFinalSync();
// Boot only. An existing selector which fails at any stage is Maintenance.
AhciSystemBoot AhciSystemVolumeMount(uint8_t* nextDrive);
// Boot-local identity of the admitted ESP attachment; zero until mounted.
uint64_t AhciSystemVolumePublication();
bool AhciSystemVolumeOwnsRoot(Vnode* retainedDirectory);
// Boot-only experimental preparation. Reserves the ESP for the returned
// adapter and denies data/GPT writes. Does not mount or expose filesystems.
// Only call before any filesystem, caches or user processes can access disk.
Device* AhciSystemGuardPrepare();
// Until a checker-completion gate exists, this adapter is never eligible for
// a writable private FAT mount.
bool AhciSystemGuardIsReservedDevice(Device* device);
// Expected disk GUID, data-partition GUID and verified primary UUID as
// lowercase hex. Available only after the reserved guard device is ready.
bool AhciSystemGuardIdentity(char (&out)[97]);
#if defined(BROOK_SSD_GUARD_PENDING_TEST)
// Disposable QEMU test build only. The separate adapter advertises stable
// writable capability but denies writes until the one-shot test transaction.
Device* AhciSystemGuardTestPrepare(Device* reserved);
bool AhciSystemGuardTestBeginWrites(Device* adapter);
void AhciSystemGuardTestFinish(Device* adapter);
Device* AhciSystemGuardTestDataArm(Device* adapter,int pendingOutcome);
bool AhciSystemGuardTestDataVerify(Device* adapter);
bool AhciSystemGuardTestDataIdentityValid(Device* adapter);
void AhciSystemGuardTestDataFinish(Device* adapter);
#if defined(BROOK_SSD_GUARD_COMPLETE_TEST)
bool AhciSystemGuardTestBeginCompletion(Device* adapter,bool verified);
#endif
bool AhciSystemGuardIsTestData(Device* adapter);
#endif
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
