# Dell XPS 13 9343 hardware bring-up

Work branch: `hardware/xps9343-uefi-probe`. Wi-Fi is excluded. Networking and
host diagnostics take priority over internal audio and native Intel graphics.
Keep known-good payloads and their exact symbol files in private build outputs;
never repartition the user's FAT32 stick to run these probes.

## Verified on the Dell

The user booted through UEFI to the software desktop with all four logical CPUs
online. Built-in keyboard and trackpad work. Consistent write-combining mappings
reduced framebuffer-copy cost dramatically: the user reports a smooth desktop
at 3200×1800, with about 1% CPU in the idle diagnostic run. The guest reports a
16 ms frame period; this is not independent wall-clock timing evidence.

A subsequent xHCI/storage probe reaches the desktop on the Dell and starts its
automatic read check. Repeating display diagnostics obscured the result, so
physical file-integrity acceptance is **not yet established**. Further payloads
should leave repeating diagnostics off and retain the final test result and
network address visibly in the console.

## USB Ethernet checkpoint

The Tecknet UL699G attached to the development host identifies as Realtek
`0bda:8153`, device revision `0x3000`. Its second configuration exposes CDC-ECM,
communication interface 0 and data interface 1 alternate 1. The driver chooses
by descriptors and configuration value, rather than descriptor index. It reads
the MAC string, configures bulk endpoints including SuperSpeed burst limits,
sets the directed/broadcast filter and receives link notifications.

RX remains outstanding while idle. A bounded packet queue applies backpressure
when full; packet processing runs outside USB locks. TX owns bounded DMA slots
until completion. Exact-MaxPacket frames get a **separate zero-length TD**:
chaining a zero-length TRB inside the data TD did not reliably emit a USB ZLP
and allowed the following Ethernet frame to become trailing data. Matched
transfer errors quarantine the interface without freeing potentially active DMA.
Permanent network callback registration retains the providing module.

DHCP startup now uses an elapsed two-second retry deadline. The previous fixed
spin count did not establish any minimum wait and could exhaust retries before
an adapter's link had settled.

QEMU acceptance uses the real guest network stack, xHCI, USB mass storage and
USB HID on one controller. It verifies 256 KiB of bidirectional TCP data and a
982-byte UDP echo, checks captured 64-byte and 1024-byte Ethernet frames, leaves
RX idle, reads USB data during traffic, executes a framed debug request, then
removes the NIC and verifies continuing USB reads and keyboard input.

```
# After building the kernel, inside nix-shell:
python3 scripts/test_usb_ecm.py --output build/hardware/ecm-check
```

The physical adapter's **ECM traffic is not accepted yet**. Passthrough selected
configuration 2, completed TX, and delivered link/speed notifications, but no
RX or DHCP reply. A host userspace standard-ECM comparison also received no
traffic, while restoring the host's ordinary vendor driver immediately gave
carrier and a successful adapter-bound HTTPS request. The ECM descriptor MAC
matches the working host MAC; a promiscuous-filter comparison did not help.
These observations do not justify blaming Brook's RX ring alone or claiming
that the adapter is ready for the Dell. The adapter subsequently disappeared
from the host USB device list, so further physical comparisons await its return.

Next physical diagnosis: preserve complete initial notifications and speed
payloads; verify actual bulk-IN submission/bulk-OUT bytes with USB tracing; check
read-only PHY state if needed. Do not add speculative vendor PHY writes. Also
keep explicit USB port assignments in the harness: an automatic-port passthrough
run failed in OVMF before Brook loaded, independently of the network driver.

`/etc/usb-net-probe` enables bounded startup notification samples and periodic
counters. It is diagnostic-only and should be absent from the normal desktop
image so it does not obscure the final status.

## Network-only hardware bridge checkpoint

The existing MCP package now provides separate `hw_*` tools and a hardware
registry. Attach a literal IP explicitly; nothing scans the LAN. The endpoint
is pinned to a kernel boot identity. Generic requests are guarded in the kernel;
structured jobs and semantic UI operations retain their existing boot checks,
durable request IDs and recovery semantics. TCP reachability and identity
mismatch are reported separately. No VM process, QMP socket or VM registry is
required by the hardware bridge.

Available tools cover diagnostics, memory/process/frame-interval statistics,
kernel logs with line cursors and retention-loss reporting, structured guest
commands with separate stdout/stderr and real exit status, app/window discovery,
and semantic window actions with read-only recovery. The package reports QMP,
raw input, raw framebuffer transfer, registers, raw memory and panic transport
as unavailable. A dropped connection does not identify a panic.

