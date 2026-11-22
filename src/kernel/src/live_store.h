#pragma once
#include <stdint.h>
namespace brook {
// Desktop live stick: if /etc/live-store exists, copy the named raw partition
// of the boot stick into RAM, verify its SHA-256 and mount it read-write at
// /nix (RAM only; nothing is written to the stick). Returns true when the
// desktop store is mounted. Any failure leaves the boot unchanged (logged).
bool LiveStoreBoot(uint8_t* nextDrive);
}
