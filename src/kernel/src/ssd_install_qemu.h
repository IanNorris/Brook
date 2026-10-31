#pragma once
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
#include <stdint.h>
namespace brook {
struct SsdQemuStatus {uint32_t state,stage;uint64_t operations;int error;bool paused;};
bool SsdQemuInstallBegin();
SsdQemuStatus SsdQemuInstallStatus();
bool SsdQemuInstallResume();
// Shutdown closes admission and waits for the whole worker to finish.
bool SsdQemuInstallCloseReady();
}
#endif