Kernel log snapshots exclude the current partial line. Review found that the
512-slot ring retains only 511 completed lines because one slot is reserved for
the current write; the readers and dropped-line counts now use that capacity.
A native host test verifies wrapping plus an unfinished current line.

The hardware MCP path passes a real stdio test through a QEMU guest TCP forward:
logs/stats, UID-1000 guest execution with exit 17 and separate UTF-8 stdout/stderr,
identical request recovery, app/window discovery, minimize/restore/maximize,
and explicit reattachment after a guest reset. Old observations and mutation
requests are rejected after reset. The bridge has no visible VM registry; QMP
is held only by the test harness. This remains emulator/protocol acceptance,
not physical Ethernet acceptance.

```
python3 scripts/test_hardware_bridge.py --output build/hardware/hardware-bridge-check
```

See [bridge instructions](../tools/vm_bridge/README.md#physical-hardware-over-tcp)
for standalone calls and the MCP client reload needed to discover new tool names.
The existing physical stick and its pinned panic-scanner ELF are unchanged by
these disposable tests.

## Remaining core work

- Complete physical USB integrity and Ethernet acceptance.
- Exercise the network-only host bridge on the Dell once Ethernet works.
  Hardware post-panic TCP inspection remains unavailable: the panic path halts
  CPUs; retain the optical scanner fallback.
- Mount an immutable compressed application image stored as an ordinary FAT32
  file, preserving Nix store symlinks and executable metadata. Provide writable
  RAM-backed session directories and a compact offline desktop package profile.
- Validate external USB input, reconnect/error handling, and power/shutdown on
  hardware. Inventory the actual PCI/ACPI devices before assigning more drivers.
- Prepare one concrete hardware review payload with readable default scaling,
  visible final storage/network status, and an unchanged recovery copy.

## Stretch work

Master volume and mute controls exist and pass captured QEMU audio tests. New
hardware probes start muted at 20%. This does not supply an internal audio
codec driver. The Dell's analog path needs Intel SST/SSP/I²S plus RT286/ALC3263
investigation rather than simply enabling the existing QEMU HDA module.
Native Intel HD display/GPU work follows core usability; the GOP software path
already supplies a working smooth desktop. Wi-Fi remains out of scope.

## Working constraints

The user is away for roughly 18 hours from the 30 October handoff. Continue
independent preparation, then request combined physical testing. Stop and leave
a checkpoint if reported remaining token quota reaches 5%; do not consume a
quota reset. No quota-reading tool was exposed at this checkpoint, so discover
one again if tool availability changes rather than inventing a percentage.


### 2026-10-30: compressed applications on the existing FAT32 stick

The kernel can mount an application image stored as a regular file on the USB
FAT filesystem. `/etc/boot-app-image` contains one complete record, for example
`/media/usb/APPS.BRK /nix`. The source must be an actual streaming FAT handle
backed by a device that advertises a lifetime read-only guarantee; ordinary
read-only mount flags, writable filesystems, cached file handles and recursive
compressed sources are insufficient. The USB read-only boot marker establishes
that guarantee before device registration. Partitions forward it and independent
FAT aliases inherit it. Each active FAT instance holds its drive binding so a
new mount cannot invalidate its retained FILs.

`VFS_O_STREAM` bypasses both whole-file FAT cache lookup and preloading. The
compressed mount retains its source vnode until its last object/FD/mapping is
released, including after namespace detach. Source close is deferred to the
existing VFS cleanup worker, outside the namespace mutation lock and compressed
I/O mutex. Registered storage callbacks now pin their module providers.

Validated with `scripts/test_usb_app_image.py` under QEMU/KVM using 512 MiB RAM,
an MBR/FAT32 USB device and xHCI: ELF execution from the compressed file,
cross-block reads, hardlinks/symlinks, permission checks, private COW mappings,
read-only rejection, and continued reads after detaching both source and image
mounts. Evidence: `build/hardware/usb-app-image-v3-20261030/result.json`.
The native FAT adapter tests additionally cover a 1 MiB streaming open without
preload, cached-reader bypass, mutation rejection before buffer changes, and
retained mount refusal; drive tests cover alias policy and rebinding refusal.
Compressed mount tests exercise initialization failures, short/error reads,
slot reuse and exactly-once deferred source close.

Prepare a disposable image with `scripts/make_uefi_probe.py --driver xhci
--usb-read-only --media-mount /media/usb --application-image /path/APPS.BRK`.
The generator sizes its FAT partition for the payload and verifies copied files
with streaming hashes. It never writes to physical devices. This is preparation
for an offline desktop image; it is not physical acceptance of USB reads or
Ethernet. The physical stick and panic scanner remain on the prior known probe.

### 2026-10-30: offline hardware desktop payload

`nix/hardware-packages.nix` builds an explicit open-source offline profile:
Bash/coreutils, curl/CA certificates, Vim, Mousepad, Files, Settings, Calculator,
Wayland, fonts/cursors and the session/message services. No browser, Mesa/LLVM,
Steam, account data or paid game assets are copied. Its 163-path, approximately
519 MB closure becomes a 262,602,752-byte compressed application file; the
prepared disposable disk is 450 MiB.

Reproduce inside the development environment:

```sh
nix-build nix/hardware-packages.nix -A profile -o build/hardware/offline-profile
cmake --build build/release --target kernel
cmake --build build/release/kernel --target module_xhci module_sched_eevdf
python3 scripts/prepare_hardware_desktop.py --output build/hardware/offline-desktop-new
```

The generator creates files and a disposable test image only. It does not open
physical devices. Its initrd opts into `/etc/volatile-desktop`: bounded tmpfs
instances for `/tmp` and `/data`, in addition to the existing `/dev/shm` instance.
**Home and settings are ephemeral and disappear at shutdown.** A fresh home is
created through retained VFS operations before scheduling starts, owned by
UID 1000/GID 100. An optional bounded `/etc/desktop-defaults.ini` seeds settings
only when none exist; the native-resolution payload selects 200% scale.
Persistent homes retain their existing ownership and preferences.

This testing found and fixed missing tmpfs socket inodes: Wayland and D-Bus can
now bind, connect and retain socket identities through unlink/rebind. Ordinary
socket file I/O/mapping stays disabled. Startup runs Wayland as the Brook user;
D-Bus gets its own mode-0700 runtime directory, and the message watcher waits
for the Wayland socket. Kernel home creation, Settings reads and debug-job cwd
validation use retained paths, so they see tmpfs rather than hidden FAT paths.
The wallpaper is center-covered and bilinearly fitted to the actual framebuffer
once during boot; no scaling work is added to steady-state frames.

The payload includes EEVDF, a finite IP/debug-port status banner, and six checked
8 MiB USB reads. With desktop services running, global free-memory changes are
diagnostic rather than a false leak assertion; the isolated probe retains its
original memory-growth check. Write rejection must specifically return EROFS.
Periodic `hwprobe` spam is disabled.

Acceptance evidence:

- `build/hardware/offline-desktop-review-v6-20261030/result.json`: fresh
  3840×2160 QEMU/KVM software desktop, 200% scale, EEVDF, USB integrity, IP banner,
  user-owned writable RAM home, Unix sockets and live Mousepad/Files/Settings.
- Terra inspected both fresh desktop and the fitted 4K wallpaper. Images are
  in that directory (`visual-qa.png`, `visual-qa-1280.png`). Settings' Apply action
  was not exercised; title/status text clipping remains cosmetic follow-up work.
- Nine selected tmpfs/VFS/FAT/mount/shutdown host tests pass in
  `build/hardware/offline-host-tests.log`.

The validated payload was copied to the existing FAT32/MBR USB without formatting
or deleting unrelated files. All eight files passed readback hashing, and the
stick was safely unmounted. Receipt and exact previous-file backup:
`build/hardware/offline-desktop-v4-20261030/probe/usb-copy-receipt.json` and
`usb-before/`. The new panic scanner service is
`brook-panic-receiver-offline-20261030.service`, pinned to that payload's exact
ELF; the earlier probe and its ELF remain preserved.

**Physical acceptance remains pending.** In particular, the real Tecknet ECM
path still has no verified receive traffic. Linux's
[r8153 ECM driver](https://github.com/torvalds/linux/blob/master/drivers/net/usb/r8153_ecm.c)
uses the standard CDC binding plus MII accessors; this supports continuing the
bounded ECM investigation, not assuming networking is solved. The adapter is
currently absent from the host, so further physical traces/PHY diagnostics wait
for its return. Internal SST/RT286 audio and native Intel GPU acceleration remain
unimplemented. D-Bus reports missing inotify support but runs its session socket.

The updated isolated 512 MiB executable/detach test also passes with volatile
runtime mounts (`build/hardware/usb-app-image-v4-retry-20261030/result.json`).
Its first attempt stopped in OVMF's USB boot lookup before the Brook loader;
the exact same image passed with fresh firmware variables on the bounded retry.
That pre-loader startup variance is distinct from the kernel acceptance checks.

### First offline physical boot: diagnostics and console resizing

The first offline Dell boot reached the desktop but reported missing
`/nix/profile/bin/waylandd`, `hardware-dbus` and `hardware-message`. This means
the application image was unavailable; it does not establish an application
compatibility failure. USB/controller initialization and image mounting need
physical diagnosis before Ethernet or offline apps can be accepted.

The original status programs wrote to inherited serial stdout, which is not
the Kernel Console ring. `run --console` now explicitly routes stdout/stderr
to serial and the visible ring. Ordinary output and redirected binary pipes
keep their existing behavior. The hardware payload opts in for its USB read
probe and finite IP-status program. Controller reset/allocation/start failures,
the initial USB device count and application-image mount results are visible
without a serial cable.

Kernel Console previously resized its frame while retaining a fixed content
buffer. It now reserves a screen-sized backing once, redraws the current
viewport on size changes, and publishes tightly packed viewport dimensions
for both software and GPU rendering. This costs one screen-sized allocation
while the diagnostic console exists, without allocations on each drag.

The next Dell photo established that the controller starts successfully but
the root-port scan enumerates zero devices. The missing application files and
IPv4 address are downstream of this failure, rather than independent package
or DHCP failures. The next driver corrects two root-port issues:

- Power unpowered ports, read back posted writes, and allow a shared 250 ms
  settling interval before reading connection state.
- Build PORTSC writes from persistent fields. PED is write-one-to-disable;
  copying it from a completed reset into the PRC acknowledgement disabled the
  port. Clear stale PRC first, then wait up to 1,000 ms of elapsed APIC time
  for fresh completion, with disconnect and inaccessible-register checks.

These semantics follow Intel's [xHCI specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf).
Disabled SuperSpeed links are reported separately rather than receiving this
USB2 reset path; warm-reset recovery remains unimplemented. Each root port's
raw status before power, after settling, and after a requested reset is shown
in Kernel Console. This will distinguish absent links from reset/enumeration
failures on the next physical boot.

The host register model exercises PED write-one-to-disable, stale completion,
timeouts, unplug and inaccessible registers. Full QEMU ECM acceptance passes
with the updated driver, including bidirectional TCP/UDP, USB storage during
traffic and continuing storage/keyboard operation after NIC removal. Evidence:
`build/hardware/root-port-ecm-20261030/result.json`. Physical acceptance of the
new root-port handling remains pending.

The following Dell boot reported PCI `8086:9cb1`, with all 15 ports at
`PORTSC=0x000002a0` both before power and after settling. Thus PP was already
set, but no connection was visible. Root-port fixes alone did not restore
physical discovery.

A narrowly gated Intel routing step now runs before any Brook USB enumeration,
only for `8086:9cb1` with the xHCI PCI class. Intel's
[9-series PCH datasheet, sections 16.1.36–39](https://www.intel.com/content/dam/www/public/us/en/documents/datasheets/9-series-chipset-pch-datasheet.pdf)
documents USB2 routing at D0/D4 and SuperSpeed visibility at D8/DC. The driver
preflights all four registers, preserves existing selector bits, and enables
only firmware-permitted bits within the documented fields. It enables
SuperSpeed first and verifies readback before switching USB2 routing. Failed
readback stops further changes; there is no speculative rollback. Permission
masks and write-once port-disable overrides are never written. Override values
are reported read-only, together with routing before/after/mask values.

Host tests cover unsupported devices/classes with no register access, missing
registers, denied permissions, reserved fields, unchanged routes, failed
SuperSpeed readback blocking USB2 writes, and partial routing without rollback.
QEMU's generic controller remains unaffected; full USB Ethernet/storage/HID
acceptance passes in `build/hardware/intel-route-ecm-20261030/result.json`.
Only the next physical before/after port evidence can establish whether routing
explains the Dell failure; `0x2a0` by itself does not prove it.

The routing payload restored physical discovery on the next Dell boot. Firmware
allowed additional USB2 selector bits and all four SuperSpeed terminations;
the driver read back the requested settings. Four USB devices were enumerated,
the FAT32 stick and application-image metadata mounted, and the Tecknet obtained
an IPv4 address. A boot-pinned hardware bridge attached successfully from the
development host. Logs, capabilities, memory/frame statistics and a UID-1000
structured guest command with separate output and collected exit status were
verified directly on the laptop. Private evidence is retained under
`build/hardware/dell-live-20261030`.

Physical USB file reads subsequently returned EIO, preventing the offline
session executables from loading. The remote probe confirmed exit 3 with
`USB_IO_OPEN: I/O error`; this alone does not identify the original transport
failure after the storage path is quarantined.

The following candidate fixes the xHCI >=1.0 TD packet-count field for large
MSC reads split at a 64 KiB DMA boundary. It publishes the first Normal TRB's
cycle only after the whole TD is prepared, requests short-packet events for IN,
and matches failures against every submitted TD token. Success still requires
the final token with zero residue; failures retain DMA/quarantine semantics.
The packet formula follows Intel xHCI section 4.11.2.4. Host tests cover packet
saturation, partial packets and matching a nonfinal token without discarding
unrelated completions. Full QEMU USB acceptance passes at
`build/hardware/msc-td-final-ecm-20261030/result.json`.

The first MSC error is now visible in the kernel log with its completion code,
residue, endpoint/slot, DMA boundary offset, tokens and command information.
This candidate still needs a physical read-integrity pass; TD defects are
established in the code, but their role in the observed Dell EIO remains to be
verified.

### Validated reset and graceful manual power-off

Power handling now separates graceful quiescence, reset and automatic power-off.
The FADT signature is `FACP`. Discovery checks firmware-memory ranges, table
lengths, RSDP/root/table checksums and MADT record bounds before using fields.
Readable regions are copied from the boot map rather than retaining its array;
explicit unusable RAM remains a distinct boot memory type and is rejected.
Checksums use a 256-byte streaming buffer, with no firmware-sized allocation.
Current discovery bounds are 1 MiB per SDT, 64 KiB per root table, 4 KiB per RSDP,
512 captured firmware regions and the bootloader's 128 GiB identity-map ceiling.
Invalid or unsupported tables disable the capability with a reason.

The initial reset backend accepts only an advertised FADT byte System-I/O GAS
(zero bit offset, unspecified/byte access size, valid nonzero 16-bit port).
These fields follow [ACPI 6.6 reset register definitions](https://uefi.org/specs/ACPI/6.6/05_ACPI_Software_Programming_Model.html).
The coordinator completes application/service cleanup, stops writeback, closes
write admission and checks sync before issuing the single selected write. A
returned/failed reset does not trigger another port; the quiesced kernel remains
available for diagnostics. The panic-oriented AP halt API is not reused.

Native soft-off still requires ACPI sleep-state evaluation/preparation and is
not implemented. Shutdown can nevertheless complete cleanup and sync, then
report **Safe to power off manually**. Unsupported commit leaves it ready;
restart can be supported independently. Explicit Q35 launch opt-in retains its
existing emulator power-off method. A stop-only Q35 launch explicitly disables
reset, preserving its host lifecycle policy even when firmware advertises it.
No `_S5` byte-pattern search or speculative hardware port fallback is used.

MCP bridge 0.11.0 adds `hw_power` with boot-pinned request IDs and separate
request/status/cancel/commit operations. `hw_doctor` returns the independent
power capabilities and reasons. Uncertain mutations are not replayed to obtain
an answer; observe the existing ID. Request descriptions belong to that ID,
including historical cancelled operations. Client tool discovery needs a reload
to expose newly added tool names; the standalone server CLI works immediately.

Validation includes actual parser tests for malformed/truncated firmware tables,
unsupported GAS, region lifetime and MADT bounds; mocked exact I/O dispatch;
no write after failed sync; manual-ready shutdown; reset-only capability; boot
identity rejection and uncertain-delivery recovery. Four native power/ACPI tests
and 119 Python hardware/VM/job/UI/lifecycle contracts pass. End-to-end stdio MCP tests boot
private copies of the offline image; QMP is used only for setup/evidence/cleanup,
not to trigger the guest reset. See `scripts/test_hardware_power.py`.

Physical reset and soft-off acceptance remain separate. The stick and scanner
remain pinned to the already validated offline payload until a new physical
copy is performed. Preserve that exact ELF; do not silently replace it when
building later power kernels.


Final power/launcher acceptance is recorded in
`build/hardware/hardware-power-v4-20261030/result.json`. Its first case launches
Mousepad using the installed catalog reference through `hw_launch`, observes its
real window, and lets the shutdown coordinator close it. The other cases verify
firmware reset without an emulator power opt-in and preserved Q35 stop-only
policy plus explicit emulator power-off. These cases passed through the actual
stdio MCP server. The frozen upgrade bundle is
`build/hardware/offline-power-20261030/probe/payload`; it is prepared on the host,
not yet copied to the powered-off stick. Scanner pinning stays unchanged until
that transfer.

For the next Ethernet control, cold-start the adapter on the Dell: disconnect
it fully, connect it before boot, then look for the finite IP banner. The host
comparison changed an already initialized adapter from vendor configuration 1
to ECM configuration 2; cold-start behavior remains unmeasured. If no IP appears,
capture configuration/notification/TX/RX evidence before trying vendor register
changes. This is a test hypothesis, not a claim that cold startup fixes receive.
