# SSD store automatic repair

The Dell boot image already contains a static `e2fsck`, but the normal boot
does not invoke it. A dirty store is rejected before `/nix` or `/data/home`
is published, and the system remains in the initrd maintenance environment.
The user has requested automatic repair; the procedure below is not yet
implemented.

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
