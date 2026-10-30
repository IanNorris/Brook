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

## Remaining core work

- Complete physical USB integrity and Ethernet acceptance.
- Provide a network-only host bridge with explicit capabilities and no QEMU or
  host-desktop assumptions. Hardware post-panic TCP inspection is currently
  unavailable: the panic path halts CPUs; retain the optical scanner fallback.
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
