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

The physical TD candidate still timed out: READ(10), tag 177, LBA 107744,
16,384-byte IN transfer, MaxPacket 1,024, three submitted tokens (two data TRBs
and a Link), DMA low bits `e000`, completion-queue failure flag clear. The next
diagnostic profile adds `/etc/usb-msc-page-chunks`, limiting MSC data transfers
to 4,096 bytes while retaining a page-aligned bounce buffer. This distinguishes
single-TRB transfers from a TD spanning the ring Link. It is a control, not
proof that the multi-TRB path is repaired. The default remains 64 KiB.
Timeout diagnostics additionally show USBSTS, endpoint state/dequeue and the
submitted TRB contents. Host byte-I/O tests now exercise both 4 KiB and 64 KiB
capacities; the full offline QEMU read/IP/console acceptance passes for the
control at `build/hardware/msc-page-desktop-test-20261030/result.json`.

The physical 4 KiB control also failed at ring wrap: slot 6/DCI 3, one 4 KiB
data TRB following a Link, USBSTS `0x10`, endpoint state Running. The submitted
Link had CH set despite being between TDs. Transfer rings also initialized
their Link with C=1, allowing hardware to cross before the producer prepared
the next lap. A Link used later as a publication gate could consequently have
already been consumed. Endpoint output-context dequeue is not assumed to be
a live instruction pointer.

The next candidate initializes command/transfer Links with C=0 while initial
producer/DCS is one, inherits Link CH from its preceding transfer, and gates
the first ring element even when it is a Link. Host consumer-model tests cover
enqueue 254/255, split TDs, two full laps and polling during preparation. A
negative control restoring the initially-owned Link makes the regression test
fail, with core dumps disabled. The test establishes producer/consumer ownership
invariants; physical read acceptance remains required. Evidence is under
`build/hardware/msc-wrap-negative-control` and
`build/hardware/msc-wrap-desktop-test-20261030`.

The stick also intermittently failed in Dell firmware before any Brook code.
All eight payload hashes and the x86-64 EFI application were correct. A
non-modifying FAT check found only boot-sector byte 65's dirty bit. After
metadata backup, managed repair changed exactly that byte from one to zero;
the FAT check became clean and all file hashes still matched. The next firmware
boot succeeded only on its third attempt, so the dirty flag is not established
as the boot-failure cause. Preserve this as a separate pre-loader issue.

The fresh ring-ownership payload passed physical read acceptance on the Dell:
six byte-verified 8 MiB passes, final `USB_IO_PROBE_OK`, no transport error, and
the compressed application image mounted. Mousepad and Files subsequently
launched through the hardware bridge and registered visible windows. Terra's
semantic receipts are in `build/hardware/dell-msc-wrap-live-20261030`; this is
not a bridge screenshot or an independent visual-rendering check. The physical
control still uses 4 KiB MSC chunks. Restoring the 64 KiB default remains a
separate throughput/correctness test.

## Next: remote USB update and reboot

The user requested remote updates so subsequent driver builds can be tested
without moving the stick between machines. The first implementation is described below; physical acceptance is pending. Reliable physical reads now pass. Checked scratch-file writes/flush/fresh-readback
on the Dell remain the gate before activating a remote boot-payload update.

The first implementation should be an opt-in maintenance profile. Its updater,
network/debug service and dependencies live in initrd/RAM, and it does not mount
the compressed application image from USB. The USB provider is writable from
initialization, while the normal FAT mount remains read-only. Ordinary desktop
boots keep their existing permanent read-only device guarantee: never toggle
that guarantee beneath a retained compressed-image source. A future desktop
that can enter maintenance without a dedicated boot needs an explicit RAM
application source or another proven handoff mechanism.

The update transaction stages a bounded, complete kernel/initrd pair in RAM and
checks declared sizes and integrity before touching storage. Binary upload
frames carry boot identity, transaction ID, length and offset; binary bytes
must never enter the current line-command parser. An uncertain reply is
recovered by querying the same transaction, without repeating commit. The
existing boot token guards identity, not authentication; writing requires an
explicit opt-in maintenance authorization established during provisioning.

Before writing, quiesce filesystem consumers and require actual FAT binding
release, not merely namespace detach. Mount the update volume exclusively RW,
write only the inactive pre-created kernel/initrd pair, sync and flush, then
verify through fresh reads bypassing caches. Only then publish a checksummed
generation/selection record and flush again. Unmount successfully before using
the existing guarded restart. Keep networking and scheduling alive during
staging/writing; global shutdown happens afterward. Any transport failure stops
the transaction and retains quarantine rather than booting an incomplete pair.

The bootloader currently chooses a kernel path but always reads optional
`INITRD.IMG`; it needs required paired-slot selection and pairwise fallback.
Use two checksummed generation records and keep the bootloader itself outside
the initial remote-update scope. Preallocated slot files reduce FAT allocation
changes but do not make FAT or USB writes power-loss transactional. Missing or
corrupt payload fallback differs from recovery after a valid kernel hangs;
trial/confirmation or manual recovery remains a separate requirement.

Acceptance should cover interrupted/repeated upload, stale boot identity,
authorization failure, oversize and corrupted images, active filesystem
references, failure at each write/flush/readback/selection step, paired fallback,
and real firmware reset into the selected verified kernel/initrd pair. Until
these gates pass, the current bridge supports observation, execution and power,
not remote boot-media updates.

### Writable scratch gate prepared before boot-media updates

`scripts/prepare_hardware_storage_test.py` derives a separate writable test
payload from the accepted read-only desktop. It removes the read-only marker
and compressed-image mount configuration in the new initrd before boot. USB
is writable from initialization; the ordinary profile and its retained image
source are unchanged. This explicit test profile mounts the test volume RW,
unlike the eventual updater's normally-RO maintenance mount. Applications are
not started and no automatic write test runs.

The host preallocates a dedicated 1 MiB scratch file with a format stamp, nonce
and header checksum. `tools/hardware_storage_probe.c` opens only an existing,
single-link regular file of that size, rejects symlinks and mismatched headers,
and writes only after validation. It never creates, truncates or deletes files.
Preallocation is established by host provisioning, not proved by guest fstat.
Successful write/fsync explicitly requests fresh-boot verification; it does not
claim power-loss durability. Repeating the deterministic pattern is not an
independent proof of another write, so preserve the initial zero-data control.

Host negative controls cover nonce/header mismatch, symlinks, hard links,
wrong size and corruption. QEMU acceptance writes and fsyncs the provisioned
file, reaches checked shutdown, boots a new VM from the same private image,
and verifies every payload byte. It then checks that all eight protected files
retain their hashes and FAT remains consistent. Image hashing streams mcopy
binary stdout; FAT checking uses bounded RAM instead of another SSD image.

```
# In nix-shell, after building the current kernel/xHCI module:
python3 scripts/prepare_hardware_storage_test.py \
  --base-probe build/hardware/offline-msc-wrap-20261030/probe \
  --output build/hardware/storage-write-profile/probe
python3 scripts/test_hardware_storage_write.py \
  --probe build/hardware/storage-write-profile/probe \
  --output build/hardware/storage-write-check
```

Final evidence: `build/hardware/hwio-host-v2-20261030/result.json` and
`build/hardware/writable-storage-acceptance-v2-20261030/result.json`.
The candidate `build/hardware/writable-storage-test-v2-20261030/probe` has not
been staged on the physical stick. No physical USB write acceptance or remote
update capability is claimed yet. After updates are implemented, confirm the
selected payload generation as well as a changed boot identity; a kernel-only
build ID cannot distinguish driver-only initrd changes.

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

### RAM maintenance updater implementation (2026-10-30)

`prepare_hardware_update.py` combines the accepted read-only recovery payload
with the tested RAM scratch profile. The kernel hashes must match. The root
kernel, initrd, APPS.BRK and control data remain unchanged. Only the one-time
bootstrap replaces EFI/config and adds preallocated A/B slots. `UPDATE_SLOTS=1`
enables the new loader; otherwise legacy behavior remains. The maintenance
provider/FAT are explicitly writable from initialization; it does not mount
USB-backed applications. Normally-read-only remount/lease transitions are not
implemented or claimed by this initial profile.

`/boot/HWUPDATE.ELF BOOT_ID TRANSACTION_ID` requires the opt-in marker and a
provisioned random key. It accepts one separate TCP connection on port 1235,
checks key/boot/transaction, stages at most 8 MiB ELF plus 64 MiB initrd in RAM,
and verifies SHA-256 before returning `staged`. An explicit ACTIVATE frame is
required before any write. Only fixed existing slot/selection files are opened;
there is no create, truncate, arbitrary destination or bootloader-writing API.
A process lock rejects concurrent writers and remains after a failed write.
The existing boot-pinned guest-job receipt resolves a lost activation reply;
never resubmit an uncertain transaction under a new ID. The LAN protocol uses
a shared secret, not TLS; it is an opt-in trusted-LAN maintenance interface.

