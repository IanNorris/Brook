# VMware bring-up

Brook provides a standalone RAM-backed probe and a persistent desktop image
for Workstation. The probe can check firmware, graphics and timing with a
small image. The persistent image adds the network desktop profile, user
storage and an authenticated debug connection.

## Prepare the probe

```sh
nix-shell
scripts/build.sh Release
python3 scripts/make_vmware_probe.py --output build/vmware/probe
```

The output directory must be new. The script creates a GPT disk with a FAT32
EFI System Partition and converts it to a sparse VMDK. It verifies boot-file
readback and raw/VMDK equality. The image contains only built Brook artifacts,
the standard public CA bundle and a RAM-backed startup script. Existing data,
Nix, home and game disks are not read or copied. The manifest records input and
output hashes. A failed preparation leaves its directory for inspection; use a
new destination for the next attempt.

Open `build/vmware/probe/Brook.vmx` in Workstation. The configuration selects
UEFI with Secure Boot disabled, two vCPUs, 2 GiB RAM, IDE boot storage and
software graphics. Network, sound and USB are disabled for the initial test.
COM1 writes `serial.log` beside the VM configuration. Keep that log together
with VMware's `vmware.log` when reporting a failure.

UEFI loads the kernel and `INITRD.IMG` before leaving firmware services. The
RAM disk contains the early driver modules and `/boot/INIT.RC`, which starts
the native desktop and a kernel log window without requiring a runtime disk
driver. Missing user applications and wallpaper are expected in this small
probe. Guest changes are not persistent.

## Prepare a persistent desktop

The persistent image contains the network desktop profile and an ext2 home
volume. Build it from a clean checkout with an operator **public** policy and
a standalone recovery initrd containing Brook's pinned FAT and ext2 checkers.

```sh
nix-shell
python3 scripts/make_vmware_image.py \
  --output build/vmware/persistent \
  --operator-policy /path/to/operators.pub \
  --recovery-checkers-from /path/to/recovery/INITRD.IMG.gz
```

The persistent image uses NAT by default so the guest can reach external
networks. For an isolated Workstation acceptance run, build with
`--network hostonly`. The private operator seed stays on the host. Attach with
the bridge's explicit `--auth-mode v3` and the matching operator seed. This
bridge suite authenticates the operator to the guest.

The generated VM has 4 vCPUs on one socket and 8 GiB of memory. Override
them with `--cpus` and `--memory-mib` (also accepted by
`scripts/brook_image.py --target vmware`); `manifest.json` records both.

The default app set is Brook's Calculator, Qalculate!, Chromium, Files, App
Gallery and Mousepad, plus the Vim command-line package. Settings, Task
Manager and Terminal are built into every desktop. Firefox is left out of the
default image to keep the disk smaller; select it with
`scripts/brook_image.py --target vmware --apps ...` if needed.

Open the generated `Brook.vmx` in Workstation. It connects one SATA disk and
an e1000e adapter through a PCIe root port. It also enables the virtual HD
Audio device for stereo playback through the Brook mixer. The virtual driver
uses the codec connection lists to find an analog output. The mixer keeps its
44.1 kHz, 16-bit stereo format, and the controller supports direct 48 kHz,
16-bit stereo PCM. The disk has a guarded EFI System Partition, two desktop
slots, recovery boot files and an ext2 data partition.
Brook admits writes only after checking the virtual disk's pinned GPT and
ext2 identities and geometry. The manifest records those identities, the
network mode, and hashes of the disk, boot files and public operator policy.
Back up the VMDK while the guest is off. A clean guest shutdown flushes pending
writes. A dirty volume goes through the guarded recovery check before the
desktop mounts it.

Framebuffer console text can be incomplete during a mode change or concurrent
console writes. Use the VM's COM1 `serial.log` as the diagnostic record,
including for a panic during a display handoff.

The image pins one SATA disk at its original capacity with 512-byte logical
and physical sectors. Keep its geometry and leave other SATA ports unused.
Use IDE if you add a CD-ROM. A changed disk, additional active SATA device,
expanded disk or different sector geometry causes boot to halt before writable
mounts. The generated VM configuration disables hot-plug. Brook also
quarantines the disk until reboot if the controller reports a possible disk
replacement. Brook briefly rechecks the link during a SATA power transition
before deciding whether the disk is still online. The authenticated
bridge command `bridge hardware ahci-ro` includes a read-only `port_registers`
snapshot for the selected port when available.

Install Brook system or recovery updates by rebuilding the VMDK. The
in-place system and recovery tools require the physical-machine write policy
and refuse the VM profile. Save a powered-off copy and plan to migrate user
files before replacing the disk.

## VMware Tools integration

When exact VMware CPUID detection succeeds, the kernel runs a small
`vmware-tools` thread that acts as the guest "toolbox" service over the
GuestRPC backdoor (TCLO for host commands, RPCI for reports). It registers a
guest-managed tools version (`tools.set.version 2147483647`, as distribution
open-vm-tools does) and the `statechange` and `softpowerop_retry`
capabilities, so Workstation reports Tools as running. It reports the
DHCP address through `SetGuestInfo` and `guestinfo.ip`, and again whenever
the address changes or the host sets `broadcastIP`.

