# SSD boot generations: proposed design

The Dell has two verified kernel/initrd slots on its SSD, plus the retained
legacy root payload and an independent USB recovery stick. Updates and fallback
between valid pairs have been tested, but an interactive menu and health-based
rollback are not implemented. Keep the USB recovery path usable.

The existing USB A/B updater and EFI loader already verify paired kernel and
initrd hashes and select the newest valid record, with a root-payload fallback.
They do not provide interactive generation selection or recovery from a
hash-valid kernel that hangs. The installed SSD now uses the same A/B slot
selection mechanism; the retained direct `KERNEL/BROOK.ELF` pair is its
fallback when neither slot is valid.

1. Define immutable, bounded generations as paired kernel/initrd files and a
   versioned manifest with generation ID, sizes, hashes and a human-readable
   label. Keep the store separate: selecting an older kernel must not silently
   roll back user data or promise backwards-compatible filesystem changes.
2. Add an EFI menu before loading the kernel. Default to the last confirmed
   generation after a roughly 2–3 second timeout; provide keyboard selection,
   a key such as F5 to open or hold the menu, a clear
   recovery entry and a way to reach the menu without relying on a running OS.
   Firmware keyboard support and held-key behavior must be checked on the Dell;
   the visible timed menu is the fallback if a held key is not delivered.
   Validate the selected pair exactly as the current loader does. A missing or
   corrupt choice must offer another verified pair rather than mixing files.
3. Publish new generations with write, flush and readback before updating a
   redundant selection record. Retain the confirmed generation and a known-good
   root recovery payload. Bound disk usage and refuse an update when there is
   insufficient safe space; do not overwrite the only working generation.
4. Add a boot-attempt record and an explicit userspace health confirmation.
   A candidate that hangs, panics or is interrupted before confirmation should
   return to a confirmed generation on a subsequent boot. Specify durable
   ordering and how repeated failures, damaged metadata, manual selection and
   confirmation interact; do not call this automatic rollback until tested.
5. Test selection and corruption in host tests, then UEFI QEMU tests covering
   timeout, keyboard, invalid pairs, interrupted publication, failed boots and
   clean confirmation. Finally test on the Dell while retaining the USB
   recovery stick. QEMU interruption alone is not proof of power-loss safety.

The initial SSD partition is 6 GiB and contains a 3.75 GiB store image, so
space accounting and an eventual expansion strategy are prerequisites for
retaining multiple substantial generations. FAT32's per-file size limit also
remains relevant to the store design.

## Existing Dell migration

Before migration, the existing SSD EFI executable was independently hashed
over the recovery bridge and matched the known slot-capable loader. At that
point the configuration used the legacy direct pair and had no `UPDATES`
directory. The system volume then reported approximately 2,064,096 KiB free;
check current space before staging further generations.

To migrate without replacing EFI or the working root pair, an identity-bound
SSD-specific helper should seed both capacity-sized slots from verified
SSD-specific kernel/initrd pairs. Flush and reopen/read back each file and
selector before enabling slots. Only after both records and pairs verify,
append `UPDATE_SLOTS=1` to the existing configuration and verify its complete
contents after flush. The root kernel/initrd and USB recovery remain intact.
If preparation fails or is interrupted, the legacy configuration remains the
default; after activation the loader tries verified pairs and then root.

This is not a guarantee against arbitrary FAT metadata loss or a hash-valid
kernel that hangs. The USB recovery remains necessary, and automated trial-boot
rollback is still a separate feature. Before physical migration, test each
interrupted stage on a private clone, including filesystem checks and booting,
then test a full slot update, corrupt-newest fallback and invalid-record root
fallback. A QEMU process stop does not establish physical power-cut behavior.

The candidate initializer and SSD-specific updater have now been exercised on
private QEMU SSDs. A new generation with a distinct payload ID booted; corrupting
it selected the previous generation; invalidating both selectors booted the
legacy pair. The initializer and updater also ran with the desktop store
mounted, followed by a clean ext2 check. Eight injected QEMU process stops at
migration stages were followed by offline FAT checks and successful boots; one
boot check needed a retry after an unexplained QEMU exit. These are not physical
power-loss tests.

