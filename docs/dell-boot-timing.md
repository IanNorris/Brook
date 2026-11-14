# Dell XPS 13 9343 boot timing

## Snapshot: 14 Nov 2026 (gen 371)

This is the first full breakdown on the Dell. It needs both the timing loader (`BROOKT6.EFI`, promoted as `BOOTX64.EFI`, `a42be35c…`) and a kernel built at or after `d4d9915d`.

**Configuration:**
- Payload `dell-main-timing-20261114` (main `0bf7b7a8` lineage), guarded SSD boot with the default boot-menu wait.
- **Firmware boot mode: Dell BIOS "Comprehensive" (thorough POST), not "Fast".**
- Boot `bb13619038f34fb68e1abcbedf37097e` session; the device was cold from a checked restart.

| Stage | Time | Source |
|---|---|---|
| Firmware (reset → loader entry) | **5,079 ms** | ACPI FPDT; loader TSC agrees at 5,107 ms |
| Boot-menu wait (F5 recovery window) | 500 ms | loader TSC; configured 500 ms, was 2 s |
| Loader (after wait → kernel jump) | **3,724 ms** | loader TSC |
| Kernel (entry → first desktop present) | 2,863 ms | kernel TSC |
| **Total** | **12,166 ms** | |

Raw lines:

```
BOOT_TIMING firmware_ms=5079 src=fpdt wait_ms=500 loader_ms=3724 kernel_ms=2863 total_ms=12166 tsc_khz=2394569 proto=5 ext=1
BOOT_TIMING_DETAIL tsc_fw_ms=5107 fpdt_fw_ms=5079 fpdt_reset_ms=0 fpdt_ebs_ms=9269 flags=0x3 wait_cfg_ms=500 wait_keys=0 last_key=0x00000000
```

The same boot path running the gen 370 kernel (which predates the marker read) gave `firmware_ms=5085 kernel_ms=2839`. The earlier old-loader boot measured firmware 4.27 s and kernel 2.79 s.

## Tuning opportunities (not yet pursued; boot time is secondary to display performance)

- **Firmware, 5.1 s.** The BIOS is set to "Comprehensive" boot. "Fast" (or "Minimal") skips most POST device enumeration and should cut this substantially. Before switching, check that Fast boot still:
  - initialises USB in time for F12 and USB maintenance boots;
  - initialises ConIn for the F5 wait;
  - keeps FPDT and the EFI runtime services Brook relies on.
- **Loader, 3.7 s.** This is the largest stage Brook controls. It reads the kernel and the 32 MiB initrd from the SSD through UEFI, then guard-verifies and hashes them. Profile the read path (block size, FAT versus ext2 slot reads) against the hashing before changing anything.
- **Wait, 0.5 s.** Already reduced. `wait_keys=0` here because no key was pressed. The Dell report that keys are ignored during the wait is still untested with this loader.
- **Kernel, 2.9 s.** Not broken down yet.
