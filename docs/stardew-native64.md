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

## Signal FP-state baseline

Local inspection of the public CoreCLR component identifies the nested #GP at
module-relative `0x519b2f`: `fxrstor [rdi+0x100]`. A breakpoint at an earlier
valid invocation is not sufficient evidence; its MXCSR is legal. A subsequent
capture at the actual `ExceptionStub13` entry uses the selected CPU's CR3 to
read just the failing operand's alignment/control metadata. The operand is
16-byte aligned, but MXCSR is 406284485, with reserved bits set. The failing
instruction and invalid control word explain the #GP. The debugger's first
virtual-memory read failed; the physical page-table walk corrected that
observation problem without changing the guest.

Brook's synchronous and syscall-return signal delivery both zero the signal
frame, leaving `mc.fpstate` null; sigreturn restores only GPRs. The public
`tools/at-syscall-probe/signalfpcheck.c` passes eight Linux cases: XMM/YMM
export, handler clobber followed by restoration, handler-edited saved state,
asynchronous versus synchronous delivery, and normal versus alternate stack.
On the unchanged T candidate, the first case fails both valid-image and
restoration checks. This independently reproduces the ABI gap. It does not
yet prove that repairing it is sufficient for the game to start.

The next implementation must use the interrupted XSAVE slots already owned by
the exception/syscall entry wrappers, not a potentially stale process save or
a C-level save after kernel SSE use. Pass the slot explicitly after XSAVE,
which clobbers EDX. Build a zeroed, standard Linux signal image from defined
x87/SSE/AVX components, normalizing absent components and reserved bytes;
never expose all 1088 kernel-slot bytes. Supply aligned FP storage and Linux
XSAVE metadata. On sigreturn, validate copied user state (including MXCSR,
header, bounds and trailer) before replacing the wrapper's final XRSTOR image.
Support legacy and null reset deliberately, and reject malformed images
without a kernel fault. Both delivery paths and their compiled entry audits
need coverage. The current nested-handler bookkeeping is a separate limit.

## Signal FP-state candidate and native acceptance

The U candidate implements the save/restore contract above in both delivery
paths. The third assembly-to-C argument is set after XSAVE clobbers EDX.
Export uses a zeroed, field-wise image; init x87/SSE components are materialized,
AVX presence follows the saved header, and no reserved kernel-slot bytes are
published. XCR0 and CPUID verify the supported standard x87/SSE/AVX layout;
the MXCSR mask comes from hardware, never from a user-supplied mask field.
Handlers begin with initial live FP state while ucontext retains interrupted
state. Sigreturn validates a copied image before replacing the authoritative
entry slot restored by the assembly wrapper.

Discovery and validation are separate. Missing magic or invalid discovery
sizes select legacy state; a readable missing trailer also selects legacy,
matching the Linux control. An unreadable trailer is an error. Recognized
extended state still rejects invalid MXCSR, unsupported components and
compacted/reserved headers. Null state resets x87/SSE/AVX; legacy state restores
x87/XMM and initializes upper YMM halves.

Validation on Linux and the eight-vCPU U review:

- Eight `signalfpcheck` cases verify nondefault x87 control and ST0, MXCSR,
  XMM/YMM state, handler edits and live clobber, default handler controls,
  synchronous versus syscall delivery, and normal versus alternate stacks.
- Ten `signalfpreturncheck` cases verify legacy/null restoration, discovery
  fallback, invalid MXCSR/XCOMP/XSTATE/pointer rejection, unreadable trailer
  rejection, and invalid MXCSR combined with legacy fallback. Invalid contexts
  terminate the child with SIGSEGV without taking down the VM.
- Focused host normalization/privacy tests pass, including poisoned init-state
  storage, x87 padding, unused tail, reserved headers and malformed images.
  The release build and supported entry/scheduler mutation audits pass. New
  entry checks reject missing/wrong FP-slot arguments in full exception stubs
  and the syscall signal-check call site; the broader dispatcher remains an
  unsupported full-return proof, as before.

The actual game still faults at CoreCLR `FXRSTOR`. Its failing context now has
capture flags `0x0010000f`; the restore caller is module offset `0x4ddac2`,
identifying the alternate-stack returnPoint after the signal worker, rather
than the converted exception context. A breakpoint after that returnPoint's
capture observes valid MXCSR `0x1f80` and mask `0xffff`. At the later restore,
the mask is zero and MXCSR invalid. Compare context identity and track the
intervening write next. A first bounded raw write-watch attempt did not yield
a writer; its packet acceptance was not verified, and its timeout-side read
used an idle CPU's CR3, so it is not evidence that no write occurred.
No title/menu or game-launch success is claimed.
