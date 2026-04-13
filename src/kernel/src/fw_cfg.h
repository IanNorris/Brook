#pragma once

#include <stdint.h>

namespace brook {

// Initialise the QEMU fw_cfg interface. Returns false if not present.
bool FwCfgInit();

// Read a named fw_cfg file into a buffer. Returns bytes read, or 0 if not found.
// The name should match what QEMU was passed, e.g. "opt/mykey".
uint32_t FwCfgReadFile(const char* name, void* buf, uint32_t bufSize);

// Read raw bytes from a fw_cfg selector key.
void FwCfgReadRaw(uint16_t key, void* buf, uint32_t size);

// Check if fw_cfg is available.
bool FwCfgAvailable();

} // namespace brook
