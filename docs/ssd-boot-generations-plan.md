# SSD boot generations: proposed design

The Dell currently boots a single kernel and initrd from its SSD. Keep the
independent USB recovery stick usable until an SSD update and recovery path
has passed physical testing. This is a plan, not an implemented menu.

The existing USB A/B updater and EFI loader already verify paired kernel and
initrd hashes and select the newest valid record, with a root-payload fallback.
They do not provide interactive generation selection or recovery from a
hash-valid kernel that hangs. The installed SSD currently uses the direct
`KERNEL/BROOK.ELF` boot path, not the USB A/B layout.

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

The existing SSD EFI executable was independently hashed over the recovery
bridge and matches the known slot-capable loader. Its current configuration
uses the legacy direct pair, with no `UPDATES` directory. The system volume
reported approximately 2,064,096 KiB free; check it again before staging.

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
power-loss tests. The physical SSD has not yet been migrated.
