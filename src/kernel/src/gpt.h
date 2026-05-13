#pragma once

#include <stdint.h>
#include "device.h"

// GPT (GUID Partition Table) scanner.
//
// GptProbeDevice() reads the GPT header and partition entries from a block
// device.  For each valid partition it registers a sub-device named
// "<parent>p<N>" (e.g. "usb0p1", "usb0p2") that reads/writes are
// transparently offset to the correct LBA range on the parent device.
//
// Returns the number of partitions registered, or 0 if no GPT found.

namespace brook {

// Well-known partition type GUIDs (mixed-endian, as stored on disk).
struct GptGuid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
};

static constexpr GptGuid GPT_TYPE_EFI_SYSTEM = {
    0xC12A7328, 0xF81F, 0x11D2, {0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}
};

static constexpr GptGuid GPT_TYPE_LINUX_FS = {
    0x0FC63DAF, 0x8483, 0x4772, {0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4}
};

// Maximum partitions we'll scan (GPT can have 128, but we cap for memory).
static constexpr uint32_t GPT_MAX_PARTITIONS = 32;

// Probe a block device for GPT and register partition sub-devices.
// Returns number of partitions registered.
uint32_t GptProbeDevice(Device* parentDev);

} // namespace brook
