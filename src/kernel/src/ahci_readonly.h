#pragma once
#include <stdint.h>
namespace brook {
struct AhciReadOnlyStatus {
    bool requested=false, ready=false, quarantined=false;
    const char* reason="not requested";
    uint32_t port=0, logicalSectorBytes=0, physicalSectorBytes=0;
    uint64_t sectors=0;
    char model[41]{};
};
void AhciReadOnlyInit();
AhciReadOnlyStatus AhciReadOnlyGetStatus();
// Internal diagnostic read: exactly one 512-byte sector. No writes exist.
int AhciReadOnlySector(uint64_t lba, void* output);
} // namespace brook
