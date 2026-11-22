#pragma once
#include <stdint.h>
#include "../../shared/brook_ahci_export.h"
namespace brook {
struct Device;
struct TrimRange;
struct AhciReadOnlyStatus {
    bool requested=false, ready=false, quarantined=false;
    const char* reason="not requested";
    uint32_t port=0, logicalSectorBytes=0, physicalSectorBytes=0;
    uint64_t sectors=0;
    char model[41]{}, serial[21]{};
};
// Read-only snapshot of the selected port's MMIO registers.
struct AhciPortRegisters {
    bool available=false;
    uint32_t cmd=0, sctl=0, ssts=0, serr=0;
};
// Runtime completion-wait statistics (relaxed counters). irqMode reports
// whether completion interrupts are currently in use.
struct AhciIoStats {
    uint64_t waits=0, spinCompletions=0, sleepingWaits=0, sleeps=0, yields=0;
    uint64_t irqWakes=0, irqTimeouts=0, irqSkipped=0, irqDisabled=0, interrupts=0;
    uint64_t waitNs=0, maxWaitNs=0;
    // Interrupted sleeps only: block -> ISR wake, and ISR wake -> waiter running.
    uint64_t irqLatencyNs=0, wakeLatencyNs=0, maxWakeLatencyNs=0;
};
struct AhciIrqStatus { bool configured=false, active=false; uint8_t vector=0, apic=0; const char* reason="not configured"; };
void AhciReadOnlyInit();
AhciIoStats AhciReadOnlyIoStats();
AhciIrqStatus AhciReadOnlyIrqStatus();
// Runtime spin budget before an interrupt sleep, 0..2000 us (diagnostic).
bool AhciReadOnlySetSpinUs(uint64_t us);
uint64_t AhciReadOnlySpinUs();
AhciReadOnlyStatus AhciReadOnlyGetStatus(AhciPortRegisters* registers=nullptr);
// Boot-only, borrowed device pointer: available only for an identity-admitted,
// ready writable disk. The registered device remains pinned for the boot.
Device* AhciWriteAdmittedDevice();
// Internal guarded pre-mount path only. The caller must own the boot write
// lease, validate filesystem exclusions and keep the allocator unpublished.
uint16_t AhciTrimPayloadLimit(Device* parent);
struct AhciTrimStats {
    uint32_t commands = 0;
    uint64_t commandTotalNs = 0, commandMaxNs = 0;
};
int AhciTrimBatch(Device* parent, TrimRange partition, const TrimRange* ranges,
                  uint32_t count, bool submit, AhciTrimStats& stats);

#if defined(BROOK_SSD_INSTALLER)
Device* AhciPhysicalInstallDevice();
Device* AhciPhysicalBlockToolsDevice();
int AhciPhysicalBlockToolsFinalSync();
#endif
bool AhciReadOnlyExportInfo(BrookAhciExportInfo& info);
int AhciReadOnlyExportRead(uint64_t offset,void* output,uint32_t length);
// Internal diagnostic read: exactly one 512-byte sector. Write admission is a
// separate boot-time identity policy; this diagnostic never writes.
int AhciReadOnlySector(uint64_t lba, void* output);
#if defined(BROOK_AHCI_PANIC_TEST) || defined(BROOK_SSD_GUARD_PENDING_TEST) || defined(BROOK_SSD_AUTO_RECOVERY)
// Disposable test disk only; retains ownership even if capture fails.
void AhciPanicTestCapture();
void AhciPanicReserveBeforeHalt();
#if defined(BROOK_AHCI_PANIC_CONTENTION_TEST)
void AhciPanicTestWaitForWriter();
#endif
bool AhciPanicTestFatCandidate();
bool AhciPanicTestMapSectors(const uint64_t* sectors,uint32_t count,uint64_t partitionStart,uint64_t partitionSectors);
#if defined(BROOK_SSD_GUARD_PENDING_TEST)
bool AhciGuardPanicTestMapSectors(const uint64_t* sectors,uint32_t count,uint64_t partitionStart,uint64_t partitionSectors);
#endif
#if defined(BROOK_SSD_AUTO_RECOVERY)
bool AhciGuardPanicRecoveryMapSectors(const uint64_t* sectors,uint32_t count,uint64_t partitionStart,uint64_t partitionSectors);
#endif
#endif
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
int AhciDisposableQemuTest();
bool AhciDisposableQemuWritable();
bool AhciDisposableLargeDataWritable();
Device* AhciDisposableQemuInstallDevice();
Device* AhciDisposableQemuBlockToolsDevice();
#endif
} // namespace brook
