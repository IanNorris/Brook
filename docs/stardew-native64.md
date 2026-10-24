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

## Alternate-stack lifecycle defect

The verified breakpoint/watch comparison uses the same private returnPoint
address at capture and restore: its control fields change while CoreCLR's
signal worker runs. A verified raw hardware-watch packet was accepted, but no
writer stop occurred before the restore fault, so this is not evidence against
an overwrite.

Source inspection identifies a concrete prerequisite failure: fresh Brook
processes returned `ss_flags=0` with no configured alternate stack. CoreCLR's
`EnsureSignalAlternateStack` allocates only when its query returns exactly
`SS_DISABLE`; Brook's response skips that allocation. CoreCLR can then treat
the handler as already on an alternate stack and switch its worker onto the
interrupted stack, overlapping the still-live returnPoint. This overlap is a
source-supported explanation, pending the repaired game retry.

The static `sigaltstackcheck` passes Linux and fails unchanged Brook at the
fresh-query/CoreCLR precondition. The candidate canonicalizes fresh disabled
state, resets configuration for new shared-address-space threads and committed
exec, preserves fork inheritance, and determines `SS_ONSTACK` from the saved
user RSP rather than the generic handler flag. Input is safely copied before
output, including aliased structures. Both signal paths save configured stack
flags in ucontext and select alternate-stack entry from the interrupted RSP.
SS_AUTODISARM remains unsupported; configurations exceeding the existing
32-bit size field fail instead of silently truncating. Sigreturn restoration
of user-edited uc_stack and nested-handler bookkeeping remain separate gaps.

The eight-vCPU V review (kernel capsule v19) passes the full alternate-stack
probe plus all eight FP-state and ten FP-return cases. Supported compiled
entry, scheduler-start and scheduler-lock/wait mutation audits pass. The U
source review completed orderly shutdown and all four offline filesystem
checks passed before V was staged. Game startup acceptance remains separate.

The first V game retry progresses past the prior CoreCLR FXRSTOR #GP. It
handles the initial generated-code null fault and resumes to a later instruction
in that generated function, where another null fault occurs. Brook then refuses
handler delivery because its blanket `inSignalHandler` flag remains set; no
rt_sigreturn occurred between the faults. CoreCLR's direct context restoration
can bypass sigreturn, making that flag unsuitable for delivery eligibility.
Signal-mask-based delivery and nested/nonlocal-return tests are the next step.
No game window has been reached.

## Nonlocal return and nested signal acceptance

A five-case static Linux control reproduces the next fault independently:
unchanged V dies with SIGSEGV on the second synchronous recovery through
`sigsetjmp`/`siglongjmp`. W (kernel capsule v20) passes it, the alternate-stack
lifecycle probe, all eight FP-state cases and all ten FP-return cases. Focused
host user-copy/frame-bound and FP-normalization tests pass, as do supported
compiled entry and scheduler mutation audits.

Delivery now follows per-signal masks instead of a persistent handler-active
boolean; nonlocal user context restoration can bypass sigreturn without
stranding kernel delivery state. The boolean prerequisite on sigreturn is
removed while copied frame/return/FP validation remains. Both delivery paths
honor SA_NODEFER without discarding explicit sa_mask bits. A blocked or ignored
synchronous hardware fault remains fatal. Frames allocated while already on
an alternate stack must stay wholly inside that configured stack, including
the FP reservation; adjacent mapped pages do not extend it.

The control covers two sequential nonlocal SIGSEGV recoveries, nested
SIGUSR1/SIGUSR2 on the alternate stack with independent MXCSR and mask
restoration, synchronous same-signal SA_NODEFER nesting, a fatal blocked
hardware fault, and alternate-stack exhaustion. This is focused empirical
coverage, not complete Linux signal conformance. Deprecated handler fields
remain diagnostic only, preserving the current process/module ABI.

