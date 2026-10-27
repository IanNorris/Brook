# VMware bring-up

27 October 2026. VMware execution is not yet verified. A standalone boot probe
and a QEMU portability control are available; this is not the full Nix desktop.

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

## Current portability control

The same VMDK reaches a 1920×1080 software desktop in QEMU with the VMware SVGA
device, no VirtIO devices and KVM CPUID exposure disabled. Serial confirms the
non-KVM LAPIC clock fallback, PS/2 keyboard/mouse initialization, successful
two-CPU TLB shootdown self-tests and the desktop-presented event. This checks
image layout and removes several QEMU device dependencies; it does not prove
VMware firmware, interrupt routing or timing compatibility. QEMU's fw_cfg
device is still present, with no Brook option files supplied.

Both FAT filesystems pass offline checks, and the control's disposable overlay
leaves the prepared VMDK unchanged. Private evidence is under
`build/vmware/probe-20261027-b/`, with earlier preparation evidence under
`probe-20261027-a/`. An initial control waited for an unsupported startup echo
and reached its deadline despite a completed boot; the corrected image removes
that command and its control waits for the serial desktop-presented event.

## Host setup and next gates

On NixOS, enable Workstation using:

```nix
virtualisation.vmware.host.enable = true;
```

Build the host configuration first. VMware's `vmmon` and `vmnet` modules must
match the running host kernel; a prepared generation with a different kernel
requires a reboot. Workstation is not installed inside Brook.

The next gates are an actual VMware boot, usable keyboard/mouse, a measured
clock sanity check and a clean stop/reboot. Brook currently has no AHCI/NVMe,
e1000/vmxnet3 or VMware SVGA acceleration driver. Persistent disks, networking
and accelerated graphics therefore require additional work after the probe.
The QMP-based VM bridge also needs a separate VMware transport before its
normal control tools can be used here.
