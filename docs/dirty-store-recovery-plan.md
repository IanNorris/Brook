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

`prepare_hardware_writable_desktop.py --e2fsck /path/to/static/e2fsck`
can embed a validated standalone checker into the initrd and records its hash.
This opt-in packaging does not itself run it or implement automatic recovery.

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

The repair must be represented by a durable attempt record on the containing
volume, tied to the store identity. Flush a `pending` record before the
checker can write. An interrupted, malformed or failed record must prevent
normal store admission even if `e2fsck` already set the ext2 clean bit. After
preen, perform a checked flush, an independent read-only full check and identity
revalidation before recording success and performing a checked reboot. Keep
the image lease pinned across checker failure and do not automatically retry an
ambiguous or interrupted attempt. An ordinary clean session can establish a
new recovery epoch.

The candidate's checker access must be a bounded capability to that retained
lease, including the read, write, seek, stat and sync operations the actual
checker uses. It must not release the lease and reopen a general pathname; an
alias or another writer could otherwise race the repair. Trace the checker on
the tested VM before settling on an FD or restricted vnode interface, and test
alias rejection, failed flush, checker death and interruption after the checker
sets the clean bit but before the durable success record.

Before enabling automatic repair, test the actual checker and its syscall/I/O
requirements with disposable images: unclean but consistent data, preen-safe
repairs, errors requiring intervention, unsupported features, short reads and
writes, failed flushes, interruption at each phase, failed reboot/admission and
concurrent alias attempts. Verify both the containing FAT filesystem and the
inner ext2 image after recovery. Test under QEMU and then on physical hardware.

When practical, preserve an unmounted image and repair a separate copy. That
evidence-preserving operation is distinct from implementing automatic boot
recovery; in-place repair still needs an independently tested checker and a
clear decision about the value of the data at risk.

## Dell recovery, 2026-10-31

The user chose direct recovery over completing the image copy, accepting that
the installation could be recreated if recovery failed. With the desktop store
unmounted, a static `e2fsck -f -n` reported an inode deletion-time issue and
block and inode bitmap inconsistencies. `e2fsck -p` exited 1 after correcting
errors; a subsequent `e2fsck -f -n` completed all five passes with exit 0.
After a checked restart, the SSD payload booted and the persistent desktop
store reported `ready`. These checks do not establish the cause of the
original hang or prove arbitrary interruption recovery. Automatic boot-time
checking and safe ownership of the backing image remain unimplemented.

The checker and a small chmod bootstrap were staged into RAM in the USB
recovery environment. The same checker and staging approach were exercised on
a disposable QEMU image first; no checker executable was written to the SSD.

## Offline diagnostic prototype, 2026-11-03

An optional recovery initrd can start a local, root-owned supervisor that
runs the bundled static checker with `-f -n` against the unmounted maintenance
device and reports the result on the console. It does not request repair or
restart and does not need network access. Its payload identity is checked
before starting the checker. This is a diagnostic only: the maintenance
device itself is still writable, so the read-only property relies on the
trusted checker and its fixed arguments, not a kernel-enforced capability.

On disposable QEMU SATA images, both a clean filesystem and deliberately
damaged allocation bitmap produced the expected result with no USB controller
or network; the entire disk hash was unchanged in each test. Console geometry
was adjusted so the result remains visible after display scaling. These tests
also verify that a missing checker is reported as an incomplete check and
leaves the complete disk unchanged. They do not establish physical Dell
behavior. This prototype has not been installed
on the Dell, and automatic repair and cross-boot recovery admission are not yet
implemented.

A Dell-identity-bound recovery pair has been prepared locally at
`build/hardware/dell-offline-recovery-pair-20261103/`. Its exact kernel,
initrd and selection record booted through F5 in QEMU with no networking or
USB. The different QEMU disk was rejected: the checker received `No such
device`, reported an incomplete check, and the entire test disk hash remained
unchanged. This confirms rejection of that wrong-disk configuration; it does
not establish correct admission of the physical SSD or a successful physical
boot. No recovery pair has been deployed to the Dell from this test.

References: [Linux ext2 filesystem checking](https://docs.kernel.org/7.0/filesystems/ext2.html),
[e2fsck manual and exit codes](https://man7.org/linux/man-pages/man8/e2fsck.8.html).
