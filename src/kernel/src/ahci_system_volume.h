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
bool AhciSystemVolumeOwnsMember(Vnode* retainedDirectory);
#if defined(BROOK_AHCI_PANIC_TEST)
void AhciPanicTestPrepareFat();
#endif
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
#if defined(BROOK_SSD_GUARD_TRANSACTION)
enum class AhciGuardDataState { Invalid, Clean, NeedsCheck };
// Read the identity-bound data superblock while the boot reservation holds.
// This classification grants no repair or normal-boot authority by itself.
AhciGuardDataState AhciSystemGuardDataState(Device* reserved);
// The separate adapter advertises stable writable capability but denies
// writes until the one-shot, checker-authorized transaction.
Device* AhciSystemGuardTestPrepare(Device* reserved);
bool AhciSystemGuardTestBeginWrites(Device* adapter);
#if defined(BROOK_SSD_AUTO_RECOVERY)
bool AhciSystemGuardBeginAttempt(Device* adapter);
#endif
#if defined(BROOK_SSD_GUARD_PENDING_TEST)
bool AhciSystemGuardPanicTestArm(Device* adapter,const uint64_t* sectors,uint32_t count);
bool AhciSystemGuardTestAdoptSystemMount(Device* adapter,uint8_t drive,uint64_t publication);
// Test-only: caller must already have checked both guards and published the
// checked ESP. Retains the guard-owned data lease and never retries failure.
bool AhciSystemGuardTestMountDesktop(Device* adapter,uint8_t* nextDrive);
int AhciSystemGuardTestFailedDesktopWrite();
#endif
#if defined(BROOK_SSD_AUTO_RECOVERY)
bool AhciSystemGuardPanicRecoveryArm(Device* adapter,const uint64_t* sectors,uint32_t count);
#endif
void AhciSystemGuardTestFinish(Device* adapter);
Device* AhciSystemGuardTestDataArm(Device* adapter,int pendingOutcome);
bool AhciSystemGuardTestDataVerify(Device* adapter);
bool AhciSystemGuardTestDataIdentityValid(Device* adapter);
void AhciSystemGuardTestDataFinish(Device* adapter);
#if defined(BROOK_SSD_GUARD_COMPLETION)
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