The W game retry gets past both generated-code null faults. It now reaches a
managed MonoGame SDL initializer and aborts with a missing macOS
`/usr/lib/libSystem.dylib`, rather than a corrupted signal return.
[MonoGame's platform detector](https://github.com/MonoGame/MonoGame/blob/v3.8.1/MonoGame.Framework/Platform/Utilities/CurrentPlatform.cs)
defaults Unix to macOS unless uname returns exactly `Linux`. A scoped game
trace confirms both uname calls succeed. Brook currently supplies `Brook` as
sysname, so the Linux ABI personality must identify as `Linux`; the candidate
retains Brook branding in the uname version and existing OS metadata. No game
assembly or library was modified.

X (capsule v21) verifies `sysname=Linux`, `version=#1 Brook`, `machine=x86_64`.
The unmodified game advances through SDL initialization to graphics-device
creation; the macOS library failure is absent. Release build and compiled entry/
scheduler audits pass. The authenticated profile contains existing save files,
so subsequent game tests use a separate empty HOME/XDG tree. A private hash and
metadata inventory of the original saves is retained for before/after checks;
no existing save is loaded. The direct diagnostic must also match the installed
Mesa GLX vendor path already included by the Steam launcher.

With the installed Mesa GLX vendor directory in its loader path, X reaches
Stardew's title menu and a mouse click opens the empty acceptance home's
`No Saved Games Found` screen. A separate SDL/OpenGL control creates a 640x360
window and reports matching logical/drawable sizes and initial viewport with
Mesa 4.6 compatibility rendering. The game initially draws a desktop-sized
menu into a smaller window; maximizing improves visibility but the lower edge
remains clipped. Escape navigation was not observed. These presentation/input
limits remain open, rather than being counted as full gameplay acceptance.
The native title-bar close ends the game with status137, not clean exit; an
in-game Exit-button test is still required. Original-save content hashes and
timestamps remain unchanged for all 33 files after the first title trial.
No through-Steam game-launch success is claimed.

The Exit-menu trial also ends with signal9. A scoped process trace attributes
this to the game itself calling `kill(getpid(), SIGKILL)`; the synthetic control
for nonleader `exit_group(37)` exits37 on both Linux and Brook. Thus signal9
is not, by itself, evidence that Brook forcibly closed a normally exiting game.
The game-initiated exit drains the tracked group completely. Full startup
strace heavily delays this workload; reducing it to process calls allows it to
reach its window again. Original-save hashes and timestamps remain identical
after all X trials.

## Native64 Steam reaper prerequisite (candidate)

The public 64-bit reaper needs real PR_SET/GET_CHILD_SUBREAPER semantics, rather
than the prior success stub. The candidate stores settings per group incarnation
under the scheduler registry lock: threads share, fork starts clear, exec
preserves. Final group death adopts live and zombie child groups into the nearest
live ancestor subreaper, with existing SIGCHLD/waitability/robust-exit gates.
The reaper fallback must not bypass adoption. User copies occur outside locks.

Review identified three important cases: exec's thread-birth latch does not mean
the surviving subreaper is dead; ancestry must come from the canonical retained
leader instead of an arbitrary copied thread; thread publication refreshes its
parent after concurrent reparenting. Nonleader exec currently detaches in Brook
rather than performing Linux de_thread/PID preservation. The bounded candidate
keeps children and the setting with any surviving old-group peers, copies the
setting to the detached exec process, and transfers ownership only when that
process is the final live member. This prevents a new ownership regression but
does not claim to repair Brook's pre-existing nonleader-exec ABI difference.

The static `subreapercheck` passes Linux: actual GET output, EFAULT, nonzero SET
and ignored unused arguments, sibling sharing, fork-clear/exec-preserve, twelve
running/zombie adoption rounds with exit status, nearest/cleared ancestor and
SIGCHLD. Unchanged X fails the initial GET-write check, independently confirming
the stub. Production-code host fixtures cover exec-latch eligibility, stale
member exclusion, PID reuse, live-old-group retention, final-member transfer
and ancestry refresh at publication. Native candidate acceptance is pending.

Y (capsule v22) passes the complete native subreaper control, including running
and zombie adoption while the original leader is dead and a sibling survives.
The actual unmodified public 64-bit reaper also passes two pipe-controlled
orphan tests on Linux and Brook: it remains alive while an adopted grandchild
is blocked, then propagates grandchild success or failure after release. All
nested/alternate-stack and 18 FP-state/return cases pass again on Y. All 108
host tests pass with assertions active in Release, along with compiled-entry
and scheduler mutation audits. General SIGCHLD is observed; the public probe
does not independently isolate the already-zombie adoption notification from
other coalescing SIGCHLD events.

The fresh Release build exposed disabled assertions in host fixtures, leading
to unused-variable errors and potentially removed fixture operations. Host
tests now centrally undefine NDEBUG so their assertions run in every build
type. This corrects test execution, not production-kernel assertion policy.

## Reproducible helper and Steam launch gate

`tools/reaper-pkg` packages the unchanged MIT-licensed upstream implementation
as a static native64 executable. Its Nix build runs the two orphan lifetime and
exit-status controls on Linux. The resulting package binary also passes both
controls on Y, with its digest verified before execution and the tracked job
fully drained. This is separate from the earlier manually built helper check.

`steam_client_check.py --launch-app 413150` queues the installed game through
Steam after startup. The current private acceptance profile redirects only this
game's HOME and XDG directories to an empty test home; existing saves and the
original Steam configuration are backed up privately. No account or game assets
are included in the repository.

Steam receives the launch request but displays a confirmation that launching
here would disconnect another computer's active game session. That confirmation
has been left untouched pending the owner's decision. A queued request and an
authenticated client do not yet establish successful through-Steam game launch.

A subsequent read-only inventory confirms all 33 original save files still have
identical content hashes and sizes, with no added or removed files. One file's
mtime changed during the Steam launch attempt. The prior byte-and-timestamp
invariant therefore no longer passes, even though save content is intact.
Private before/after inventories and Cloud logs are preserved. Steam Cloud
activity is present; causation for the timestamp change remains unproven.
Redirecting the game's HOME alone does not establish isolation from the Steam
client's Cloud synchronization. Before completing acceptance, verify that Cloud
cannot read or write the original save location during the controlled test.
