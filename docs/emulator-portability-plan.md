# Emulator portability plan

Status: proposed, 27 October 2026. Reviewed with Astra. Hyper-V is out of scope.
Implementation is deferred while the existing qemu-wasm experiment is revisited.

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
VMware boot evidence and pending input acceptance in [vmware.md](vmware.md).

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

## Sources

- [Oracle emulated devices and paravirtualization providers](https://docs.oracle.com/en/virtualization/virtualbox/7.2/user/working-with-vms.html)
- [Oracle storage interfaces](https://docs.oracle.com/en/virtualization/virtualbox/7.2/user/storage.html)
- [Oracle flush troubleshooting](https://docs.oracle.com/en/virtualization/virtualbox/7.2/user/Troubleshooting.html)
- [Oracle NIC models](https://docs.oracle.com/en/virtualization/virtualbox/7.2/user/networkingdetails.html)
- [Mesa SVGA3D components](https://docs.mesa3d.org/drivers/svga3d.html)
