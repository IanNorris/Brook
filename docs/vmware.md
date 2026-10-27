# VMware bring-up

27 October 2026. Workstation 25H2u1 boots the standalone probe to a
1920×1080 software desktop with two vCPUs. This is an early portability
milestone; the full Nix desktop and its applications are not supported yet.

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

## Verified behavior and timing limits

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

The next gates are keyboard/wheel/capture acceptance, cold-start clock behavior
and a persistent-filesystem clean stop/reboot. Brook currently has no AHCI/NVMe,
e1000/vmxnet3 or VMware SVGA acceleration driver. Persistent disks, networking
and accelerated graphics therefore require additional work after the probe.
The QMP-based VM bridge also needs a separate VMware transport before its
normal control tools can be used here.
