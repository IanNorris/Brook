# Emulator portability plan

Status: proposed, 27 October 2026. Reviewed with Astra. Hyper-V is out of scope.
Implementation is deferred while the existing qemu-wasm experiment is revisited.

## First physical-hardware gate: XPS 13 9343 (30 October)

Start with firmware loading and kernel entry, independently of native USB
drivers. Build the loader/kernel, then run inside `nix-shell`:

```
python3 scripts/make_uefi_probe.py --output build/hardware/xps9343-probe
```

Copy the generated `payload/` contents onto the FAT32 USB partition. The builder
does not open physical devices or format the stick. It creates a separate GPT
test image and verifies its FAT file contents against SHA256 hashes. The payload
uses the removable-media `EFI/BOOT/BOOTX64.EFI` path, `KERNEL/BROOK.ELF`, root
`INITRD.IMG` and `BROOK.CFG`. No runtime driver modules or user disks are packed;
the expected success screen is a RAM-only software desktop and kernel console.
The built-in PS/2 fallbacks may still respond; input, persistent USB storage
and applications need separate acceptance. Boot unsigned
development images with Secure Boot disabled and select the UEFI USB entry.

`DEBUG_TEXT=1` now enables the loader's existing console diagnostics and explicit
kernel-load/handoff milestones. The loader rejects successful-but-short file
reads rather than handing incomplete contents to the ELF/initrd consumers.

For phone diagnostics, start `tools/panic_receiver.py --port 9001 --elf
<frozen-payload-kernel>`; keep that exact ELF unchanged while testing. The receiver
saves reports and local inbox notifications under `crash_reports/` rather than
the removed Enclave workspace. The panic URL QR currently targets
`https://khione:9001/`; use the host's LAN address manually if name resolution
is unavailable. The receiver uses its local certificate. The QR payload carries
registers, stack and process/system state, not a complete serial boot transcript.

The first disposable probe boots through OVMF's USB storage path without VirtIO
devices or KVM CPUID exposed and reaches the software desktop. A deliberate
panic's actual emitted Base45 bytes pass HTTPS ingestion, LZ4 decoding and
symbolication against the frozen ELF; Terra confirms coherent desktop/panic
frames. The user subsequently booted this payload on the actual Dell XPS 13
9343 on 30 October. Their photograph shows the desktop and kernel console;
Terra reads APs 1–3 online, **4/4 CPUs online**, boot complete and the window
manager enabled, with no obvious gross display corruption. This passes the
first physical UEFI/kernel/RAM-desktop gate. It does not establish input,
native USB storage, clock accuracy or sustained stability. The missing
wallpaper is expected in this minimal payload. The photo remains a private chat
attachment and has not been published into the repository.

The user also confirmed phone connectivity to the scanner. It now runs as the
`brook-panic-receiver.service` user unit so it survives command-session teardown;
port 9001 was already covered by the host's configured 9000–9200 TCP range.
Optical scanning of an actual hardware panic remains untested.
Private hashes, USB copy receipt and VM evidence are under
`build/hardware/xps9343-uefi-20261030/`.

## Deliverables and order

1. A usable persistent software-rendered desktop under VMware and VirtualBox.
2. A portable QEMU distribution with platform launchers, MCP bridge and VM harness.
3. Native SVGA display improvements after functional acceptance. Accelerated
   SVGA3D is a separate project, not a reuse of the VirGL/Venus transport.

## VMware and VirtualBox gates

### 1. Match the RAM-only baseline

Generate both configurations from the same hashed Brook image: UEFI, Secure
Boot off, two CPUs, serial logs, PS/2 input, no network/audio/USB initially.
Observe real guest responses to typing, modifiers, movement, clicks, wheel and
capture/release. Sending input is insufficient evidence. Preserve the current
VMware boot evidence and remaining input acceptance in [vmware.md](vmware.md).
The user has confirmed native-console mouse movement and launcher clicks;
keyboard, wheel and capture/release still need validation.

### 2. Validate clocks and SMP

Measure cold/warm and idle/loaded-host timing, monotonic deadlines and timer
lateness. Sample on both vCPUs and across task migration; a monotonic floor can
hide TSC offsets. VMware warm timing passes but cold catch-up remains open.
Configure VirtualBox's KVM paravirtualization provider first to exercise Brook's
existing pvclock; separately test unsupported-provider fallback. Suspend and
snapshot restoration remain unsupported until explicitly tested.

### 3. Implement AHCI as the shared storage baseline

