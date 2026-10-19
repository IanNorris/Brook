# Recoverable user copies

Checkpoint of 17 October 2026. This protects explicit checked-copy helpers and
selected callers. It is not completion of the kernel-wide uaccess audit and does
not enable mapped regular-file truncate or writable MAP_SHARED.

## Mechanism

`user_copy.S` has separate copy-from/to stubs, each with one labeled REP MOVSB
instruction. A fault can redirect only to that stub's local return of RCX (bytes
remaining). No caller stack frame, C++ cleanup or held resource is skipped.

The pure classifier accepts only CPL0 data page faults at those exact labels,
with a nonempty, overflow-safe user interval, the expected write/read direction,
CR2 matching the current user-side index register, and DF clear. User-mode,
reserved-bit, instruction-fetch, unrelated-instruction and trusted-side faults
are rejected. IDT handles the fixup before legacy kernel lazy/CoW resolution,
without locks, allocation, scheduling or enabling interrupts; IRET restores the
saved flags.

The high-level helpers pin one AS and cancellation region across prefault,
optional test barrier, copy, bounded retry and cleanup. Retries resume at the
residual offset and validate/resolve only the remaining faulting page, outside
the exception handler. Four retries permit successful concurrent-fork CoW
resolution without allowing an endlessly changing mapping to spin forever.
Progress APIs return bytes actually copied; boolean wrappers require all bytes.
CString copying stays within each source page and accepts a copied terminator
without touching the next page.

These helpers may sleep while prefaulting/retrying. Callers must not hold the
inode publication gate, AS mutation or PT locks. Returning EFAULT from a copy is
not permission to abandon a caller's normal lock/resource cleanup.

## Connected callers

The old syscall checked-buffer, CString and CopyToUser helpers now use the
shared implementation. Clock/time, uname and getrandom outputs use kernel-local
values and protected copyout. Append staging uses only a successfully copied
prefix. FileObject-backed regular reads/writes use bounded kernel staging and
progress-aware copyout/copyin, outside the inode publication gate.

A file read uses a local backend offset and commits only delivered bytes. A
partial copyout therefore returns its delivered prefix and leaves the next file
byte available to the next read. Writes never submit uninitialized staging
suffixes. A separate inode `ioOffsetGate`, ordered before its publication gate,
serializes Read/Write/Append through staging and offset commit. Fault loading
uses backend operations directly and does not acquire this offset mutex, allowing
faults on the same file without recursive acquisition. Truncate and writeback
remain publication-gate-only.

This conservatively serializes independent opens of the same inode too. It is
not a complete open-file-description/dup/fork/lseek atomicity implementation;
those semantics remain a separate audit. The extra serialization needs inclusion
in subsequent I/O and real-workload performance checks.

## Verification

The new hosted test executes the actual assembly against Linux guard/read-only
pages. A SIGSEGV adapter supplies CPL0 framing to the actual classifier; the
copied bytes and REP residual registers are real. Cases cover partial counts,
zero length, untouched suffixes, direction/address/RIP/CPL/flags/overflow rejection,
and faults on the trusted side. This is not a hardware test of Brook's IDT.

The FileObject fixture separately injects partial/zero copy progress and checks
backend bytes, returned counts, unchanged failure offsets, retry without skipped
file data, and a concurrent reader held behind a partial offset commit. All 31
hosted suites pass (`build/user-copy-host-20261017.log`). Bootstrap acceptance's
expected native-suite count is updated to 31.

Running this assembly fixture inside Brook exposed a pre-existing signal-return
bug: SYSRET consumes RCX/R11 as RIP/RFLAGS, losing the actual interrupted values.
rt_sigreturn now selects a stack-local IRET frame, restores those GPRs separately,
uses the current GDT user selectors and validates RIP/RSP and user flags before
returning. The ucontext snapshot uses protected copyin with cancellation checked
again afterwards. Invalid contexts terminate the user process. Normal syscalls
keep their SYSRET path. A first candidate used stale selector literals and failed
with a kernel #GP; the next candidate passed the actual assembly fixture after
switching to GDT constants. Both failures and subsequent results are retained.

The fixture checks preserved/edited R11, RCX residual progress and malformed
RIP/RSP children. See [register tests](register-state-tests.md) for broader
entry/return tests and remaining interrupt coverage. Full signal FPU-frame
restoration is not established by these GPR checks.

The same static `scripts/workloads/user_copy.c` probe passes its six control
cases on Linux. A root-only test kernel adds phase 4 to the existing mapping
barrier: pause after checked-copy prefault and before the real copy instruction,
with AS/cancellation ownership and no inode publication/AS/PT lock held. Phase 3
remains the compositor barrier.

