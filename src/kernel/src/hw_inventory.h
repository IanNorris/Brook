#pragma once
#include <stdint.h>

namespace brook {
struct BootProtocol;
// Opt-in (/etc/hw-inventory) read-only boot inventory: CPU, memory map, ACPI
// table headers, MCFG/DMAR, and every PCI function with BARs, bridge windows,
// capabilities and (via MCFG ECAM, read-only mappings) extended capabilities.
// Capture runs once on the BSP after the boot PCI scan, before APs are
// activated and before modules bind devices. No device register is written.
void HwInventoryCapture(const BootProtocol* boot);
// Console summary after network configuration: bridge endpoint, BRIDGE_V3_LOCAL
// lines and key devices. No-op unless the inventory was captured.
void HwInventoryAnnounce();
bool HwInventoryCaptured();
uint32_t HwInventoryBytes();
uint32_t HwInventoryLines();
bool HwInventoryOverflow();
// Copies up to count bytes at offset into out (NUL-terminated); returns bytes copied.
uint32_t HwInventoryRead(uint32_t offset,char* out,uint32_t count);
}
