# Stardew Valley native64 startup — 24 October 2026

The default Linux depot installs and verifies through Steam, but game startup
is not yet accepted. The title/menu, basic input and normal exit remain the
goal; no existing save was loaded during these trials. Game files and account
evidence stay in ignored private reviews.

## Memory barriers

The original direct startup reports CoreCLR HRESULT 0x8007FF02. A PID-scoped
trace shows membarrier QUERY returning ENOSYS, followed by one-page mlock
returning ENOSYS and process exit. This matches the initialization choice in
[.NET's GC platform code](https://raw.githubusercontent.com/dotnet/runtime/v6.0.36/src/coreclr/gc/unix/gcenv.unix.cpp):
use registered private expedited barriers when available, otherwise lock a
helper page for the protection-change fallback. This source comparison does
not identify the shipped binary's exact source revision.

The candidate implements only QUERY, REGISTER_PRIVATE_EXPEDITED and
PRIVATE_EXPEDITED. Registration belongs to the address space: threads sharing
CLONE_VM share it, fork copies it and fresh exec clears it. Other commands
and nonzero flags return EINVAL; use before registration returns EPERM.
Full mlock semantics remain unimplemented.

The first implementation broadcasts a separate memory-barrier epoch to every
online CPU, a stronger scope than the process-private contract. It shares the
existing TLB interrupt delivery and timer service points, but neither the TLB
request slot/lock nor the PMM quarantine epoch. The caller fences before
publication and after acknowledgements. Each CPU fences before publishing its
acknowledgement, with interrupts disabled across CPU identification and ACK
publication. Concurrent epochs coalesce; epoch wrap is rejected. The caller
waits with interrupts enabled and no request/address-space lock. Missing ACKs
cause failure after a bounded wait, never off-CR3 forgiveness or false success.

An earlier, untested draft sharing the TLB request lock was discarded: a CPU
holding that lock while waiting for all CPUs can form a transitive cycle with
other IRQ locks. The independent epoch removes that lock dependency.
Narrowing targets later would require explicit scheduler/migration ordering.
See [Linux's ordering scenarios](https://raw.githubusercontent.com/torvalds/linux/v6.12/kernel/sched/membarrier.c).

## Validation so far

- Release kernel builds. Supported compiled entry, scheduler-start and
  scheduler lock/wait audits pass with mutation checks. The entry audit admits
  scalar MFENCE in the GPR-only TLB leaf; vector instructions, calls and
  external branches remain rejected. Its external-branch mutation now targets
  beyond the actual function size rather than assuming a rel8 displacement
  cannot land inside a larger function.
- `tools/at-syscall-probe/membarriercheck.c` passes on Linux and the eight-vCPU
  Brook T review: 15 checks, including QUERY, invalid flags/combined commands,
  EPERM before registration, idempotent registration, inherited fork state,
  exec reset, shared-thread use and 10,000 StoreLoad litmus rounds with the
  fast side using only a compiler barrier. No forbidden outcome occurred.
- A strengthened probe also passes on both systems, with two concurrent
  issuers explicitly alternating CPU affinity while a third worker performs
  mmap/mprotect/munmap. These are empirical ordering/stability controls, not
  exhaustive proof or performance acceptance.

The T review uses kernel capsule v17, 12 GiB RAM, eight vCPUs, MLFQ and the
existing Intel hardware graphics configuration. The S staging attempt stopped
before VM launch because its manifest incorrectly included compiler temporary
objects; the corrected capsule inventories only kernel, symbols and 13 modules.
No running VM was overwritten or rebuilt.

## Next startup failure

With the candidate, the traced direct game apphost observes QUERY=24 and
successful registration, then creates runtime threads. The earlier CoreCLR
initialization error is absent. Two launches instead end with a null-adjacent
SIGSEGV in dynamically mapped executable code, at the same page-relative
offset. After Brook delivers that fault to .NET's handler, a user-mode #GP
occurs inside a different mapped code region (CS 0x2b, error zero). Signal
context handling is the next diagnostic target; the current Linux-compatible
signal frame explicitly supplies a null fpstate pointer. Its role in the
handler fault has not yet been established. No game window is reached.
A scoped trace also finds getcpu ENOSYS;
that is a compatibility lead, not an established cause of the fault.

The original Steam route independently remains blocked by its i386 reaper and
the later game-container bind-source failure. A previously tested private
64-bit reaper was restored to stock after diagnosis; no replacement is shipped.
Successful direct startup alone will not complete Steam launch acceptance.