Implement one disk and one outstanding command first, with bounded completion,
error recovery and explicit flush. Use existing BlockDeviceOps/filesystems;
load from initrd before mounting disks. Generalize the virtio-specific mount
probing so all supported block devices can use the existing mount markers.
Defer NCQ, hotplug, throughput tuning and NVMe.

Acceptance order: read-only hashes; disposable write/read/hash checks; flush
and injected failures; clean shutdown/reboot plus offline filesystem checks;
then the complete Nix/application image. On VirtualBox durability profiles set
SATA IgnoreFlush=0: the default may acknowledge flush without forwarding it.
Keep original images immutable and use private working copies.

### 4. Add networking

Try VirtualBox virtio-net first, inspecting its PCI capabilities and features.
Brook currently requires modern capabilities even for a transitional PCI ID;
reuse is not yet established. For VMware, implement one verified Intel model
and check whether VirtualBox offers a matching fallback. Treat e1000/e1000e
as different interfaces; defer vmxnet3.

Require DHCP, DNS, hashed TCP transfers, sustained RX/TX, reset/link recovery
and interrupt teardown. Retain QEMU regressions throughout.

### 5. Extend the VM harness

Report backend capabilities explicitly. Begin with launch, serial logs,
identity, stop and artifact collection; add screenshots/input and diagnostics
only when the backend supports them. Preserve session generations, cancellation
and guest-confirmed shutdown. Do not emulate unavailable QMP operations or
count host power-off as a clean guest sync.

### 6. Display and convenience

Ship GOP/software rendering first. Then add capability-negotiated SVGA mode
setting, dirty updates, cursor and resizing, tested separately on VMware and
VirtualBox VMSVGA. Scope SVGA3D separately. Audit audio controllers/codecs and
USB integration after the persistent desktop is reliable.

## Portable QEMU package

User-requested additional deliverable: a downloadable package with launch
commands for Windows, macOS and Linux, bundled with Brook's MCP scripts and
VM harness. Inspect the existing package_qemu_folder.sh, run-qemu.sh and
run-qemu.bat before designing a replacement.

The existing folder packager copies the current ESP and runtime/user disks;
it is not a public-release allowlist. Its Windows launcher expects QEMU to
be installed, and MCP packaging is not present. Reuse its launch knowledge,
not its current copy-everything behavior for a distributable build.

Prefer per-platform/architecture archives sharing a versioned guest image and
manifest, rather than one giant archive. Investigate legally redistributable,
pinned QEMU and firmware binaries, their complete license/source obligations,
and host runtime dependencies before deciding whether to bundle binaries or
bootstrap verified downloads. Do not promise an entirely self-contained archive
until this audit passes. Preserve source and third-party notices.

Required behavior:

- One documented launch command per platform, including paths with spaces.
- Explicit software-rendering fallback; probe acceleration and display/audio
  capabilities rather than assuming KVM, HVF or WHPX is available.
- Platform-appropriate MCP entry point, logs and VM control endpoints; audit
  Unix-socket, PTY, process-lifecycle and native Python dependency assumptions.
- Package a runtime or define a tested prerequisite; avoid hidden Nix/PATH
  dependencies. Keep user disks/settings in a separate writable data directory.
- Manifest build ID, guest ABI, host architecture, checksums and tool versions.
- Public-data-only default image: no commercial games, accounts, saves or
  personal media. Paid content remains an explicit local import.
- Smoke tests on actual Windows, macOS and Linux hosts before claiming support:
  boot, display/input, bridge discovery, capture, guest command, clean shutdown,
  restart and filesystem integrity. Cross-compilation alone is insufficient.

## Dell input and software-display probe (2026-10-30)

The first physical boot reached the desktop with all four logical CPUs online.
The user confirmed the internal pointer works, but reported very slow redraws
and no response to Super+Enter. The RAM-only probe has no terminal executable,
so that shortcut alone does not establish keyboard failure.

The next probe explicitly configures i8042 IRQ1/translation, preserves auxiliary
settings, and verifies the command byte with port 1 disabled before enabling it.
Both keyboard and mouse initialization preserve the caller's interrupt state.
Controller waits and stale-byte drains are bounded; keyboard reset, scan-set
negotiation and USB driver bring-up remain separate work.

`make_uefi_probe.py --input-probe` requests `DISPLAY_WIDTH=1280` and
`DISPLAY_HEIGHT=720` in BROOK.CFG. GOP selection prefers the largest mode fitting
both dimensions, or the smallest linear framebuffer if none fits. Without these
settings the default remains 1920x1080, then 1280x720, then largest available.

