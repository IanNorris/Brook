# Potential next hardware target: discrete GPU

Status: selected next physical test after the Dell Broadwell milestone.
No hardware purchase, firmware changes, or physical testing implied.

Use the existing Windows desktop (ASUS ROG Maximus X Hero Z370, i7-8086K, 64 GB,
Zotac GeForce RTX 2080: Turing TU104, 10de:1e87, subsystem 19da:3500) for short
bare-metal sessions. Keep Windows as the default. First prove
platform boot and a repeatable debug connection; then consider a Turing
backend and eventually real game tests. Broadwell remains the integrated
GPU regression target.

Defer building a dedicated Brook desktop until hardware support is broader.
A used RX 6600 or Arc A380 remains an option for a future test machine.

## Desktop smoke test

1. Prepare a self-contained Brook UEFI USB with internal disk writes disabled.
   Use ASUS's F8 one-time boot picker and a rear Intel chipset USB port. Keep a
   monitor on the RTX 2080 and use firmware GOP only. Leave firmware and storage
   settings alone. Verify console, input, orderly shutdown and return to Windows.
2. Try our known USB Ethernet adapter for the debug bridge. The onboard I219-V
   has no verified Brook driver. Record the exact USB controller, boot and link
   results across a few restarts before deciding whether the desktop is practical
   for occasional iteration.
3. If convenient, capture a read-only Linux reference from a live USB: PCI
   topology and device IDs, bridge windows, BARs, interrupt capabilities,
   IOMMU groups and drivers, plus working display mode. Record Linux performance
   for the same workload and resolution once acceleration testing is in scope.

## Inventory facts (read-only smoke boot, run 1)

From the v2 smoke stick's boot inventory (`bridge hardware inventory`, config
space reads only; BARs decoded, never sized, so sizes are alignment bounds):

- **GPU 02:00.0** 10de:1e87 rev a1, VGA, subsystem 19da:3500, behind root port
  00:01.1 (8086:1905). The CPU PEG lanes are split x8/x8: 00:01.0 is empty, and
  00:01.1 has link_cap gen3 x8. The card reports link_cap gen3 x16 and
  link_sta **gen1 x8**; the gen1 is probably idle downtraining under GOP.
  MPS 128 (capability 256), MRRS 512.
  - BAR0 mem32 non-prefetchable 0xde000000 (register MMIO, align_max 32 MB)
  - BAR1 mem64 prefetchable 0xc0000000 (VRAM aperture, align_max 1 GB)
  - BAR3 mem64 prefetchable 0xd0000000 (align_max 256 MB)
  - BAR5 I/O 0xe000. The expansion ROM is at 0xdf000000 and disabled.
  - Root-port windows: mem 0xde000000-0xdf0fffff, pref64 0xc0000000-0xd20fffff
    (289 MB). There is **no ReBAR** extended capability. ECAM shows VC, LTR and L1SS.
  - MSI is 64-bit with one vector, and disabled. INTx pin A, intline 255 (unrouted).
- The same device has three more functions: 02:00.1 HDA audio (10de:10f8),
  02:00.2 a **USB 3.1 xHCI** for VirtualLink (10de:1ad8, BARs 0xd2000000 and
  0xd2040000), and 02:00.3 a USB-C UCSI serial-bus function (10de:1ad9). A bus or
  function reset of the GPU affects all four functions. Brook's xHCI driver
  claims 02:00.2 like any other xHCI.
- **VT-d:** a single DRHD at 0xfed90000 with INCLUDE_PCI_ALL (segment 0, host
  address width 39, interrupt remapping supported). Firmware left DMA
  translation and IRQ remapping off (gsts 0). One RMRR at
  0x8cec9000-0x8cee8fff covers the xHCI controllers 00:14.0, 04:00.0 and 05:00.0
  (USB legacy handoff buffers). Any IOMMU bring-up must keep that range
  identity-mapped for those requesters. The GPU has no RMRR.

## First accelerated backend

- Scope to one card and slot. Use firmware-assigned resources. Confirm 64-bit
  BAR and bridge window handling, distinguish register MMIO from the VRAM aperture,
  and use the appropriate cache policy. Defer BAR resizing, resource rebalance
  and hotplug. The card exposes no Resizable BAR capability (Turing), so BAR1
  stays the firmware-assigned 256 MB-class window described below.
- Inventory the PCIe requester and neighbouring functions. Brook's Broadwell
  DMAR code is scoped to the integrated endpoint at 00:02.0. Extend DMA ownership
  to the discrete requester before enabling driver bus mastering. Keep GPU
  address spaces and system IOMMU mappings separate.
- Start with firmware initialization, one queue, bounded submission and a
  retiring fence. Prove a known GPU rendering result before attempting native
  scanout. Test engine or function reset where supported, including warm boot
  and timeout recovery. A bus reset requires knowing all affected functions.
  Do not recycle memory until outstanding DMA has stopped or been isolated.
- Reuse Mesa where possible: Iris with a supported Intel kernel interface,
  RadeonSI/RADV for AMD, or NVK with Zink for NVIDIA. Reuse Brook's buffer,
  queue and fence ownership contracts across backends. Then prove native
  presentation without CPU readback, safe display retirement, and matched
  Quake and compositor runs at 1080p and 4K against Linux on the same box.

Full inventory: kept host-side with the run-1 artifacts (not committed).

References: [ASUS board specs](https://rog.asus.com/uk/motherboards/rog-maximus/rog-maximus-x-hero-model/spec/),
[ASUS manual](https://dlcdnet.asus.com/pub/ASUS/mb/LGA1151/ROG_MAXIMUS_X_HERO/E13645_ROG_MAXIMUS_X_HERO_UM_V2_WEB.pdf),
[ASUS BIOS notes](https://rog.asus.com/motherboards/rog-maximus/rog-maximus-x-hero-model/helpdesk_bios/),
[Linux PCI API](https://docs.kernel.org/driver-api/pci/pci.html),
[Linux Xe submission](https://docs.kernel.org/gpu/xe/xe_cs.html),
[AMDGPU architecture](https://docs.kernel.org/next/gpu/amdgpu/driver-core.html),
[Mesa NVK](https://docs.mesa3d.org/drivers/nvk.html).

## Game validation

Once the RTX 2080 can render and present reliably, compare the same yquake2
demo and Vulkan test on Brook and Linux on that desktop. Then try a known
Steam title with matched settings and capture displayed frame pacing.
A firmware framebuffer smoke test does not establish game readiness.
