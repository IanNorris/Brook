# SSD store automatic repair

**Current Dell layout:** the installation now uses a direct ext2 data
partition. The earlier FAT-backed image design below describes the older
layout and is not an implementation plan for the current SSD. The same
durability and exclusive-ownership requirements apply, but identities must
bind the physical disk, GPT disk and partition GUIDs, partition bounds and
ext2 UUID. No automatic writer or recovery admission is implemented yet.

## Experimental boot guard

An opt-in build produces a separate `BROOKGUARD.efi`; the normal
`BOOTX64.efi` is unchanged. The guarded binary embeds its expected disk GUID,
data partition GUID and ext2 UUID and requires two identical, valid, clean
records on the EFI system partition before trying any normal A/B or legacy
payload. An absent, interrupted, inconsistent or wrong-identity record enters
the independently verified recovery pair; if that pair is unavailable, the
loader halts. The decision does not depend on `BROOK.CFG`.

On a private QEMU SATA fixture, the clean case booted normally and pending,
mismatched, missing, truncated, corrupt and wrong-identity records selected
recovery, including when the configuration file was absent. These checks used
the legacy normal payload. Follow-up tests exercised a corrupt newest slot
with a usable older slot and invalid selectors with a usable legacy payload;
clean guards allowed the respective fallback, while pending guards selected
recovery. With a pending record and either a missing recovery initrd or a
corrupt recovery selection, the loader reached its fatal recovery message
without a normal kernel marker. Each denied case ran on a writable private
copy and left its full disk hash unchanged. Interruption across durable writes
still needs acceptance tests. The guard has no writer and
does not itself validate the physical disk or filesystem. It is not installed
on the Dell. Before any physical deployment, inventory every firmware-reachable
EFI entrypoint: an older EFI loader can bypass this guard even though old
kernel generations loaded through the guarded EFI cannot.

An unintegrated transaction primitive now specifies the first repair step:
validate both clean records and the expected identity, then write, sync and
independently read back a higher-generation pending record in each slot. It
authorizes a checker only after both records have been verified; any failure
after a write attempt is treated as uncertain. Host fault tests model failures,
partial writes and early persistence at every operation. This is not yet a
working disk transaction. In particular, the current FAT image lease accepts
only sector-aligned files, whereas these records are 128 bytes, and a retained
FatFs file buffer is not independent media readback. A bounded fixed-record
lease and uncached verification must be implemented and tested before wiring
this primitive to a checker or deploying it.

The Dell boot image already contains a static `e2fsck`, but the normal boot
does not invoke it. A dirty store is rejected before `/nix` or `/data/home`
is published, and the system remains in the initrd maintenance environment.
The user has requested automatic repair; the procedure below is not yet
implemented.

Dirty-store classification validates the superblock identity and expected
geometry before returning `needs-filesystem-check`, but deliberately does not
require group descriptors to be valid. Their condition remains unknown until
the offline checker reads them. Clean-store admission still validates group
descriptors before any write or mount. A deliberately dirty private QEMU store
with an invalid block-bitmap pointer entered maintenance without mounting or
altering the store; host tests cover the corresponding clean-state rejection.

The repair must happen after the scheduler can run a user process: store
admission currently runs before ordinary user processes can execute. On the
verified system-volume path, a boot-only maintenance coordinator should:

1. Only when the bound SSD identity, `STORE.OK` and configured image agree,
   and admission ended specifically at `needs-filesystem-check`, retain the
   already-acquired `FatImageDevice` backing and exclusive FAT lease instead
   of destroying it. Keep it unregistered, unbound to ext2 and unavailable
   through ordinary pathname opens. Other failures remain maintenance failures;
   they must not trigger repair against a guessed disk or another file.
2. Before any checker writes, persist and flush an identity-bound pending
   repair record. Normal store admission must respect that record even if an
   interrupted checker has already marked ext2 clean. Suppress ordinary app
   startup on this repair boot and start the coordinator only once the scheduler
   and cleanup infrastructure can run it.
3. Provide the checker a narrowly scoped descriptor over the retained device,
   with bounded reads, writes, seek, stat and flush. Keep the exclusive lease
   across both checker processes. Run the bundled checker with `-p`; save its
   exit status and output in the maintenance console. Never run a second
   checker or admit the store concurrently. A lost helper or ambiguous status
   is failure, not permission to retry a write automatically. Timeout handling
   must terminate and drain the checker and its I/O before ownership changes.
4. Treat preen exit codes 0 and 1 as candidates for recovery,
   flush the backing file and its FAT volume, then run a separate read-only
   `-f -n` verification. The read-only check must exit cleanly. Interrupted,
   uncorrected, operational, usage and cancellation outcomes remain in
   maintenance; never turn them into a successful boot.
5. Clear the identity-bound pending record only after successful verification
   and checked flush; persist and verify its completion before releasing the
   repair lease. A clean reboot is the first implementation's admission path: it
   rechecks SSD identity and `STORE.OK` and avoids resetting the existing
   boot-once mount state. Limit automatic repair/reboot attempts using durable,
   verified state so a recurring failure cannot create an endless reboot loop.
   Surface the result to the user and keep the USB recovery path intact.

Before enabling this on the Dell, use disposable QEMU disk copies to test clean
boots, dirty-but-repairable and uncorrectable filesystems, mismatched identity
or readiness records, concurrent aliases, checker failures and timeouts,
interruption at each write/flush/reboot boundary, retry limits and independent
offline `e2fsck -fn` verification. A clean result must survive a second boot
and preserve a known pre-reboot file. QEMU interruption does not establish
physical power-loss behavior. Keep the accepted physical boot generation and
USB recovery available through the first Dell test.

## Checker interface probe

The exact bundled static e2fsck 1.47.4 was traced on Linux against a disposable
64 MiB ext2 fixture, first with `-f -n` and then after marking it unclean with
`-p`. In the preen run it opened the image with `O_RDWR|O_EXCL`, kept that
descriptor open, and opened the same pathname again read-only for geometry
probing. Its image I/O included `pread64`, `pwrite64`, `lseek`, `fstat` and
`fsync`; block-size/read-only ioctls returned `ENOTTY` for the regular-file
fixture and it continued. The clean-state repair performed a 4096-byte
positional write followed by syncs. Therefore the repair endpoint needs
controlled concurrent read-only opens by the authorized checker in addition
to its exclusive writable description, and should present regular-file
semantics rather than assuming one inherited descriptor is enough.

This is evidence for one small Linux-hosted fixture and one repair case, not a
complete syscall specification or proof that the synthetic endpoint works in
Brook. More involved repairs and the actual guest checker still need testing.
