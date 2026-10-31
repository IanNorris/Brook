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
