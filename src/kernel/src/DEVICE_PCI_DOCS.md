# Device, PCI & VirtIO Subsystem Documentation

## Overview

Brook's device subsystem provides a vtable-based driver interface for
block devices, PCI enumeration, VirtIO-BLK with a read-through cache,
and platform devices (fw_cfg, RTC, kvmclock, ramdisk).

## Files

| File | Lines | Purpose |
|------|-------|---------|
| device.cpp/h | ~200 | Block device registry, DeviceOps vtable |
| pci.cpp/h | ~300 | PCI bus enumeration, BAR extraction, config space |
| virtio_blk.cpp/h | ~700 | VirtIO-BLK driver: descriptor chains, 16MiB cache |
| fw_cfg.cpp/h | ~150 | QEMU fw_cfg device (DMA reads) |
| ramdisk.cpp/h | ~80 | RAM-backed block device |
| rtc.cpp/h | ~60 | CMOS RTC time readout |
| kvmclock.cpp/h | ~80 | KVM paravirtual clock (pvclock) |

## Architecture

```
Filesystem (ext2, FatFS)
  → DeviceOps.read / DeviceOps.write  (synchronous API)
    → VirtIO-BLK: ticket lock → cache check → SubmitRequest
      → Descriptor chain → virtq notify → busy-poll for completion
    → Ramdisk: direct memcpy
    → fw_cfg: port I/O + DMA
```

### VirtIO-BLK Cache
- 16 MiB per device, 4 KiB direct-mapped blocks (4096 entries)
- Small reads served from cache on hit; cache filled on miss
- Large reads (>= 64 KiB) bypass cache via coalesced path
- Writes invalidate overlapping cache entries
- Tag array tracks block number + valid flag per entry

### Request Serialization
- Ticket lock (`requestNext`/`requestServing`) for single in-flight model
- Non-IRQ-masking: timer interrupts fire during poll loop
- Profiler thread kept on polling path to avoid VFS reentrancy

## Audit Findings (2026-05-10)

### Known Issues
- **BRO-123**: `offset + len` overflow in read/write path; `sectorCount`
  from device never validated (zero or huge values cause issues)
- Cache block number not bounds-checked before computing `blockSector`
- Ticket lock has no timeout — deadlock possible if holder dies
- PCI BAR extraction doesn't validate BAR type bits thoroughly
- DMA buffer physical overlap check exists but is advisory only

### Overall Assessment
**Good for single-device QEMU use.** The cache design is sound and the
ticket lock avoids the earlier KMutex/scheduler reentrancy issues. Main
risks are unchecked arithmetic on device-reported values and lack of
timeout on the request lock. The vtable-based device model is clean and
extensible to AHCI/NVMe.