The physical Dell was subsequently booted into the verified USB maintenance
payload. The one-shot workflow checked the exact SSD identity and original EFI,
kernel and initrd hashes, then initialized both SSD slots. The system updater
verified and activated the new xHCI/checker payload as generation 3 in slot A.
After a checked restart, the Dell reported that payload ID, a ready persistent
store and working Ethernet; the earlier persistence fixture verified again.
The legacy root pair and USB recovery remain available. After a clean shutdown,
the Dell booted from the SSD without the USB, but Ethernet failed: the xHCI
reset reported phase 0, error 4, USBCMD 0x2 and USBSTS 0x11. Disconnecting
power and USB devices restored the controller. An updated bounded reset-wait
candidate was then staged as generation 4 in slot B. One clean SSD-only warm
restart reached that payload, mounted the persistent store and initialized
xHCI and Ethernet. A second checked SSD-only warm restart into generation 5
also reached Ethernet and the persistent store; the existing 8 KiB test file
passed its cross-page and private-mapping checks. The installed test helper uses
a fixed filename and correctly refused a new exclusive-create attempt because
that file already exists. A third checked SSD-only warm restart entered
generation 6, reached Ethernet, and passed the existing file check again.
The user confirmed kernel-console keyboard scrolling on that boot. These
restarts are not proof that the
intermittent controller failure is fixed; test further and verify persistence
through a separate cold-power cycle before calling SSD-only operation reliable.
The subsequent physical SSD-only cold boot reached the desktop but reported
`phase=8 error=2 USBCMD=0x2 USBSTS=0x11`: the existing-reset wait timed out.
Brook had not issued its own reset at that point. The reported USBSTS was an
earlier sample, not a final reading, and does not establish the timeout state.
After a full power disconnection, Ethernet returned but the store was unclean
and not mounted. A read-only `e2fsck -f -n` passed, `e2fsck -p` reported
corrections to the unclean state, and a second `-f -n` passed. The QEMU-tested
candidate deferring operational PAGESIZE reads until after reset was staged as
generation 7 and booted on the Dell with Ethernet, the persistent store and
the existing file check all working. On this successful boot PCI command was
`0x6` and PMCSR `0x8` before and after handoff. This one warm boot does not
prove the intermittent cold-boot failure is fixed. The `e2fsck` checker is
bundled but automatic repair is not yet implemented.

The next SSD-only cold boot with generation 7 again failed at phase 8:
`error=2 USBCMD=0x2 USBSTS_last=0x11 USBSTS_now=0x11 valid=1`, after
10,002 ms. This confirms that the pre-existing reset bit did not clear during
the wait; it does not identify why. The successful warm boot's PCI and PMCSR
values must not be assumed to apply to this failing boot. Linux performs Intel
port routing after firmware handoff and before its readiness/reset sequence;
Brook currently routes only after controller start. A controlled comparison of
the failing boot's PCI power, ownership and routing state should precede any
change to that ordering. No blind controller reset or PCI power-cycle recovery
is established by this observation.

Generation 8 added read-only routing snapshots and was staged after the next
verified store repair. Its warm boot had PMCSR `0x8` and before-reset routing
`d0=0x300 d4=0x4ff d8=0 dc=0xf`; the later routing step enabled the allowed
ports. On the subsequent failed SSD-only cold boot, the user's photograph
showed the same PCI command/status and PMCSR (`cmd=0x6 status=0x290
pmcsr=0x8`), released firmware ownership (`sem=0x1000801 ctl=0`), and routing
already enabled before reset (`d0=0x7ff d4=0x4ff d8=0xf dc=0xf`). HCRST
remained set after 10,002 ms and both status samples were `0x11`. Thus missing
port routing is not supported as the immediate explanation for this failure;
the difference between firmware boot paths remains unresolved. The SSD store
was mounted in that failed-xHCI boot, so it must be cleanly shut down before
power is removed.

A later physical SSD boot successfully initialized xHCI, acquired Ethernet,
mounted the store and verified the existing test file. The user confirmed that
the recovery USB was unplugged, so this is an SSD-only success, not the planned
USB-presence comparison. The result confirms that the failure is intermittent;
it does not establish its cause or a fix. The store was then cleanly synced
for the next comparison.

The user then booted the SSD with the recovery USB and Ethernet adapter both
connected. Dell's F12 menu did not list the USB stick, and Brook again logged
the same phase-8 timeout with HCRST set and USBSTS `0x11`; PCI power, ownership
and routing matched the earlier failing boot. The store mounted successfully.
USB presence alone therefore did not prevent the failure. Firmware's failure
to list the stick is evidence of an earlier USB problem, but does not by itself
prove when HCRST was asserted or whether that is the sole firmware issue.

The user checked the Dell's settings: BIOS version A15, Fastboot already set to
Thorough, USB boot support enabled, and legacy boot disabled. No setting was
changed. These observations rule out disabled USB boot and a non-Thorough
Fastboot setting as explanations for the failed F12 observation; UEFI remains
the intended boot mode. Dell's [USB boot guidance for newer systems](https://www.dell.com/support/kbdoc/en-us/000131221/how-to-boot-to-an-external-device-using-usb-type-c-connection)
recommends Thorough; it is not proof of the 9343's exact settings or of a fix.
The [9343 service manual](https://downloads.dell.com/manuals/all-products/esuprt_laptop/esuprt_xps_laptop/xps-13-9343-laptop_service%20manual_en-us.pdf)
does not enumerate Fastboot settings. The next control is a clean shutdown and
F12 check with the recovery stick connected and the Ethernet adapter removed;
its result is pending. If firmware can enumerate the recovery stick, compare
USB and SSD firmware boot paths with matched payloads. Do not
replace the accepted EFI loader or force a controller reset to obtain this
comparison.
