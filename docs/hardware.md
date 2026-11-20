# Hardware support

## QEMU (main development target)

Brook runs on QEMU with KVM using UEFI (OVMF). It supports:

- virtio block, network, input and RNG devices;
- Intel HDA audio and xHCI USB;
- VirGL (OpenGL) and Venus (Vulkan) for graphics accelerated by the host GPU.

`scripts/run-qemu.sh` sets all of this up; see [building and running](building.md).

## Dell XPS 13 9343 (2015)

Brook's real-hardware target, installed on the internal SSD:

| Component | Status |
|---|---|
| Intel HD 5500 (Broadwell) GPU | Native driver: render and blitter engines, page flipping and zero-copy scanout. OpenGL through Mesa Iris, Vulkan through hasvk. |
| Display | 3200×1800 at 200% scaling, with backlight control. |
| Video engine | Hardware video decoding works but is off by default because it isn't yet faster than software ([details](bdw-video-engine.md)). |
| Storage | AHCI SSD, with boot generations and automatic rollback. |
| Keyboard and touchpad | Work. The Synaptics touchpad supports two-finger scrolling, tap to click and clickpad buttons. |
| Audio | Internal speakers through Intel HDA. A stream fault disables audio until the next reboot. |
| Networking | USB Ethernet adapters. The Broadcom Wi-Fi card is not supported (it has no open driver). |
| Power | No suspend yet, and only shallow CPU idle states. The GPU clock is capped for thermal reasons. |

Notes on GPU stalls are in [GPU hang diagnostics](gpu-hang-diagnostics.md).

## VMware

A small RAM-disk probe boots VMware Workstation to a software-rendered
desktop. Storage, networking and graphics acceleration are not ported. See
[VMware](vmware.md).