Canonical 128-byte records contain generation, slot, exact lengths, SHA-256
hashes and CRC32. Both firmware and updater use the same bounded ELF validator,
including when identifying which existing slot is active. A hash-valid invalid
ELF must not cause the updater to overwrite the older working slot. Equal
conflicting generations fail closed. Updater allocation/open/read/close failures
abort without selecting another slot; only successfully read, invalid content
permits fallback. Host tests inject newest-slot allocation and read failures and
verify that every slot/record remains unchanged. Payloads are written/flushed first,
reopened O_RDWR with a new FAT FIL (bypassing whole-file RO caches), and checked.
Only then is the other selection record written/flushed/read back. EFI verifies
the entire required pair again before loading either. Invalid/missing pairs
fall back together, eventually to the root recovery payload. Candidate buffer
allocation is fallible so resource failure can also fall back.

This updates **kernel and initrd FAT disk image**. Updating the standalone
compressed APPS.BRK image is still pending. It does not update the whole USB
raw image, partition table or EFI executable. A valid kernel that hangs needs
manual recovery; runtime trial/confirmation rollback is not implemented. Fixed
slots and checked flushes do not make FAT/USB power-cut atomic.

Evidence:

- `test_update_payload`: SHA-256 published empty/abc/million-a vectors,
  incremental hashing, CRC32, every single-byte record mutation and bounds.
- `test_update_slots`: production selection code with missing/corrupt pairs,
  malformed ELF, ambiguous records, capacity limits and allocation cleanup.
- `build/hardware/update-host-tests-v3-20261030/result.json`: real receiver,
  invalid authorization/boot, oversize/interrupted/corrupt/staged-only uploads,
  activation, corrupt and hash-valid malformed newest ELF, conflicting records.
- `build/hardware/update-vm-acceptance-v3-20261030/result.json`: the production
  RAM helper receives a network upload in QEMU, activates generation 2, completes
  checked shutdown, then fresh EFI/QEMU boots the distinct updated initrd ID.
  All non-slot files remain unchanged and FAT checks clean.

The prepared physical candidate is
`build/hardware/update-bootstrap-v3-20261030/probe`. Before its first activation,
run the existing nonce/header scratch refusal and write/fsync gate on the Dell,
restart, verify the scratch bytes, then test remote upload and guarded restart.
A different guest boot epoch and the new initrd payload ID are the acceptance
proof; an activation acknowledgement or kernel build string alone is not.

### Host update orchestration and first physical write gate

`scripts/hardware_update_device.py` starts the boot-pinned RAM receiver and saves
all source hashes, transaction ID and power request ID before mutation. Its
output directory is exclusive and cannot be reused. Activation and restart are
explicit flags. A restart requires a declared expected initrd payload ID; the
host observes a changed epoch, attaches a new session and runs HWSTATUS there
before reporting `boot_verified`. A lost activation reply leaves a durable
`needs_inspection` receipt and never triggers a restart. A lost reset reply is
observed, never replayed. Reuse the recorded IDs to inspect uncertain delivery.

Twelve dependency-injected state tests cover staged-only/activated operation,
single-ID restart, missing reset acknowledgement, unchanged boot, wrong payload,
lost upload reply, mismatched job receipt, stale session, unsupported reset,
existing receipt and invalid key. Actual end-to-end live reset passed in
`build/hardware/update-workflow-vm-20261030/result.json`; QEMU stayed alive while
the guest's validated ACPI register reset it and the new payload was confirmed.

The first physical maintenance boot was confirmed at 192.168.85.106 with payload
`maintenance-bootstrap-5501ac9ce3064efc92a4718e7ad76d71`. Wrong-nonce refusal and
initial zero-data verification passed. The 1 MiB scratch write reached fsync,
which returned EIO. The USB driver logged CSW command failure status1/residue0;
it did not yet include the original opcode or sense. No update activation was
attempted, and no kernel/initrd slot changed. Evidence is in
`build/hardware/dell-update-maintenance-20261030`.

