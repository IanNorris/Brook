#pragma once
#include <stdint.h>
namespace brook {
struct Device;
struct AhciReadOnlyStatus {
    bool requested=false, ready=false, quarantined=false;
    const char* reason="not requested";
    uint32_t port=0, logicalSectorBytes=0, physicalSectorBytes=0;
    uint64_t sectors=0;
    char model[41]{}, serial[21]{};
};
void AhciReadOnlyInit();
AhciReadOnlyStatus AhciReadOnlyGetStatus();
// Boot-only, borrowed device pointer: available only for an identity-admitted,
// ready writable disk. The registered device remains pinned for the boot.
Device* AhciWriteAdmittedDevice();
// Internal diagnostic read: exactly one 512-byte sector. Write admission is a
// separate boot-time identity policy; this diagnostic never writes.
int AhciReadOnlySector(uint64_t lba, void* output);
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
int AhciDisposableQemuTest();
bool AhciDisposableQemuWritable();
Device* AhciDisposableQemuInstallDevice();
#endif
} // namespace brook
