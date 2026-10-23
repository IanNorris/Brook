# Steam robust-futex checkpoint (2026-10-23)

Steam is not at the login screen. This checkpoint removes the previously
identified robust-list initialization crash; the staged client now exits 1 at
another startup failure. It does not establish updater or authentication support.

## Cause and implementation

The prior steamwebhelper fault at RIP 0x67df26 was an intentional null store,
not an unexplained memory corruption. Its preceding get_robust_list (274)
expects a non-null head and a 24-byte x86-64 robust-list ABI. Brook previously
accepted set_robust_list (273) without retaining the registration and lacked the
query syscall. The ELF assertion identifies missing pthread robust-list setup.

Brook now retains a per-thread registration, implements both syscalls and walks
the legacy non-PI list on normal exit, forced termination and exec. Cleanup
atomically replaces the dying owner's TID with OWNER_DIED, preserves WAITERS,
and wakes one appropriate private/shared waiter. The pending-operation entry
is handled separately, including an interrupted unlock whose owner is already
zero. Walks and retry loops are bounded; malformed pointers, PI entries,
foreign owners and device mappings do not authorize arbitrary writes.

Forced termination queues one retained process reference on the existing cleanup
worker, waits until the victim is off CPU, and holds its address space explicitly.
The worker resolves and validates the target mapping, including COW and lazy
pages, without switching CR3 or treating the dying process as the operation actor.
Mapping locks are released before scheduler wakeups. Queue ownership prevents
reaping before cleanup, and SIGCHLD notification is gated until cleanup finishes.
Exec cleanup runs before replacing the old address space and thread identity.
Fork and clone start with no inherited robust-list registration.

Module ABI is 49. get_robust_list queries are limited to the caller's thread
group; cross-process ptrace/dumpability access policy remains unimplemented.
PI futexes, shared WAKE_OP and newer robust-unlock ABI extensions are not claimed.

## Validation

The static source-owned probe is
`tools/at-syscall-probe/robustfutexcheck.c`. Linux control and the frozen Brook
kernel pass registration/query, invalid-length/output checks, normal and pending
owner death, cyclic/malformed lists, foreign-owner preservation, SIGKILL,
exit_group, WNOWAIT-before-reap, pthread robust recovery, exec-before-exit,
COW isolation and concurrent mapping replacement. The Brook run reports 225
checks; this count includes timing-dependent polling assertions.

The host fixture verifies cleanup queue ownership, duplicate suppression,
on-CPU deferral and final reference release. Astra reviewed lifecycle ordering,
target-address-space access, bounded fault retries and notification-aware query
reference release. Hardware Intel virgl GLX context/readback passes with no GL
or X errors. The canonical /dev/shm 19-case alias/isolation/cancellation/remap
suite passes again as UID 1000.

The Steam retry uses the existing staged client and interpreter patches, with
bootstrap skipped and raw client logs retained inside the guest. Its wrapper
exits 1 instead of the prior 139. Descendant completion is checked separately.
Fixed counters show later pipe assertions, but no SIGSEGV, controller shared-memory
allocation error, /dev/shm error or out-of-space error. Guest command transport
briefly timed out during this run; the original diagnostic identity was recovered,
not replaced or relaunched.

The pipe assertion's ELF constructor calls pipe2 with flags 0x84000:
O_CLOEXEC | O_DIRECT. Brook currently permits only O_CLOEXEC | O_NONBLOCK.
Packet-mode pipe semantics are the next distinct implementation lead; merely
ignoring O_DIRECT would incorrectly merge messages or retain truncated packets.

Ignored evidence: `build/workloads/steam-robust-20261023/` and
`build/reviews/steam-robust-off-gpu-1ms-fixed-mlfq-a-20261023/`.
Frozen ELF SHA-256:
`ebf66a450e3d8d7733f796a5256f2cb7eb2d2758b48895599213305376bafdec`.
Earlier build/controller errors and host-test timeouts are retained as failures.
All 101 rebuilt host tests pass in the established Debug host configuration,
with normal CPU affinity after the VM stopped (12.53 seconds). Six affected
ASan/UBSan fixtures pass, including a single-CPU repeat for mapping pins.
Entry-state and scheduler mutation audits pass against the frozen ELF. All
tracked guest jobs drained; the exact Xwayland process exited 0. Heap integrity
passed, guest sync completed, the original launcher was harvested with exit 0,
and boot/data/Nix/temporary disks each passed offline fsck with exit 0.

Validation limitations remain recorded rather than hidden: a fresh unwrapped
host compiler failed to link its CMake probe, a fresh Release host configuration
hit unused variables in the existing assert-based user-namespace fixture, and
CPU-constrained stress runs timed out. The established rebuilt host suite with
normal affinity is the final acceptance run; it does not certify every alternate
host test configuration. No kernel build or staging happened while QEMU ran.

Legacy ABI reference: <https://kernel.org/doc/html/latest/locking/robust-futex-ABI.html>.