This is a device durability gate, not proof that SYNCHRONIZE CACHE is
unsupported. The next driver reports original command/sense, distinguishes
transport from command failure, and parses current cache mode data. Only current
ILLEGAL REQUEST / INVALID COMMAND OPERATION CODE (05/20/00) plus an explicit,
complete WCE0 caching page permits treating unsupported flush as write-through.
Unknown/malformed/missing cache data, deferred errors and all media/transport
failures remain errors. Ordinary data I/O and CBW/CSW lengths stay exact; short
IN is restricted to single-data-TRB diagnostics at most192 bytes. A stall still
quarantines; no endpoint/BOT halt recovery is claimed. This policy is deliberately
stricter than Linux's broad compatibility defaults and shutdown handling:
[Linux SCSI disk implementation](https://github.com/torvalds/linux/blob/master/drivers/scsi/sd.c).

The diagnostic/policy candidate is
`build/hardware/update-bootstrap-v4-20261030/probe`. It includes the freshly built
xHCI module while retaining the exact accepted recovery kernel/root initrd.
`test_usb_scsi_cache` covers fixed/descriptor and deferred sense, malformed and
truncated caching pages, block descriptors, explicit WCE0/WCE1, unsupported vs
other errors, and narrow short-completion eligibility. The private QEMU scratch
write/flush/fresh-boot verification passes in
`build/hardware/msc-cache-write-acceptance-20261030/result.json`. Real SanDisk
cache reporting and corrected flush acceptance are still unverified.
The diagnostic candidate also passes the live guarded remote-update/reset
workflow in `build/hardware/msc-cache-update-workflow-20261030/result.json`.
After the physical fsync failure, checked shutdown stopped with EIO (request
`4a670abe9938438db6f809bc64957c2e`), so a clean physical sync is not claimed.
Only the existing scratch file was written; activation was never dispatched.

### Opt-in RAM application snapshots (not yet on the physical payload)

A desktop that retains its immutable compressed application image in RAM can
coexist with writable USB update slots. `/etc/boot-app-snapshot` selects this
boot-only route using one record: permitted source path, `/nix`, exact image
bytes and SHA-256. Allowed paths are the root APPS.BRK or the A/B update slot's
APPS.BRK. The initial limit is512MiB, in4KiB units; admission leaves256MiB of
initial free physical memory for indexes, kernel work and recovery. This is not
a guarantee of post-cache headroom. Metadata and decoded/file caches allocate
separately; acceptance records actual post-mount free memory.

The kernel streams and verifies a complete private snapshot, closes the USB
source, validates the compressed header and publishes an immutable RAM device.
Its write operation always returns EROFS and the lifetime-stable RO ioctl reports
one. Successful snapshots remain allocated for the boot lifetime, even if their
filesystem is later detached. Failed allocations, reads, hashes and private
mounts unwind. This is single-threaded boot setup, not a runtime snapshot API:
DeviceUnregister does not drain arbitrary live device references. Snapshot
failure leaves applications disabled and the RAM/network maintenance path
available; it never falls back to mounting a mutable USB image.

`test_ramdisk` checks immutable ops, writable regression behavior, overflow-safe
end bounds and both factory allocation failures. `test_immutable_image_snapshot`
checks metadata paths/limits, independent copy ownership, partial reads, EOF,
read/alloc/hash failures and admission headroom. Actual QEMU acceptance is in
`build/hardware/app-snapshot-qemu-v2-20261030/result.json`:

- The entire2.2MiB USB source image was overwritten and its namespace detached
  before a fresh ELF launch. The app then verified cross-block data, links,
  permissions, private COW and read-only filesystem behavior from the snapshot.
- Bad-hash, short-source and256MiB low-memory boots published no application
  image while the hardware network/job bridge remained usable.
- The512MiB positive boot had434036736 free bytes after mounting. This small
  fixture is not acceptance of the full physical desktop image yet.

The current Linux umount syscall supports MNT_DETACH only; this test does not
claim a strict umount. Source closure is before publication in the boot code,
and source overwrite makes the fresh execution test independent of namespace
visibility or a retained cached source file.

### Confirmed unsupported sync and bounded BOT status recovery

Physical v4 diagnostics confirmed current05/20/00 for opcode35. Its MODE SENSE6
DBD1/page08 query then completed its data stage but stalled the13-byte CSW receive
(cc6, residue13); the transport was correctly quarantined. This was not a new
payload write. Evidence: `build/hardware/dell-flush-diagnostics-v4-20261030`.

The next candidate follows the BOT status-stage recovery sequence: clear the
bulk-IN halt through EP0, reset that xHCI endpoint, set its dequeue to the known
software position after the failed TD, then retry only the CSW once. No CBW or
write is replayed, and timeouts remain failures. Matching includes the slot,
DCI and final TRB. Command completions must precede the retry. At ring index255,
normalization skips the Link, toggles DCS and leaves that skipped Link unowned
for the next lap. Recovery failure or another stalled CSW quarantines the device.
See [USB BOT1.0 sections5.3.3 and6.7](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf).

The compatibility query now uses current MODE SENSE6 all-pages3f with DBD0.
Diagnostic responses may contain padding; CSW-relevant bytes must actually have
arrived, and parsing ignores the padding. Ordinary I/O still requires exact
transfers and zero residue. Cache policy distinguishes reported WCE0/WCE1 from
**assumed write-through**: only a successful, complete all-pages response with
no caching page permits that narrower Linux-style default, and only alongside
current05/20/00 for the unsupported flush. Unknown/truncated/malformed data and
all other errors remain failures. Logs label the assumption explicitly; neither
it nor scratch readback proves safety against sudden device power loss.

`test_usb_bot_recovery` checks a successful retry, each failed recovery step,
second stall/error, no timeout retry, both cycles at indices0/1/254/255, skipped
Link ownership, original tag matching and padding/residue bounds. The frozen
candidate is `build/hardware/update-bootstrap-v5-20261030/probe`: accepted kernel
plus the same module ABI and new MSC driver only, excluding RAM-snapshot work.
Private QEMU scratch write/flush/fresh-boot verification passes in
`build/hardware/msc-csw-write-acceptance-20261030/result.json`. Actual stalled
endpoint recovery and mode reporting still require the Dell test.
The v5 live remote-update/guarded-reset workflow also passes in
`build/hardware/msc-csw-update-workflow-20261030/result.json`. The v4 diagnostic
run was quiesced, but checked shutdown again reported EIO due to quarantine
(request25d2f13469004dec98580653f4bb2bbc). No activation was dispatched.

### Remotely uploadable full RAM desktop

`scripts/prepare_hardware_ram_desktop.py` freezes a new kernel/initrd from the
accepted offline desktop and the provisioned maintenance key. It refreshes xHCI
and EEVDF modules, embeds RAM updater/status/scratch tools, selects the existing
root APPS.BRK by exact length/hash for a RAM snapshot and keeps home/settings
volatile. It neither copies another application image nor accesses physical
media. The accepted root recovery payload remains untouched. Automatic USB
stress reads are omitted from this desktop's startup; they remain explicit
acceptance jobs.

The candidate is `build/hardware/ram-desktop-update-20261030`, payload
`desktop-ram-apps-2ae017dcbfc34284ad59c7dcb4413c67`. Its manifest freezes both upload
hashes and the application dependency. `test_hardware_ram_desktop.py` installs
the exact pair in private slot B, then boots the full262602752-byte application
snapshot under QEMU. Only the test's outer GOP configuration differs from the
physical stick; the uploadable initrd is unchanged. Terra's semantic catalog
launches registered Mousepad and Files as visible windows. No screenshot was
obtained, so rendering was not visually verified. Checked shutdown passed;
1637068800 bytes remained free in the2GiB guest after mounting. Evidence:
`build/hardware/ram-desktop-qemu-20261030/result.json` and `apps-terra.json`.

Do not activate this on the Dell until v5 cache/stall recovery and physical
scratch write/fsync/fresh-boot readback pass. The current panic receiver still
uses the accepted old ELF. Before testing the new kernel, pin the scanner to the
candidate's frozen `kernel.elf`; do not point it at the mutable build output.
The RAM desktop enables future USB updates without a live mutable application
source, but the current upload protocol still updates only kernel/initrd.
Application-image upload needs a separate dependency-aware extension: protect
the selected pair's actual app source, bind a new image to the inactive slot,
verify that the uploaded initrd references that exact image, stage/hash before
writes, verify new app bytes before pair/selector commit and preserve root
APPS.BRK. Creation needs provenance and disk-full/interruption handling; arbitrary
existing files may not be adopted or deleted. Whole-device rewriting remains
out of scope.

### Read-only initrd dependency inspection groundwork

`tools/update_initrd_reader.c` uses an isolated, read-only configuration of the
vendored FatFs R0.16 parser against a bounded RAM image. It accepts only fixed
boot metadata paths, returns exact bytes without truncation, checks declared
FAT volume bounds, limits backend read traffic and preserves backend failures.
No host device or kernel mount is used. The small CLI accepts regular images
only; its build script copies unmodified vendor sources beside the separate
configuration. Calls must remain single-threaded; the active flag is not a lock.

`build/hardware/update-initrd-reader-v3-20261030/result.json` covers FAT12/16/32
long filenames, unchanged input hashes, oversize/missing metadata, invalid paths,
truncated/bad boot data, a bounded cyclic-directory case and the exact prepared
RAM desktop's real snapshot marker. A cyclic lookup returned MISSING under the
upstream directory bound; MISSING is not global filesystem-integrity proof and
must never authorize overwriting a potentially referenced application image.
Backend read limits do not prove a universal parser CPU-time bound; malformed
fixtures also have host timeouts. This foundation is **not integrated into the
upload server**, establishes no application dependency policy yet and changes
neither the staged USB nor the current kernel/initrd-only protocol. A future
application update must require present, parsed dependency metadata and the
slot/provenance/commit gates described above.

### Kernel-reported application snapshot readiness

The new kernel reports finalized boot state at read-only
`/proc/brook_app_snapshot` and in `bridge hardware caps` (`app_snapshot_api=1`).
The report distinguishes configuration from successful RAM publication, records
source closure, declared bytes, source path and a bounded error code; the proc
record additionally carries the expected SHA-256. Ordinary file-backed/disabled
boots never report snapshot readiness. `HWSTATUS.ELF` prints the versioned record
when available. The host doctor reports API support separately from readiness.

This closes an application-updater admission gap: the future helper can compare
present initrd dependency metadata against the actual RAM snapshot rather than
assuming a marker means the image was mounted. Boot state is written only in
serialized initialization and immutable after user scheduling starts. It does
not imply current window visibility or authorize an application-image overwrite
by itself. Existing physical v5 and frozen desktop candidates are unchanged.

Five QEMU cases pass in
`build/hardware/app-snapshot-readiness-20261030/result.json`: independent RAM
execution after source overwrite/detach, bad hash, short source, low memory and
unconfigured snapshot. Each checks the proc report and boot-pinned hardware caps
against actual publication. Eleven hardware-bridge contract tests pass,
including API-supported-but-not-ready and legacy-no-API behavior.

### First successful physical remote update

Physical v5 recovered the stalled CSW, completed MODE SENSE and **reported WCE0**
(mode1,68 relevant bytes). The assumed-write-through compatibility branch was
not needed for this SanDisk. The explicit scratch nonce/zero controls passed,
then1MiB write/fsync succeeded. A bridge-requested checked ACPI reset returned
with a different boot identity and the complete scratch pattern verified.
Evidence: `build/hardware/dell-csw-recovery-v5-20261030/scratch-result.json`.

The first actual network kernel/initrd update then passed: generation2, slot B,
checked restart, changed boot ID and expected distinct initrd identity. This
smoke test deliberately used the same accepted kernel while exercising both
slot writes, validation and selection. It is not yet physical acceptance of a
newly built kernel or the full RAM desktop. The complete durable receipt is
`build/hardware/dell-first-remote-update-20261030/workflow/receipt.json`.

The user reported that the stick was not recognized in its previous USB port;
swapping it with Ethernet made both work. Preserve that arrangement for these
comparisons and track the port/firmware-enumeration issue separately. The cause
is not established by this successful boot.

The host updater now accepts `--boot-timeout-s` (default120, range1..900) for
slower physical image loads. This changes only observation of the original
restart; it never repeats activation/reset. Thirteen workflow tests pass,
including a short configured deadline that still sends commit exactly once.

### Physical new-kernel RAM desktop update passed

The next network update installed a genuinely newer kernel plus the full RAM
application desktop into generation3/slot A, preserving the working maintenance
pair in slot B and the root recovery files. Checked reset returned with a new
boot identity and the exact expected initrd ID. Both HWSTATUS and boot-pinned
hardware caps report configured/ready/source_closed=true,262602752 bytes, error0
and the expected root-image SHA-256. Cache probing again recovered the CSW stall
and reported WCE0; no transport quarantine followed. Evidence:
`build/hardware/dell-ram-desktop-update-v2-20261030/receipt.json`, `doctor.json`
and captured boot logs. The scanner now uses the frozen new kernel with SHA256
`f1b966a95ec5c245c0c494bf4ed2caed7caa3ced3a5d267bc8c14773db67d085`.

The user observed a reboot taking over25 seconds. This boot newly snapshots and
hashes the250MiB application image before starting the desktop; no component
breakdown was measured, so do not attribute the entire delay to that copy.
Physical graphical-app visibility and the remaining DPI mismatch still need
review. The kernel/initrd network update/reset loop is physically established;
standalone application-image upload remains future work.

### DPI provider candidate and boot timing (2026-10-30)

The saved generation3 boot log reaches the scheduler at25570ms and fits the
wallpaper at25669ms, consistent with the user's observation of over25 seconds.
The next kernel logs timestamped boundaries around early driver loading, USB
filesystem probing, application-image mounting and network setup. These are
boot-only markers, not continuous profiling. A private QEMU run measured the
application-image mount at11064ms; this is not a physical Dell breakdown.

`waylandd` now separates logical surface geometry from physical presentation:
integer output scale, scaled native buffers and subsurface destinations in
both scene and software rendering, inverse absolute pointer coordinates with
fixed-point fractions, logical resize configures, scaled popup placement and
cursor images/hotspots. Raw relative mouse packets are unchanged. The
authenticated Xwayland client's output and native-window bridge retain scale1.
Client buffer scale and viewport source sampling remain independent of output
scale, so a2x client buffer is sampled directly at physical resolution.

`display-scale-test.c` exercises actual frontend handlers, CPU pixels, scene
rectangles, resize, negative input-region edges and wire events; the output
identity tests check advertised scales for native and attached Xwayland clients.
The complete Nix provider checks pass. Initial private QEMU acceptance caught
an asynchronous startup race: the requested200% setting had not yet been
applied when the provider read100%. Initialization now honors the restored
requested setting, and the test simulates that race. The first candidate was
withheld from hardware.

This initial provider fixes its scale at startup.100% and200% are the tested
targets; intermediate settings are rounded to an integer and live scale changes
require a provider restart. It does not add subsurface input routing or confined
pointer support. `prepare_hardware_ram_desktop.py --waylandd /path/to/waylandd`
can stage a tested provider in the initrd, retaining the root APPS.BRK image;
its interpreter and shared-library closure must already exist in that image.
Private QEMU acceptance is required before a hardware update.

Candidate2 (`build/hardware/dpi-desktop-update-v2-20261030`) passed private
QEMU launch/scale checks and checked shutdown at1920x1080 and at a requested
3200x1800. The latter actually selected2560x1600; both app windows fit that
mode. Receipts are `dpi-desktop-qemu-v2-20261030/result.json` and
`dpi-desktop-qemu-native-20261030/result.json` under `build/hardware`.
These two checks are semantic, not visual/input proof. The review harness now
provides a second QMP endpoint so screenshot/input inspection does not wait on
the monitor connection already held by its shutdown controller. The active
Dell remains on the previous accepted payload until handed back for update.

The user subsequently handed over the Dell for ongoing work and restarts.
Candidate2 is now physically accepted as generation4/slotB, with a new boot ID
and the expected payload ID verified. The250MiB immutable RAM snapshot is ready
with its USB source closed. Mousepad and Files both produce visible, doubled
client dimensions (1600x1200 and2048x1400) fitting the actual3200x1800 display.
Receipts: `build/hardware/dell-dpi-update-20261030/{receipt,doctor,apps-terra}.json`.
Root recovery files and the previous working slotA remain intact.

Measured Dell stages: early modules634ms, USB filesystem probing15ms,
application-image mount13659ms, network21ms. The scheduler starts at15262ms,
wallpaper at15351ms. This run was shorter than the previous25.57s observation;
the previous run lacked stage measurements, so that difference has no proven
cause. Detailed evidence is `dell-dpi-update-20261030/boot-timing.json`.
The scanner now pins candidate ELF SHA256
`fe33b74e86afae8baa46e9fbfd3547dbe958564fe495bd7e4dc3b628f7020539`.

The independent review monitor produced actual QEMU screenshots at2560x1600.
Terra observed readable Files/Mousepad content and text entered into Mousepad;
exact click-to-caret alignment was not independently proven. Files' status line
was partly clipped by the taskbar. That visual review exceeded its600s bound and
the harness terminated its private VM; it is not a clean-shutdown acceptance.
The two earlier semantic runs did complete checked shutdown. Native hardware
frame capture is still unavailable; none is claimed from these QEMU images.

Next requested milestone: software-rendered Quake2 on the Dell, using private
game data outside Git. First add a separate binary transfer helper for large
files and screenshots; MCP returns commands, receipts and artifact paths only.
Keep file-transfer staging authority separate from boot-slot activation in
HWUPDATE. Reuse the existing completed-frame producer through a bounded,
owner-bound userspace copy interface rather than exposing kernel addresses or
streaming mutable scanout memory. The kernel debug console's older font path is
also separate from the Wayland DPI correction.

Short-term DPI backlog: scale the built-in desktop pointer and its hotspot with
desktop DPI, and scale ordinary desktop pointer movement/provide sensitivity
settings. The user reports the pointer is barely visible and movement too slow
at native Dell resolution. Captured raw game deltas must remain independent.
Client-provided Wayland cursors are already presentation-scaled; that does not
fix the default kernel desktop arrow. Keep the binary-transfer milestone first.

### Binary transfer and completed screenshots (2026-10-30)

The prepared `bulk-desktop-update-v4-20261030` pair adds `HWBULK.ELF` and
kernel completed-frame copy API1. `hw_transfer` and
`scripts/hardware_transfer.py` launch one boot-pinned helper job, authenticate
over TCP1236 with the provisioned key/boot/transaction, then stream bytes over
that separate socket. MCP/stdout carry metadata, receipts and artifact paths;
file bytes never pass through JSON or base64. The durable host receipt retains
the original job ID after an uncertain reply; no operation is redispatched.

PUT writes only generated names beneath `/data/.brook-transfer`. Exact
length/SHA256, file flush and fresh readback precede explicit FINALIZE and
atomic `renameat2(RENAME_NOREPLACE)` publication. A completed artifact is never
overwritten, including a dangling symlink destination. Publication is recorded
before sending its final acknowledgement. GET accepts only completed staging
artifact IDs. Boot slots and activation remain exclusively in HWUPDATE.
Storage is current-boot RAM, not power-loss durability or a general USB writer.
The helper bounds each file at1GiB, each connection at10 minutes, and checks
reported filesystem free space before a PUT. The tmpfs quota can be smaller.

Root-only syscall0xB0E requests, inspects, reads and releases an immutable
completed-frame lease. Tokens belong to the calling process incarnation.
Metadata exposes dimensions, packed BGRA8 format, frame epoch/tick and size,
never a kernel address. Reads copy at most64KiB into kernel scratch under the
capture lock; user-memory copies occur after unlocking, protected by a kernel
operation guard. The helper promptly copies the frame into its own bounded RAM
and releases the30-second lease before network delivery. Capture is unavailable
post-panic; optical panic recovery remains separate.

The host verifies the full payload and final receipt before publication, then
converts BGRA to PNG locally. Frame results keep only PNG by default; diagnostic
raw bytes require `--keep-raw`/`keep_raw=true`. Hardware doctor reports helper,
capture and transfer limits independently of QEMU/host-desktop capabilities.

Two compatibility gaps were fixed during actual QEMU acceptance: tmpfs lacks
hard links, so publication uses the newly implemented no-replace rename policy;
the old global executable-open path could not run object-only tmpfs files, so
it now uses retained executable opening when available and never falls back
after a retained denial. Tests include racing publishers and object-only opens.

Evidence:15 real receiver host cases in
`build/hardware/bulk-host-tests-v4-20261030/result.json`, nine host workflow tests
including lost replies/no redispatch and default PNG retention, existing hardware
bridge tests, and `test_vfs_paths`. The exact kernel/initrd pair passed private
QEMU acceptance and checked shutdown in `build/hardware/bulk-qemu-v3-20261030`.
`bulk-qa/result.json` records file round trip, executing the transferred probe,
owner/root/bounds/EFAULT/stale checks, capture contention/expiry and acquiring a
new frame while another binary screenshot download is paused. Terra inspected
the generated PNG: coherent desktop/colors and legible console, with some long
console lines clipped by its viewport. Hardware deployment is the next gate.

Physical acceptance subsequently passed as generation5/slotA: changed boot ID
and expected initrd payload verified, with previous working slotB retained.
`build/hardware/dell-bulk-update-20261030/receipt.json` records activation/reset.
The native binary capture returned3200x1800 /23040000 packed BGRA bytes with
verified SHA256; the default result kept only its local PNG. Terra inspected
that actual Dell image and found coherent colors/layout without gross corruption.
The kernel console font remains small and some long lines clip, as already
tracked separately. Native screenshot and receipt are under
`build/hardware/dell-bulk-acceptance-20261030/screenshot`.

The same actual Dell then passed a2056458-byte upload/download round trip via
the MCP `call_tool` adapter. Both responses were metadata-only and under8KiB;
the downloaded SHA256 matched the source. End-to-end times were10.09s upload
and8.19s download, including job startup and polling—not wire-throughput numbers.
Evidence: `dell-bulk-acceptance-20261030/roundtrip-result.json` and the retained
MCP responses. The scanner uses the exact accepted bulk-payload kernel ELF.

### Native software Quake II (2026-10-30)

The existing static Brook Quake II port now runs on the physical Dell without
Mesa or LLVM. A fresh source build and a caller-supplied private `pak0.pak`
passed the private 8GiB QEMU test before binary transfer to the Dell's RAM home.
The engine and PAK are moved within `/data`, avoiding a second copy of the
176MiB PAK against tmpfs's quota. Neither paid game assets nor music are added
to Git or the distributable image. The engine runs as UID1000. Internal Dell
audio remains unavailable and is explicitly disabled for this test.

`dell-quake2-software-20261030/result.json` records the physical run; successive
binary captures show a changing textured 3D scene, weapon and HUD. Terra
inspected the actual native capture, and the user subsequently played the game.
This verifies gameplay rather than only a loading screen. The displayed FPS
in a screenshot is a single observation, not a sustained benchmark.

The binary phase for the native 3.66MB engine was5.53s and the184MB PAK273.93s,
including streaming, hashing and verification but excluding helper startup and
job polling. Private QEMU's PAK phase was6.75s. This isolates a substantial
physical-network throughput gap. Source review identifies one outstanding
2048-byte ECM receive TD and unprogrammed xHCI interrupt moderation as leads;
measure completion/IRQ timing before attributing the gap or changing queues.

The user's gameplay review found no mouse input. The old platform adapter
attempted to open `/dev/mouse`, a device Brook does not implement, and lacked
button-release handling. The revised adapter uses an owned native WM window,
its keyboard/button/raw-relative event queue and revocable pointer lease.
Click in gameplay captures; menus/focus loss release; Ctrl+Alt+Esc releases
without an immediate automatic re-grab. Initial capture clicks are consumed
so a kernel sequence fence cannot leave firing stuck on. Desktop DPI cursor
size and movement sensitivity remain separate from raw game deltas.

The host adapter test is:

```sh
cc -std=gnu99 -I tools/quake2/source \
  tools/quake2/source/in_brook_test.c -o /tmp/brook-q2-input-test
/tmp/brook-q2-input-test
```

It covers raw motion axes, complete button pairs, stale capture tokens, focus
and menu release, manual revocation, extended keys and native close requests.
Actual input acceptance is recorded separately under
`quake2-mouse-qemu-20261030`; physical mouse behavior still requires review.

The mouse-pass private guest reached the rendered scene and accepted a keyboard
`map base1` command. Its later shutdown remained in `Closing apps` for90s and
the harness terminated QEMU in cleanup. Do not count that run as a checked
shutdown pass; the earlier software-game test completed checked shutdown.
The failed shutdown log is `build/hardware/quake2-mouse-qemu.log`.

Terra's private QMP check subsequently confirmed camera rotation from a raw
relative x+120 event: fixed wall/beam edges shifted while the HUD and window
stayed fixed. Before/after images are `terra-before-motion.png` and
`terra-after-motion.png` beneath the mouse-pass test directory. Weapon clicks
and release shortcuts were not independently exercised in that guest; their
adapter behavior is covered by the host test. The final executable (SHA256
`6ffb7112581e25f1ba0584b8b712cce0bf6c425ac649a0cdd1c23d02a8f976a3`)
is installed on the Dell and launched into `base1`, with the earlier executable
retained as `quake2.before-mouse`. Physical mouse acceptance remains pending.
Installation/job evidence: `dell-quake2-mouse-install-20261030/result.json`.

### Native-window identity and shutdown follow-up (2026-10-30)

The earlier mouse-pass shutdown failure came from native Quake using the
per-window API, whose default deliberately records an unknown client: a shared
userspace compositor cannot identify an app merely by owning its buffer.
`WmApplyDebugAction(Close)` therefore refused that window, leaving shutdown in
`Closing apps`. This was an identity-policy mismatch, not evidence of a game
save prompt or a filesystem sync stall.

WM_CREATE_WINDOW flag bit2 now explicitly requests native self-origin. It
retains only the calling process's kernel-recorded group/incarnation/launch
identity; callers cannot supply another app's PID. The default remains unknown
for display-server windows, with Unix-peer registration still required.
Unrecognized flag bits fail with EINVAL. Quake requests native self-origin.

`tools/hardware_native_window_probe.c` and
`scripts/test_hardware_native_window.py` exercise the actual private guest:
ordinary UID1000, unknown relay has no close capability, explicit native window
has the correct client identity, semantic close reaches its event queue, and
the original process exits0 after destroying its window. Evidence is
`build/hardware/native-window-qemu-20261030/native-qa/result.json`. Existing
window-surface, pointer-capture and shutdown host tests also pass.

The candidate pair is frozen at
`build/hardware/native-window-update-v1-20261030`; the Dell remains on the prior
accepted bulk payload while this update is tested. No recovery slot or USB
application image was modified during preparation.

The same private guest then rendered Quake's demo with native client identity.
Quake was deliberately left running when the harness requested shutdown: the
cooperative close completed, application/service cleanup and sync reached
`ready`, and QEMU exited successfully. Both `result.json` and `shutdown.json`
in `native-window-qemu-20261030` confirm this corrected end-to-end gate. The
physical Dell has not yet booted this candidate kernel.

### Desktop pointer DPI follow-up (2026-10-30)

The built-in12x16 arrow was always drawn in physical pixels, and relative
mouse counts always advanced the desktop by one physical pixel. At200% DPI
this made both the pointer image and the distance travelled too small.

Compositor-owned arrows and their busy badge now follow applied desktop DPI;
client cursor images remain physical pixels to avoid scaling GTK/Wayland's
already-sized cursor twice. The same prepared frame/hotspot is used by CPU
save/restore and GPU drawing. Scaling is bounded by the existing64x64 capacity.

PS/2, USB boot mice and virtio relative mice now call `MouseMoveRelative` for
scaled desktop positioning. Fractional carry preserves small150% movements.
Explicit absolute positions (including tablets and capture-release warps) stay
physical and reset that carry. Position/bounds updates use one short IRQ-safe
lock so concurrent device updates do not lose motion. Input events still carry
the original signed deltas; pointer-captured games do not receive DPI gain.

`test_mouse_motion` exercises production mouse position functions, including
fractional carry, DPI changes, absolute warps, bounds, wide signed deltas and
concurrent updates. `test_cursor_composition` covers scaled dimensions,
hotspots, alpha pixels and capacity guards; existing pointer-capture tests
continue to pass. The RAM desktop preparation script stages the matching
virtio-input module as well as xHCI and EEVDF.

The frozen candidate pair is `build/hardware/dpi-pointer-update-v1-20261030`.
It includes the previously tested native-window cooperative shutdown fix.
Private QEMU input/visual acceptance and physical acceptance are separate gates;
the Dell's current game run has not been replaced by this candidate.

Private acceptance passed in
`build/hardware/dpi-pointer-qemu-20261030/pointer-qa/result.json`: ten raw
counts moved the desktop twenty pixels at200%, captured motion retained ten
counts, Ctrl+Alt+Esc restored uncaptured movement, and native cooperative close
exited0. The enclosing harness also completed checked shutdown. Terra inspected
the saved cursor crop: a coherent white arrow with black outline, approximately
22 visible pixels wide by32 tall (24x32 allocated), without obvious corruption.
Evidence image: `pointer-qa/desktop-cursor-crop.png`. These results do not replace
physical pointer-speed and external-device acceptance on the Dell.

### Networked desktop / audio / SSD-preparation goal (2026-10-30)

Work continues on `hardware/xps9343-desktop-audio` from main6311f468.
Order: actual Dell Firefox/Chromium/App Gallery browsing and package install;
internal SST/SSP/I2S plus RT286 audio with visible volume/mute and muted boot;
then read-only internal-SSD inventory and reviewed installation/boot tooling.
Internal SSD writes, repartitioning and formatting are outside this preparation
scope. USB recovery and accepted application/boot payloads remain available.

The DPI/pointer and cooperative-shutdown candidate was deployed using the
existing boot-pinned updater. Generation6/slotB booted the expected initrd;
generation5/slotA remains the prior accepted fallback. Root recovery and
APPS.BRK were not replaced. New bridge session
`b0a02b9629ca4fb4a9ad3082ffb9576b`, boot
`3ed9fa9697404d1c9ac8a3d66771b099` verifies the262602752-byte immutable snapshot
ready/source_closed with error0. Evidence:
`build/hardware/dpi-pointer-native-update-20261030/receipt.json`, `doctor.json`
and `screenshot/receipt.json`. The old Quake job was identified and terminated
before reboot; its volatile game installation no longer exists after reboot.
The scanner now uses the exact DPI candidate ELF. Physical pointer feel remains
a separate user observation; a successful boot does not prove that acceptance.

`nix/hardware-network-packages.nix` builds Firefox150.0.1, Chromium148.0.7778.96
and App Gallery alongside the existing small desktop. It reuses the tested
browser wrappers and the runtime Mesa/LLVM subset, uses a native Wayland/CPU
route, sets the actual session-bus address and supplies explicit application
entries. The accepted small profile remains unchanged. The candidate profile
built successfully; all four launcher scripts pass `bash -n`. This is package
preparation, not yet browser acceptance on QEMU or hardware.

Measured closure:434 store paths,2893148043 regular-file bytes. Chromium accounts
for700500611 bytes, Firefox377666699, Mesa264120497 and runtime LLVM194783344.
Measurement: `build/hardware/network-profile-size-20261030.json`. A writable
ext2 image just below FAT32's4GiB single-file limit can hold this closure with
bounded installation headroom; calculate metadata and free blocks before
publication. Do not raise the current512MiB immutable snapshot cap blindly.

App Gallery's installer writes `/nix/store` and `/nix/profile`; neither can be
writable in the accepted compressed `/nix` mount. Astra reviewed the current
source and recommends a dedicated, fixed-size FAT-file-backed ext2 device.
This preserves the tested ext2 package semantics without formatting the stick,
using a whole multi-gigabyte RAM disk or implementing a general overlay FS.
Seed the entire `/nix` namespace: an empty writable mount would hide the base.
Materialize a writable profile, installer/index and a user-owned persistent
home directory; retain the accepted immutable desktop as recovery.

Required implementation boundaries before hardware use:

- Typed exclusive uncached RW FAT backing lease; reject conflicting handles
  and later writes, truncation, unlink, replacement or mappings through aliases.
  Ordinary VFS_O_STREAM stays read-only. A resize pin alone is insufficient.
- Block callbacks use an already-retained file and caller-local offsets. No
  namespace lookup/open/close inside ext2/FAT I/O. Keep the lock order
  namespace -> ext2 -> FAT -> USB and defer lower release outside those locks.
- Fixed image bounds, exact transfers, sticky write errors, write-behind off
  initially, and checked ext2 -> FAT file -> USB flush ordering.
- Bind a fresh device slot once; do not replace live ext2 bindings. Keep the
  device/backing for the boot lifetime initially.
- A separate bounded streaming provisioner writes a new dedicated filename,
  hashes, flushes and fresh-read verifies it before marking it usable. Current
  RAM-only bulk staging quotas/timeouts cannot provision this multi-GB image.
- QEMU must exercise the actual USB/FAT/ext2 stack: random writes, executable
  mmap, installer activity, conflicting aliases, injected I/O/flush failures,
  checked shutdown, reboot persistence and offline checks of both filesystems.
  Accepted recovery/APPS/update-slot hashes must remain unchanged.

No writable backing device or expanded USB store is implemented/deployed at
this checkpoint. This design is the next concrete dependency for Gallery
installation and persistent browser profiles.

### Exclusive FAT image device primitives (2026-10-30)

The next-store backend is implemented, but not yet connected to a boot marker
or deployed on the Dell. `fat_image_device.cpp` creates an unpublished,
fixed-size device from a privately owned uncached RW FAT vnode. Its lease pins
that vnode/mount; existing aliases prevent admission and subsequent ordinary
opens (including case/SFN/LFN aliases), reads, writes, resize, replacement,
unlink and mmap are refused. Ordinary VFS_O_STREAM remains read-only. Published
devices are boot-lifetime; unpublished cleanup checks registration, flushes and
keeps failed state pinned. Ext2 drive slots now reject foreign rebinding.

The fixed image size is512-byte aligned and below FAT32's single-file limit.
Device I/O is byte-addressed: existing ext2 needs4-byte indirect-map reads and
128-byte inode writes. Each exact FAT transfer is at most64KiB and holds the
FAT mutex across seek and buffered partial-sector RMW. Multi-chunk requests are
not atomic; upper ext2 serialization supplies ordering. All read/write/flush
failures remain sticky, including errors observed through ordinary file,
directory or mount-wide sync paths. No pathname lookup occurs in block I/O.

A retained64KiB extent map avoids repeated full FAT-chain seeks. The new
`f_object_image_map` validates at most the declared file's cluster count,
requires an exact terminal EOC, rejects bad/reserved/short/excess/cyclic chains,
and stops immediately when its extent capacity is exhausted. It deliberately
does not use upstream CREATE_LINKMAP's potentially unbounded chain traversal.
Kernel FatFs fast seeking is enabled; the object adapter remains configured
for external locking, FAT16/FAT32 and non-exFAT operation.

Validation:

- Production FAT16/FAT32 fixtures exercise alias exclusion, allocation and
  admission rollback, mapping policy, random byte/sector I/O, small partial and
  sector-crossing writes,64KiB boundaries, adjacent concurrent partial updates,
  registration guards and cleanup. Injected read/write/file-sync/mount-sync
  errors remain visible to later I/O and release.
- Timed private child fixtures cover self/two/longer cycles, short/excess/bad
  chains and extent overflow, including8192 FAT32 fragments. Failed admission
  publishes nothing and frees provisional state.
- `test_fat_image_ext2` runs production VFS -> ext2 -> image device -> FAT16/32
  against ordinary host images. Create, indirect-block data, partial writes,
  fsync, lower flush failure and unmount/remount persistence pass. Extracted
  ext2 and containing FAT pass offline checks. Its host file-cache shims bypass
  cached mmap; actual executable mmap and USB transport still need QEMU gates.
- All161 host tests pass in12.55s; kernel build and both compiled entry-state /
  scheduler-start mutation audits pass. Logs are under
  `build/hardware/fat-image-*20261030*`.

Astra's review caught and resolved missing sync-error latching, the unbounded
stock map builder, and the initially incorrect sector-only Device interface.
Next: boot admission/fallback, complete writable-store builder and verified
streaming provisioner, followed by actual USB/QEMU restart and mmap gates.
No new USB store file or internal-SSD writes occurred in this pass.

### Priority addition: internal display power (2026-10-30)

The user requests panel/backlight power control as a priority; full system
sleep/suspend remains deferred. After checkpointing the storage primitives,
inspect the Dell's firmware-established display/backlight route before adding
manual off/on and idle blanking with keyboard/mouse wake. Keep the network
bridge usable while the display is off, and force the backlight on for optical
panic diagnostics. This is a display-control subset, not native Intel3D work.

Initial primary references are Linux's MIT-licensed Intel
[backlight implementation](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/display/intel_backlight.c)
and [register definitions](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/display/intel_backlight_regs.h).
They expose generation-specific PWM and panel AUX paths; neither the active
Dell route nor the relevant register state has been verified yet. Begin with
read-only PCI/register inventory. Do not infer a working DPMS capability from
framebuffer blanking or claim complete panel/link shutdown from backlight-only
control. Full ACPI sleep and speculative display-engine reinitialization are
out of scope for this addition.

### Read-only boot PCI inventory (2026-10-30)

`bridge hardware pci CURSOR COUNT` exposes the immutable initial PCI scan in
pages of1..16 entries. It reports BDF, PCI IDs, class/revision/header, command,
subsystem IDs for type0 headers, and the six raw configuration BAR dwords.
For bridge/non-type0 headers, dwords beyond the header-defined BARs are not
resources and must not be interpreted as such. The256-entry bound reports both
stored and seen counts plus truncation. No runtime config-port access, BAR
sizing writes or driver ABI changes are introduced by the debug command.

Host parser/pagination regressions pass. The frozen candidate is
`build/hardware/display-inventory-v1-20261030`; private QEMU acceptance is in
`display-inventory-qemu-20261030/result.json` with six devices cross-checked
against the serial scan, bounded pagination/invalid-limit checks and successful
cooperative shutdown. Physical deployment/identification remains a separate
gate. This is preparation for display/backlight controls, audio and read-only
SSD inventory; it does not implement any display power transition.

Physical inventory deployment passed: generation7/slotA, payload
`desktop-ram-apps-856e39e666704004b1f8cac9f39a4b77`. Generation6/slotB remains
accepted fallback; root recovery and APPS.BRK are unchanged. Evidence:
`build/hardware/display-inventory-native-update-20261030/receipt.json`,
`pci.json`, `doctor.json`. Session `24bd2e94a5d34326954306c81554f127`, boot
`e10cc6bbc79c437692c0b537412a9771`. The scanner is pinned to the exact inventory
ELF by `brook-panic-receiver-display-inventory-20261030.service` (HTTPS200).

Actual relevant PCI functions (no controller reconfiguration):
- GPU00:02.0 Intel8086:1616 revision09, subsystem1028:0665; memory decode
  already enabled, BAR0 raw `f6000004,00000000` (MMIO base0xf6000000).
- Audio00:03.0 Intel8086:160c and00:1b.0 Intel8086:9ca0, command0 at boot;
  this PCI observation does not inventory ACPI-enumerated SST/I2C devices or
  establish the internal speaker codec routing.
- SATA00:1f.2 Intel8086:9c83, class01:06/prog-if01, BAR5 0xf7422000;
  no disk sectors or partition tables were read/written by this inventory.

Next display gate: read only the documented Broadwell/LPT backlight register
state and establish whether firmware uses CPU PWM, PCH override or another
route before enabling any manual off/on operation. Keep system sleep deferred.

### Internal display/backlight control candidate (2026-10-30)

The provider is deliberately narrower than a native graphics driver: the exact
Dell1028:0665 / Intel8086:1616 GPU plus8086:9cc3 PCH, memory decode already on,
with no BAR-sizing writes. It maps only the two register pages UC/NX after
rejecting described RAM overlaps. These borrowed MMIO mappings are boot-lifetime.
It reads firmware CPU/PCH PWM controls and duty/period without writes at boot.
Only an enabled, plausible CPU PWM or explicit PCH override route is admitted;
frequency, polarity, enable bits and routing remain untouched. The operation is
zero PWM duty / restore original duty, not eDP-link power-down or system sleep.
Primary implementation reference: Linux's MIT-licensed
[intel_backlight.c](https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/i915/display/intel_backlight.c)
and [register definitions](https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/i915/display/intel_backlight_regs.h).

Boot-pinned `Hardware.command` accepts:
- `bridge hardware backlight status`: firmware/current registers, availability,
  logical on/uncertain state, idle timeout and restore deadline.
- `bridge hardware backlight off 10`: ten-second timed restoration test; allowed
  duration0..60, where0 stays off until input/on.
- `bridge hardware backlight on`: restore the admitted original duty.
- `bridge hardware backlight idle 300`: five-minute idle timeout;0 disables it
  and restores the backlight. Maximum86400seconds.

The production controller observes the existing input publication sequence from
normal compositor context, waking on input without doing MMIO in an input IRQ.
Manual control samples input before the MMIO operation, preserving input that
arrives during a transition. Panic restore uses no normal lock and atomically
published transition ownership; it restores only the still-compatible firmware
route. Failed readback enters uncertain state and forces restoration/idle-disable
on the next tick. External duty/routing changes are refused, including otherwise
idempotent commands; a failed command cannot reset the idle policy.

Astra reviewed route/period interpretation, ownership and wake/failure paths;
the idempotent-duty finding is fixed with a regression test. Production core
host tests cover CPU/PCH paths, preserved non-duty bits, invalid firmware,
external writers, readback failures, panic restoration, timed/manual/input wake
and idle reset/disable. The debug parser and compositor capture tests also pass.
Evidence: `build/hardware/backlight-host-build-20261030.log` and
`backlight-kernel-build-20261030.log`. Private QEMU boot and checked shutdown pass
in `backlight-qemu-20261030/result.json`; its unsupported VGA device is never
mapped, and power/idle mutations are refused. QEMU does not emulate this physical
PWM path; those results do not prove the Dell panel switches off.

Frozen candidate: `build/hardware/backlight-v1-20261030`. Automatic idle remains
disabled by default until physical route/off/wake acceptance; no persistent
settings or full suspend/resume behavior is claimed by this checkpoint.

The candidate has now boot-verified on the Dell as generation8/slotB, payload
`desktop-ram-apps-cfe9c5e5b9424c4bbe7794fc5145cb85`. Generation7/slotA remains
accepted fallback; root recovery and APPS.BRK were not replaced. Session
`f061048e48004232818cd055acec5d46`, boot
`3bdb4219ee864479a56326a87a483714`. Scanner unit
`brook-panic-receiver-backlight-20261030.service` uses the exact frozen ELF.

The actual firmware state is PCH override: CPU control0xe0000000, PCH
control0xc0000000, CPU/PCH data both0x03a903a9 (period/duty937). The first
status read matched the boot snapshot. One timed `off 10` reached zero PCH
duty with the period/control fields intact, network doctor stayed reachable,
and the controller restored every original register after10.158seconds.
A separate `idle 2` compositor-policy cycle then reached off and was restored
with `on`/`idle 0`; automatic idle is left disabled. Evidence is in
`build/hardware/backlight-native-update-20261030/receipt.json`,
`backlight-before.json`, `timed-cycle.json`, `idle-cycle.json` and `doctor.json`.
These are register/transport acceptance; physical panel darkness and waking
from the built-in keyboard/touchpad are still pending user observation.

### Writable network-desktop image builder (2026-10-30)

`scripts/prepare_hardware_store.py` replaces the legacy mounted-image assembly
for this candidate. It creates only a new regular-file output directory/image;
no FUSE mount, physical device, stale-process killing or image replacement is
performed. Build inside nix-shell with the already built pinned profile:

```
python3 scripts/prepare_hardware_store.py \
  --profile build/hardware/network-profile-20261030 \
  --output build/hardware/network-store-NEW
```

The complete434-path closure includes Firefox, Chromium, App Gallery and the
offline desktop. Profile directories are real writable directories and its
file exports link to existing store files; directory cycles and references
outside the closure are rejected. Static nix-install/nix-fetch/nix-search and
nar-unpack are rebuilt from source. The current checked-in package index and a
seeded installed-package manifest are included. Package/profile/cache/launcher
mutation directories and `/nix/home/brook` belong to UID/GID1000; prebuilt store
contents and package helper executables belong to root. Inode-number ownership
normalization is verified by reopening the image read-only, avoiding unsafe
filename interpolation into debugfs commands. Package data stays under its
original absolute Nix store paths. Builder inputs and image hashes are recorded;
filesystem timestamps are not claimed to make the image bit-for-bit reproducible.

Artifact: `build/hardware/network-store-v2-20261030/STORE.IMG`,4026531840bytes,
UUID `cf16f1ae-68c5-5fac-89e9-382b8af5cd5c`,1058750464bytes free. Ext2 geometry
is4096-byte blocks/128-byte inodes, compat0/incompat2/rocompat3. Clean `e2fsck -fn`
passes. Read-only namespace validation checks51692paths and5019profile file
links, writable profile/store/cache/home directories, and all four freshly built
package tools have no ELF interpreter. The extracted nar-unpack passes its four
existing archive tests. Evidence: `manifest.json`, `e2fsck.log`, and
`validation/{result.json,nar-tests.log}` inside that artifact directory. Whole
closure build scratch is removed after success; failed v1 stopped during Nix
metadata evaluation and contains no image. No image has been provisioned to the
Dell or claimed to execute there yet.

Astra's boot-admission review identifies a necessary durability gate: the current
ext2 driver does not validate feature masks/clean state or mark mutable sessions
dirty/clean. Proposed narrow image-specific lifecycle: strict UUID/size/features/
geometry validation, durably clear s_state before any mount/use, and set it clean
only after full home+/nix admission, quiescence, stopped writeback, closed device
write admission owned by the shutdown worker, and successful global VFS sync.
Checked two-byte clean marking plus backing flush must complete before Ready.
Failed admission or interrupted use remains dirty and requires an offline check;
never clear it merely because immutable fallback later shuts down cleanly.
Existing ext2 superblock updates modify separate free-count fields and do not
replace s_state. This plan is not implemented by the image-builder checkpoint.

Next: boot-only image admission/home bind, strict header and clean-session
handling, bounded direct USB streaming provisioning (not RAM staging), then
private USB-QEMU execution/mmap/COW/install/restart tests before Dell activation.
Successful writable admission must explicitly skip compressed mounting. The
immutable file-page-cache reclaimer is unrelated: ext2 regular-file vnodes use
cacheId0 and bypass it; ext2 has a separate bounded block cache. Failed namespace
cleanup must leave maintenance mode, not
silently stack another `/nix`. USB capacity was measured on the Dell as30307041280
bytes available; the constraint is FAT32's single-file limit plus the247967744-byte
free RAM `/data`, not total USB capacity.

### Physical backlight acceptance and writable boot lifecycle (2026-10-30)

The user physically confirmed that the Dell panel went dark and woke with a
keyboard press. Runtime idle blanking is now five minutes. The final register
sample followed additional input, so it does not independently measure which
input preceded the timed restoration; the physical observation is the wake
acceptance. Evidence: `build/hardware/backlight-live-observation-20261030/cycle.json`.
Touchpad wake has the same input-sequence path but has not been separately
physically reported. `/etc/display-idle-seconds` now provides an optional strict
0..86400-second boot policy; absent configuration remains passive. The writable
candidate preparer includes300seconds. This boot policy has not yet replaced the
accepted Dell generation8 payload.

`writable_desktop.cpp` now implements the image-specific lifecycle described
above. It checks the boot record and matching USB `STORE.OK` identity, a clean
supported ext2 header and group layout, then acquires an exclusive FAT file
backing. The two-byte dirty marker is written and flushed before publishing
`/nix`; the validated UID1000 home is bound at `/data/home`. Existing application
or home mounts refuse admission rather than being overlaid. A failed private
mount must be fully retired before immutable fallback is allowed; uncertain
cleanup leaves maintenance mode. Once ext2 is bound, its device/lease remains
pinned until reboot. Owned vnode references are dropped with `VfsRelease`, not
the final-destruction `VfsClose` operation.

Checked shutdown closes device write admission, stops writeback, and performs
global VFS sync. Only that successful shutdown worker can write and flush the
clean marker. A failed or unarmed session cannot be marked clean by fallback
shutdown. This is a checked-session completion marker, not a substitute for
filesystem consistency checking after an interrupted session.

`scripts/prepare_hardware_writable_desktop.py` freezes a kernel/initrd, preserves
the RAM application recovery snapshot, and records the expected mutable-image
UUID/size. `scripts/test_hardware_writable_desktop.py` boots a private USB-style
FAT disk with the ext2 file, using8CPUs/8GiB and software VGA. It never accesses
physical disks or the host desktop. Its interactive review and storage acceptance
are separate results.

The v4 private-QEMU run passed UID1000 home writes, cross-page MAP_SHARED
writeback/msync/fsync, forked MAP_PRIVATE isolation, exclusion of ordinary opens
against the leased image, checked shutdown, fresh-process reboot and persistence,
a second checked shutdown, and host `e2fsck -fn`. Evidence:
`build/hardware/writable-desktop-qemu-v4-20261030/result.json` and `e2fsck.log`.
Host lifecycle tests cover22 admission/failure scenarios; the strict planner,
shutdown owner, write admission, debug capability, display policy and real
FAT/ext2 backend tests also pass. Earlier private v1-v3 failures are retained as
evidence; v4 fixes the namespace case and vnode-lifetime defects they exposed.

Both Firefox and Chromium rendered HTTPS in the writable image; Chromium cold
startup took minutes. Gallery search and install dispatch worked, but galculator
installation failed: the checked-in index points to an unavailable hash, and
nix-fetch cached its HTTP404 body as metadata. Full desktop acceptance is therefore
**not passed**. Refreshing a pin-coherent index, validating HTTP/cache responses,
package installation/launch, browser downloads and quit/relaunch, bounded direct
USB provisioning and actual Dell execution remain required. No writable image
has been activated on the Dell, and no internal SSD writes are authorized.

The complete164-test host run passed163tests in parallel; `test_audio_delivery`
hit its15-second timeout under that load and passed in10.04seconds when rerun
alone. Both outcomes are retained in `writable-desktop-all-host-20261030.log` and
`writable-desktop-audio-recheck-20261030.log`; this is not reported as an entirely
clean parallel-suite result.

### Package metadata and launcher refresh corrections (2026-10-30)

The installer failure above had two independent causes. The old index's
galculator hash returned404; evaluating Brook's pin instead gives
`vf2yfk2x5qvgxm611mn2lhh35g09zgpv-galculator-2.1.4`, whose narinfo is available.
The generator now uses one explicit pinned x86_64-linux/allowUnfree=false package
set for both evaluation phases and writes provenance plus an index digest. The
refreshed21674-entry index is checked in. The hardware-store builder checks that
provenance against the current pin and includes it in the image identity.
Availability of every indexed archive is not implied by pin coherence.

nix-fetch now rejects HTTP errors for metadata and archives, validates bounded
complete metadata against the requested store hash before caching, removes
invalid legacy cache entries and retries them, and uses unique cache temporary
files. Parallel prefetch writes only private staging files; any transport failure
discards the batch, including peers that succeeded. `--fail-early` prevents a
later successful URL from hiding an earlier failure. Validated metadata is
published only after successful transport. This remains format/identity
validation, not a new NAR signature-verification implementation.

The existing C regression and `tools/nix-fetch/test_http.py` pass. The latter
uses real curl and a loopback HTTP server for404/500, a poisoned existing cache,
wrong hash, incomplete/oversized/NUL metadata, oversized-cache recovery,
archive404 without installation publication, an interrupted multi-URL batch,
successful batch publication and concurrent cache writers. Astra reviewed both
the implementation and regression coverage. Evidence:
`build/hardware/nix-fetch-{regression,http}-20261030.log`.

The v5 image successfully installed galculator through Gallery over HTTPS and
opened the actual calculator through brook-run. Its checked storage restart
and e2fsck gate passed. The remaining menu-refresh failure was traced to an
absent `/boot/SHORTCUTS`: first boot allows that source to be missing, but later
refreshes retain the old snapshot when it disappears. Prepared desktop initrds
now include an empty directory. The collector also preserves hardware-specific
core-app wrappers after an installation and supplies galculator's GUI runtime.
The real collector regression passes. These final menu fixes are being checked
in the v6 candidate; full desktop/Dell acceptance remains pending.

Direct provisioning will use a separate root one-shot fixed-file helper, sharing
the updater's boot/key/transaction framing and exclusion lock. It must stream
straight to a new USB STORE.IMG, without replacing any existing file or staging
it in RAM; check exact size/hash and file sync; close and reopen RW without
truncate for uncached FAT readback; recheck size/hash/header/GDT; and publish
STORE.OK last with no-replace semantics after explicit finalization. On uncertain
failure retain the incomplete image without a ready marker, rather than deleting
or retrying by pathname. Preserve APPS and recovery/boot-slot hashes. Start with
uncompressed64KiB chunks and progress timeouts; compression can follow. The
existing600-second RAM bulk-uploader deadline is not adequate for a multi-GiB
USB provisioning/readback operation.

The v6 private-QEMU review now passes actual Gallery installation, automatic
launcher-catalog refresh, launch from the newly installed Galculator entry and
keyboard interaction (`2+2=4`). After checked shutdown and a fresh QEMU process,
the installed launcher entry and executable remain present and launch again;
the calculator again renders4. The refreshed menu retains hardware wrappers for
Files, Settings, Mousepad and Gallery. Evidence:
`build/hardware/writable-desktop-qemu-v6-20261030/review/` and `reboot-review/`.
This is application installation/persistence acceptance in QEMU; browser
interaction/download/relaunch and actual Dell execution remain separate gates.
The final v6 storage result also passes both checked shutdowns, clean-marker
finalization and `e2fsck -fn`; see its `result.json` and `e2fsck.log`.

The read-only `tools/hardware_store_validate.cpp` adapter exposes the same strict
record/header/group validation used by boot to a future C provisioner. It also
checks the file type and exact extent and handles interrupted/short reads. A
static musl build passed against the prepared v4 STORE.IMG; the regression also
rejects a mismatched UUID, invalid descriptor, dirty state and truncated size.
This checks admission metadata only: exclusive ownership, complete SHA256
readback and filesystem consistency remain separate responsibilities. The direct
USB transfer helper and its QEMU/physical acceptance are still outstanding.

The unintegrated `HardwareStoreTransaction` core now creates a new fixed-name
image exclusively, writes bounded chunks, hashes the transfer, syncs and reopens
RW, checks boot metadata and hashes the complete readback. Finalization creates
and syncs a temporary marker, publishes it without replacement, syncs the
directory and checks the marker. Its monotonic publication/durability status
makes a failure after rename distinguishable from one before publication. The
caller must retain the directory FD and shared update lock throughout. Static
musl host tests with a16MiB real ext2 image pass successful publication, existing
and symlink collisions, bad hash, and injected failures immediately before and
after marker rename. Astra reviewed this component. The network receiver,
exclusive access enforcement, long-running transfer timeouts and QEMU/native
FAT acceptance are not implemented yet; this is not a deployable updater.

`hardware_store_receiver.cpp` and `hardware_store_protocol.py` now provide a
host-tested one-shot stream into the transaction core. It checks the fixed USB
mount is writable FAT, distinct from `/media`, and has sufficient free space;
authenticates the supplied key/boot/transaction; shares the updater lock; and
retains that lock on failures after image creation. It bounds receive progress
and total transfer time. Brook TCP currently may wait up to its existing30-second
ACK timeout during a reply even with MSG_DONTWAIT; the application deadline is
therefore not a strict bound on a send already in progress. The helper's boot
binding depends on the existing boot-guarded job launcher, which must be used by
its future workflow.

Static musl host tests with private loopback and a16MiB ext2 image pass successful
raw and host-client transfers, wrong key/boot, hash failure, interruption,
existing marker and withheld finalization. Test mode bypasses the production
FAT mount/device checks; these must be tested in QEMU. The receiver is not yet
included in an initrd or wired to an MCP workflow, and no physical deployment
has been performed. Native file ownership/exclusion, lost reply, write/sync
failure and QEMU checked-boot acceptance remain required before deployment.

The receiver is now packaged in the candidate RAM initrd and a boot-pinned host
workflow persists the transaction ID before dispatch, validates the entire source
and record size before starting the receiver, and never automatically replays a
lost mutation. A private QEMU FAT test first proved that insufficient capacity
is rejected without a marker. A larger private FAT then reached the production
create call, which returned `EOPNOTSUPP`; no image or marker was created. A
separate guest diagnostic confirmed absent names return `ENOENT`, but
`openat(O_CREAT|O_EXCL)` on `/media/usb` returns errno95. Evidence is under
`build/hardware/store-provision-qemu-v{1,2}-20261031/`.

Astra confirmed that the retained FAT object operations lack exclusive create
and rename callbacks. The marker's no-replace rename is also unsupported in
that interface. The legacy pathname callbacks do not provide the required
retained-object semantics and cannot safely be substituted: retained source
objects pin directory-slot identities that change on rename, and the legacy
replacement path deletes an existing destination. The next change must provide
object-relative exclusive creation and absent-destination rename while preserving
open/leased aliases, entry identity, read-only policy and namespace/FAT lock
ordering. The receiver remains unproven for QEMU and has not been deployed.

The FAT adapter and retained VFS backend now implement exclusive creation of a
regular child relative to a retained directory. Vnode, FIL and identity objects
are allocated before directory publication; FAT locking serializes collisions.
Read-only creators still carry dirty creation metadata, so they participate in
writer synchronization and failed-close recovery; an image lease additionally
requires actual read/write FIL access. FAT16/FAT32 host tests pass renamed-parent
and LFN handling, concurrent case-insensitive creation, collisions, allocation
failures, read-only mounts, a read-only creator racing a writer, and deferred
close/sync recovery. Astra reviewed both the adapter and backend change.

The production `openat(O_CREAT|O_EXCL)` syscall now succeeds in private QEMU.
A full16MiB receiver run created, streamed, synced, reopened and hash-verified
the disposable image. Its final marker rename returned `-95`, as the retained
FAT rename callback remains absent. The receiver reported `published=0` and
retained the failed transaction evidence; this is not a successful provisioning
run. Evidence: `build/hardware/store-fat-create-qemu-v1-20261031/`, including
`fat-create-diagnostic-result.json` and `store-qa/receipt.json`. Safe object-
relative no-replace rename and its identity/alias tests remain the next gate.
