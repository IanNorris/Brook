#pragma once

#include "device.h"
#include <stdint.h>

// virtio-blk block device driver (legacy PCI interface).
//
// Scans PCI for vendor=0x1AF4, device=0x1001 (virtio-blk legacy).
// Sets up virtqueue 0, registers as DEV_BLOCK devices named "virtio0", "virtio1", ...
//
// QEMU flags to expose disk images as virtio-blk:
//   -drive if=virtio,format=raw,file=<path/to/disk.img>
//
// Usage:
//   uint32_t n = VirtioBlkInitAll();  // call after VfsInit(); registers all found devices

namespace brook {

// Scan PCI, initialise ALL virtio-blk devices found, register each in the device
// registry as "virtio0", "virtio1", ...  Returns the number of devices registered.
extern "C" uint32_t VirtioBlkInitAll();

// Marker filename written to each disk image to identify its mount purpose.
// A disk containing this file at its root gets mounted at the path it specifies.
static constexpr const char* VIRTIO_ESP_MARKER = "BROOK.MNT";

// I/O statistics for procfs. Returns cumulative read/write ops and bytes.
void VirtioBlkGetStats(Device* dev, uint64_t& readOps, uint64_t& writeOps,
                       uint64_t& readBytes, uint64_t& writeBytes);

// Cold-read latency probe (BRO-165). Cumulative-since-boot counters per device;
// surfaced read-only via /proc/blkprobe. Take deltas across a workload.
struct VirtioBlkProbeStats {
    uint64_t waitCount;      // number of completion waits
    uint64_t reqSubmitted;   // total requests/slots awaited across those waits
    uint64_t waitNsTotal;    // cumulative ns spent in completion waits
    uint64_t waitNsMax;      // worst single wait (ns)
    uint64_t spinItersTotal; // cumulative spin iterations
    uint64_t pathLegacy;     // waits via SubmitRequest (legacy single-request)
    uint64_t pathBatch;      // waits via WaitAllSlots (small-read batched)
    uint64_t pathSG;         // waits via SubmitScatterGatherRead (zero-copy DMA)
};
void VirtioBlkGetProbe(Device* dev, VirtioBlkProbeStats& out);

} // namespace brook