The guest test kernel passes 13/13 user-copy cases: controls plus partial
read/write/append, unmap failures, a clock protection race, and concurrent-fork
CoW retry with child data isolation. The existing file-fault probe passes 13/13,
including stale-EOF growth/remap/protection. The test VM then completes checked
sync/shutdown, QEMU exit and offline data/Nix/FAT checks with exit 0. Evidence is
under `build/workloads/user-copy-hooks-20261017/acceptance/`.

The final production kernel with signal-return and DF-entry fixes passes the
assembly fixture, all ten register probes, six copy controls and ten file-fault
cases as UID 1000. Checked shutdown and offline data/Nix/FAT checks pass. Evidence
is under `build/workloads/user-copy-df-production-20261017/acceptance/`; see the
register report for exact kernel/session identity.

The final hook-enabled kernel also passes user-copy 13/13 and file-fault 13/13,
plus register 10/10 as UID 1000 through the repository runner. Repeating that
runner returns the same completed request ID. Checked shutdown and all offline
disk checks pass. Evidence: `build/workloads/user-copy-df-hooks-20261017/`, session
`59ce2cf4826240008c841824db24dc21`. The normal build is restored with hooks OFF.

## Remaining gate before mapped resize

The [signal target checkpoint](signal-return-targets.md) also migrates
rt_sigaction input/output to recoverable staged copies. Its disposition lock
does not span those copies. Signal-frame construction still needs migration.

The exact-instruction fixup deliberately does not cover arbitrary kernel loads,
stores or memcpy. The inspected legacy paths still needing migration include:

- memfd reads/writes and other fd backends receiving raw user buffers;
- TTY/eventfd/timerfd/socket/pipe data paths and their partial-consumption rules;
- stat/directory/credential/timer structures and vector-array input/output;
- exec argv/env pointer traversal and signal-frame construction (signal-return
  ucontext input now uses protected copyin);
- futex user-word atomics, requiring their own safe atomic access contract.

This list is a source-review starting point, not an exhaustive passing audit.
Protect every remaining access route before enabling mapped resize generally,
then connect the resize coordinator, inject backend/EOF recovery failures and
renew bootstrap/browser/workload acceptance. No SQLite WAL or Firefox audio fix
is claimed by these copies alone.

## Connect address-copy follow-up (2026-10-19)

The pathname AF_UNIX `connect` path still dereferenced the caller's sockaddr
directly and read beyond the supplied length while scanning its path. The
AF_INET path also passed a raw user address to logging and the network stack.
Both now validate supported lengths and snapshot the supplied bytes through
`CopyFromUser` before using the address. The Unix snapshot is zero-initialized,
so a short supplied path cannot make the scanner inspect adjacent user memory.
AF_INET also accepts the usual 128-byte sockaddr-storage callers while copying
only their advertised length.

`scripts/workloads/connect_user_memory.py` passes 21 cases both on Linux and
through real Brook syscalls as UID1000. These cover null/kernel/protected-page
addresses, a family or final byte crossing into a protected page, valid-buffer
short/oversized lengths, and Unix/IPv4 addresses ending exactly at a page
boundary. A 128-byte IPv4 storage address at the boundary also succeeds. UDP
connects only set a local destination; this fixture sends no network packets.
The absent Unix destination case checks that bounded copying reaches name
lookup; its ENOENT/ECONNREFUSED alternatives do not establish pathname-socket
namespace conformance.

Candidate session `3ea046c94bdc4db995547fe8978c52f2` used eight CPUs,
4 GiB and fresh private disks with the hardware virgl compositor. All 52 host
suites and both supported compiled entry/startup audits passed for its exact
kernel. After the guest probe, Quake GL3's demo and Mousepad rendered and closed
normally, with full launch lineages drained. All 2,308 tracked GPU mappings
(65,867,776 bytes) were released without failure or quarantine. Audio streams
emptied; checked shutdown, owned launcher/QEMU exit zero and all three offline
filesystem checks passed. All ten existing document fixtures were preserved,
including the latest Codium save and its Brook ownership.

Evidence: `build/workloads/connect-user-copy-20261019/candidate/`, including
`acceptance-audit.json`, `guest-result.json`, `linux-reference.json`, the exact
`tested-fixture.py`, command output and frozen kernel/module artifacts.

This is targeted argument-copy coverage. Invalid ranges can be rejected before
the REP copy instruction, so EFAULT results alone do not prove live fault-fixup
execution. This fixture does not exercise concurrent unmap/protection changes,
all address/state error-ordering combinations, abstract Unix names, other
socket operations or every syscall handler. Existing assembly/register fixtures
and their separate limitations still apply. The source finding has not been
connected causally to the intermittent pre-Wayland boot panic; that investigation
remains open.
