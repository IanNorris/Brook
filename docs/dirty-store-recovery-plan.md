# Unclean desktop store: proposed automatic recovery

Brook's persistent desktop is an ext2 image stored as a FAT file. A dirty
ext2 state means the previous session was not cleanly finalized; it does not
by itself prove that metadata is corrupt. Conversely, changing the state bit
alone would not establish consistency. Linux commonly checks unclean ext2
with `e2fsck`; `e2fsck -p` fixes issues it considers safe and leaves other
cases for intervention. Ext2 has no journal to replay.

The current admission path runs before the scheduler starts, and only once per
boot. It validates the configured store identity, geometry and group metadata,
marks a clean store dirty, then binds ext2 and publishes `/nix` and home.
It cannot run a userspace checker at that point, and an error leaves the system
in maintenance without the applications contained in the store.

The intended recovery path is:

1. Distinguish a structurally recognized store needing a check from an invalid
   or unsupported image. Neither is mounted writable. A recognized header is
   only a candidate for checking, not proof of full filesystem integrity.
2. Start a restricted maintenance environment from the initrd, with a real
   `e2fsck` and its dependencies independent of `/nix`. Expose a clear progress
   and failure message while keeping normal services and apps stopped.
3. Keep exclusive kernel ownership of the exact FAT-backed image during the
   check. Provide the checker a narrowly scoped, bounded read/write/seek/fsync
   interface to that image rather than releasing the backing lease for an
   unrestricted pathname reopen. Bind recovery to the configured volume,
   size, UUID and the current boot; refuse ambiguous or unsupported inputs.
4. Run `e2fsck -p` offline. Interpret its exit flags, flush results and
   subsequent validation strictly. Uncorrected errors, unsupported features,
   interrupted checks and uncertain I/O stay in maintenance; never simply set
   the clean bit to make the normal mount proceed. Preserve useful diagnostics
   outside the store and prevent unbounded automatic repair/reboot loops.
5. After a successful check and durable flush, perform a checked reboot. The
   next boot repeats normal admission with fresh FAT handles and publishes the
   store only after its existing validation succeeds. This avoids rebinding a
   previously attempted ext2 drive or reusing retired cache state.

Before enabling automatic repair, test the actual checker and its syscall/I/O
requirements with disposable images: unclean but consistent data, preen-safe
repairs, errors requiring intervention, unsupported features, short reads and
writes, failed flushes, interruption at each phase, failed reboot/admission and
concurrent alias attempts. Verify both the containing FAT filesystem and the
inner ext2 image after recovery. Test under QEMU and then on physical hardware.

For the current Dell incident, first preserve the existing unmounted image and
repair a separate copy. That evidence-preserving operation is distinct from
implementing automatic boot recovery and must not be replaced by an untested
in-place repair.

References: [Linux ext2 filesystem checking](https://docs.kernel.org/7.0/filesystems/ext2.html),
[e2fsck manual and exit codes](https://man7.org/linux/man-pages/man8/e2fsck.8.html).
