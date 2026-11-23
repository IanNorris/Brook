#pragma once
#include <stdint.h>
namespace brook {
struct Device;
// Desktop live stick: if /etc/live-store exists, copy the named raw partition
// of the boot stick into RAM, verify its SHA-256 and mount it read-write at
// /nix (RAM only; nothing is written to the stick). Returns true when the
// desktop store is mounted. Any failure leaves the boot unchanged (logged).
bool LiveStoreBoot(uint8_t* nextDrive);
// True when /etc/live-store names this partition: the boot-time USB probe must
// not mount it directly (it is copied to RAM by LiveStoreBoot instead).
bool LiveStoreIsStorePartition(Device* partition);
}