The optional `hwprobe on` command reports geometry, raw IRQ1/status/byte counters,
decoded key actions and draw versus framebuffer-copy time in the kernel console.
These durations use the guest clock and are diagnostic, not a wall-clock timing
validation. Press plain A repeatedly and photograph HWKEY/HWKBD/HWMS lines.
External USB keyboards are not supported by this module-free probe.

The final payload passed USB/UEFI boot under QEMU without VirtIO devices or KVM
clock CPUID exposure, at 1280x720; three A presses and releases reached both raw
counters and decoded events. Host tests cover mode-order permutations, auxiliary
configuration preservation, controller timeouts and failed readback. Entry-state
and scheduler-start audits passed. The user subsequently confirmed that both built-in keyboard and trackpad work
on the Dell, and lower resolution improves the estimated redraw rate to 5–10 FPS.
A subsequent photo confirms 800x600, stride 800. Guest-clock samples report
draw=0 ms at timer resolution and copy=80–141 ms (one sample 133 ms), with
periods around 96–157 ms. This makes framebuffer-store bandwidth the leading
next target; bare-metal clock accuracy is not yet independently verified.
No framebuffer PAT/cache alias change
was made: compositor HHDM and TTY aliases must be considered together with MTRRs.

## Early framebuffer write-combining probe (2026-10-30)

The next payload holds resolution at 800x600 for comparison. Immediately after
VMM setup, before heap/TTY/AP startup, it validates the GOP physical span and
prepares replacement page tables for both retained identity and HHDM aliases.
Only framebuffer pages change; adjacent mappings retain their original flags.
Allocation or validation failure leaves the original mappings intact. A cache
and global-TLB transition brackets an allocation-free commit.

This first implementation is gated to Intel CPUs with supported PAT layout,
enabled MTRRs and defined overlap behavior. Firmware MTRRs are never modified.
Every AP must match the BSP's PAT/MTRR state before online admission, or remains
parked. The TTY and compositor share the physical mapping. New raw /dev/fb0
aliases inherit uniform cache flags; mixed-type spans are rejected. Managed
application VFB RAM keeps write-back caching. Framebuffer write batches finish
with SFENCE, including boot/panic paths. HWFB reports WC or unchanged.

Host tests pass for boundary splits, adjacent mappings, allocation rollback,
unsupported huge/PAT layouts, shared page tables, alias cache flags and MTRR
validation. Existing VMM/PAT regressions and compiled entry/scheduler-start audits
also pass. QEMU USB/UEFI checks pass at 800x600 with four CPUs for WC enabled,
PAT-disabled fallback, decoded PS/2 events and a symbolicated panic. Visual
review also exposed an existing low-resolution panic QR clipping bug: URL and
payload codes now fit separate bounded cells, including a 2x2 grid for up to four
pages, with diagnostic text kept out of the QR column. Layout tests cover
640x480 through 3840x2160. The first
fallback attempt hit a host QMP timeout during image cleanup; the repeat with
cleanup paused passed. This is correctness evidence, not a physical performance
claim. In the subsequent Dell run, the user reports CPU usage falling to 1%
and much smoother interaction. The 800x600 photo confirms HWFB cache=WC,
HWMS draw=0 and copy=0 at timer resolution, with period=16 ms on the guest
clock. The prior copy time was 80–141ms. Wall-clock timer accuracy remains
unverified. A configuration-only repeat
requests the largest linear GOP mode within 3840x2160, retaining the exact
verified kernel/initrd/loader. The subsequent physical photo confirms
3200x1800, stride 3200, HWFB cache=WC, and visible HWMS draw=0 copy=0 period=16
on the guest clock; taskbar CPU is 1% and memory 12%. The user confirms it
remains very smooth at this resolution. This validates responsiveness, not
independently calibrated panel refresh rate or timer accuracy.

References: [Linux PAT alias rules](https://www.kernel.org/doc/html/latest/arch/x86/pat.html)
and [Intel memory-type tables](https://cdrdv2-public.intel.com/812380/252046-sdm-change-document.pdf).

## Sources

- [Oracle emulated devices and paravirtualization providers](https://docs.oracle.com/en/virtualization/virtualbox/7.2/user/working-with-vms.html)
- [Oracle storage interfaces](https://docs.oracle.com/en/virtualization/virtualbox/7.2/user/storage.html)
- [Oracle flush troubleshooting](https://docs.oracle.com/en/virtualization/virtualbox/7.2/user/Troubleshooting.html)
- [Oracle NIC models](https://docs.oracle.com/en/virtualization/virtualbox/7.2/user/networkingdetails.html)
- [Mesa SVGA3D components](https://docs.mesa3d.org/drivers/svga3d.html)