**VM > Power > Shut Down Guest** and **Restart Guest** send `OS_Halt` and
`OS_Reboot`. Brook accepts them only when the checked shutdown path is idle
and the platform offers ACPI S5 or reset. It then replies, reports
`tools.os.statechange.status`, and starts the same checked desktop shutdown
or restart as the launcher power menu. Apps with unsaved changes can still
hold the shutdown at their save prompts. `OS_PowerOn`, `OS_Resume` and
`OS_Suspend` are acknowledged without guest scripts.

With `--autofit`, the SVGA driver registers host resize hints through the
same session. The thread polls every 100 ms, each poll is a bounded number of
backdoor calls, and a transport error closes the session and retries after
two seconds. It never blocks boot. On QEMU, other hypervisors and physical
machines, it performs no backdoor I/O.

## Persistent image validation

The exact VMDK booted in headless QEMU with e1000e behind a PCIe root
port. A v3-authenticated session ran a job as the desktop user and captured
a 1920×1080 frame. The user's saved file survived a clean shutdown and an
interrupted boot followed by automatic ext2 recovery. A disk with a changed
GPT data partition ID was refused before any writes. Workstation acceptance
of the persistent image is a separate test.

## Pointer input

Brook checks for VMMouse automatically on VMware. In QEMU, select
`BROOK_POINTER_DEVICE=vmmouse` in the Brook launcher, or use
`-machine q35,vmport=on -fw_cfg name=opt/brook/vmmouse,string=1`. On success, the guest reports `MOUSE: VMMouse absolute pointer ready` and maps
host pointer positions to the guest display, independent of desktop scaling.
Mouse buttons and vertical wheel events use the same desktop input path as
PS/2. When a Brook app captures relative motion, VMMouse switches modes at the
next auxiliary interrupt. Releasing the capture returns to absolute motion.
If device probing or queue validation fails, Brook uses the PS/2 mouse.

For a Workstation input check, move the pointer into and out of the console
without clicking to grab it. Check keyboard input, all three mouse buttons,
wheel scrolling and relative capture in a test application. Use a disposable
guest and retain its serial log if input fails.

## Probe timing and portability

Actual VMware boots through UEFI, initializes both CPUs, passes the TLB
shootdown self-tests and presents the native desktop. The first boot exposed
a BSP initialization bug: Brook created execute-disabled page mappings before
enabling EFER.NXE. Brook now explicitly checks and enables that capability,
rather than depending on firmware to leave it enabled. LAPIC and IOAPIC MMIO
also use explicit uncached mappings.

On VMware, elapsed milliseconds use its invariant virtual TSC frequency from
CPUID leaf `0x40000010`, gated by exact vendor detection, RDTSCP/invariant-TSC
support and an acknowledged lazy-timer notification. This keeps deadlines and
RTC elapsed time independent of delivered periodic interrupt counts. KVM
continues to use its existing paravirtual clock. Unsupported platforms retain
the IRQ fallback. The VMware millisecond path does not claim nanosecond
precision or implement the KVM vDSO interface.

A warm ten-second sample measured 10.019 guest seconds against 10.018803245
seconds from VMware's diagnostic real-time counter. The sample ran on one
vCPU; it does not establish cross-vCPU or suspend/migration correctness.
Cold probes showed virtual-time catch-up, so early boot timing remains a
limitation. See VMware's [timekeeping paper](https://www.vmware.com/docs/vmware_timekeeping)
for apparent-time behavior and diagnostic counter semantics.

The same current image also boots under QEMU/KVM with KVM CPUID exposed:
serial selects KVM pvclock and all two-CPU TLB tests pass. Earlier portability
controls hid KVM CPUID and booted with VMware SVGA, no VirtIO devices and the
IRQ clock fallback. QEMU's fw_cfg device remains present without Brook option
files. These controls complement actual VMware testing.

Private probe manifests, serial logs, clock samples and disposable QEMU
controls are retained under `build/vmware/`. They are generated artifacts and
are not committed. The RAM-only probes have no runtime disk driver or user
state to sync; powering one off is not a persistent-filesystem shutdown test.

## Host setup and next gates

On NixOS, enable Workstation using:

```nix
virtualisation.vmware.host.enable = true;
# Let other hypervisors use VT-x while no KVM VMs are running.
boot.kernelParams = [ "kvm.enable_virt_at_load=0" ];
```

Build the host configuration first. VMware's `vmmon` and `vmnet` modules must
match the running host kernel; a prepared generation with a different kernel
requires a reboot. Workstation is not installed inside Brook.
Stop running KVM guests before starting VMware. The kernel option prevents KVM
from retaining virtualization hardware while idle; it does not permit both
hypervisors to own that hardware concurrently. See the
[Linux kernel parameter documentation](https://docs.kernel.org/6.12/admin-guide/kernel-parameters.html).

The user confirmed mouse movement and launcher clicks work in the native
VMware console on 27 October 2026. Keyboard, wheel and capture/release
acceptance remain pending. Loopback VNC captures confirm that the desktop
updates, but initial remote input attempts have not established working guest
input through that transport. The firmware pointer queue was already
disabled in the tested boot; the defensive PS/2 handoff is not a demonstrated
fix for this observation.

Workstation 25H2u1 accepted the network probe with e1000e behind a PCIe root
port. Polling was used because the firmware did not assign a legacy
interrupt. The hardware debug bridge connects over the guest network, so it
works independently of QEMU's QMP controls. Console keyboard, wheel and
pointer capture still need acceptance on the persistent image. VMMouse
absolute input and SVGA II acceleration are separate work.
