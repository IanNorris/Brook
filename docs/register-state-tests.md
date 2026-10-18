# Entry and return register tests

The native workload `scripts/workloads/register_state.c` links with
`register_state.S`. The assembly loads distinct values and captures state before
calling C or libc again. Build in the dev shell:

```sh
"$MUSL_CC" -static -O2 -Wall -Wextra -Werror \
  scripts/workloads/register_state.c scripts/workloads/register_state.S \
  -o build/package-cache/register-state
build/package-cache/register-state
```

## Contracts and current probes

Ordinary SYSCALL preserves GPRs except RAX (result), RCX (resume RIP) and R11
(entry flags). Tests check those three outputs too. Signal return restores the
saved GPRs, including RCX and R11, and accepts validated handler edits. A page
fault resolved by the kernel preserves the interrupted state.

Every probe compares the exact RSP captured before entry with the first snapshot
after return. Fifteen stack canaries occupy red-zone slots -16 through -128.
Slot -8 is excluded because the probe itself uses PUSHFQ to capture flags.
Restoring RSP alone does not establish kernel-stack balance or frame bounds.

The initial matrix covers getpid/gettid, ENOSYS, sched_yield, blocking nanosleep,
EBADF, first-touch anonymous page fault, SIGILL delivery/return with unchanged
and edited GPRs, and a long user loop exposed to timer interrupts. The loop
changes only RCX and preserves flags. It does not independently count IRQs or
prove a scheduler switch occurred during that interval.

The nonsignal probes compare XMM0–15, MXCSR and x87 control state using FXSAVE.
Signal vector-state preservation and handler edits are not covered by these
initial signal cases. Linux is the reference run; a Linux pass is not a Brook
pass. The initial static Linux probe passes 10/10.

The first Brook run exposed missing CLD before the full exception stubs' C
handlers. With DF deliberately set, SIGILL delivery corrupted kernel execution.
Both error-code and no-error-code assembly stubs now clear live DF before C;
the IRET frame preserves the interrupted flags. Signal delivery also clears
DF/TF/RF for the C signal handler while preserving them in the saved ucontext.
This checkpoint additionally replaces stale signal-context CS literals with the
current GDT constant. The failed run remains under
`build/workloads/user-copy-selectors-production-20261017/`; forced termination
was required, and all isolated offline filesystem checks returned zero.

After those fixes, the same ten cases pass inside Brook as UID 1000, including
exact RSP, red-zone canaries and both signal-return GPR cases. The actual-copy
assembly fixture also passes, including malformed-context child termination.
The six copy controls and ten file-fault cases pass in the same VM, followed by
checked sync/shutdown, QEMU exit 0 and data/Nix/FAT offline checks all zero.
Evidence: `build/workloads/user-copy-df-production-20261017/acceptance/`, session
`1f3bf94bfabd4947aa48d95102e977b3`, kernel SHA-256
`8c4d6158d3f51a52ee5aa655056c29d9d7c1dbc2ce921d3b63a7c0656309495f`.
Hosted suites pass 31/31 (`build/register-state-df-build-20261017.log`). These
results do not establish the unimplemented coverage below.

To repeat in a guest, stage the static binary onto an offline experiment boot
disk as `REGCHK`, boot that candidate and run `/boot/REGCHK` through
`vm_guest_run` as UID 1000. Require exit 0 and `REGISTER_RESULT 10/10`, retain
per-case output, and verify cleanup and checked shutdown. A timeout, lost job or
kernel panic is a failure, even if preceding cases printed PASS.

The repository runner records one durable request ID and bounded job output:

```sh
python3 scripts/register_acceptance.py SESSION build/workloads/register-check
```

It runs as UID 1000 and requires the exact marker plus exit 0. On observation
timeout, rerun the same command/output directory to resume the same job. It
does not start or stop the VM; keep lifecycle/disk verification separate.

The runner passes against the final hook-enabled candidate as UID 1000; a repeat
uses the same completed ID `9b8dc7c3c8b6412985bd180192340844`. Evidence is in
`build/workloads/user-copy-df-hooks-20261017/register-runner/`. That VM also passes
13/13 copy races and 13/13 file-fault cases before checked shutdown and clean
offline data/Nix/FAT checks. This checks runner repeat observation, not arbitrary
transport-failure injection or all interrupt paths.

`test_user_copy` additionally executes real guard-page faults, verifies REP's
remaining count (RCX), preserves and edits R11, and submits malformed restored
RIP/RSP in child processes. Invalid user contexts must terminate that process
without a kernel IRET fault. Its pure classifier tests are complementary to
hardware guest execution, not substitutes for it.

## Coverage required before claiming all handlers

The subsequent [signal target/snapshot checkpoint](signal-return-targets.md)
strengthens SYSRET's full-range guard, validates signal targets and protects
the action table. It adds independent native invalid-handler/copy/concurrency
controls; complete static modeling of the syscall dispatcher is still open.

### Controlled kernel-mode checkpoint

`BROOK_REGISTER_TEST_HOOKS=ON` adds the root-only scalar test syscall 0xB31.
It runs four bounded CPL0 probes with maskable interrupts disabled, serializes
the shared scratch state without sleeping, and restores the caller's FPU state
and original IF. The exception hook accepts only its owning task's exact probe
RIP and expected vector; unrelated kernel faults keep their normal handling.
The interface and assembly are absent from normal builds.

The probes compare all GPRs, exact kernel RSP, four guards above the interrupted
RSP, arithmetic/DF flags, XMM0–15, MXCSR and x87 control state. Deliberate inner
clobbers ensure register/vector restores do real work. A recorded inner entry
RSP verifies C ABI alignment; arrival counts establish actual execution. Cases:

- A synthetic same-privilege frame enters the production IRQ wrapper macro.
  It checks the wrapper but is not evidence of hardware IRQ/IDT delivery.
- Real UD2 traverses the no-error-code #UD exception stub.
- Loading an invalid DS selector traverses the real error-code #GP stub.
- A controlled unmapped supervisor store traverses the real #PF stub.

Run the staged client with `scripts/register_acceptance.py SESSION OUTPUT
--kernel` (11/11 includes the original ten cases and the four-probe kernel batch).
As UID 1000, `/boot/REGCHK --kernel-denied` requires EPERM. Initial guest evidence
is in `build/workloads/kernel-register-hooks-20261017/kernel-run/`: all four
kernel cases pass, both client modes pass 11/11, checked shutdown and all three
offline filesystem checks pass. The returned kernel RSP matches exactly in
every case. These guards do not establish lower stack bounds/high-water marks.

### Compiled ELF static audit

The dev shell includes Capstone and pyelftools. Run:

```sh
python3 scripts/audit_entry_state.py build/release/kernel/BROOK.elf \
  --mutations --output build/register-entry-static.json
```

`scripts/build.sh` also runs this supported-template/mutation gate after building
the kernel and saves its coverage report. Unsupported paths remain visible in
that report; this ordinary build gate does not imply `--require-all` passed.

The checker decodes actual function bytes and recognizes complete supported
templates, including their branch targets. It checks register push/pop order,
error-slot discard, saved-frame RSP restoration, 64-byte C-call alignment, CLD,
XSAVE/header/XRSTOR sequences, GS entry/exit branches and live return-flag
normalization. It assumes callees obey the SysV ABI; it does not prove arbitrary
callee implementations, arbitrary assembly or hardware behavior.

An unknown path is reported as unsupported. A malformed nominated template or
missing required family fails. `--require-all` also fails for any unsupported
return path, so the current partial audit cannot pass as full coverage.
Fatal/nonreturning handlers, runtime IDT installation and external driver modules
still need a separate inventory/contract audit.

The initial production ELF passed nine supported stubs, with 39 rejected machine-byte
mutations. The initial test ELF passed ten stubs and 43 mutations. Cases change CLD to
STD, a pop register, XSAVE bytes, a GS branch displacement and error-slot discard.
Six paths were then unsupported: syscall dispatcher, timer, reschedule/TLB IPIs,
fork-child trampoline and initial user-mode switch. Reports:
`build/register-entry-static-production-20261017.json` and
`build/register-entry-static-hooks-20261017.json`.

### APIC follow-up and reschedule-IPI regression

The audit now models the timer wrapper, including its interrupted RIP/CS/RBP
offsets, validator call and final vector restore. The TLB IPI wrapper has no
XSAVE; its entire compiled callee is checked as a bounded scalar leaf, with no
calls, vector/FPU operations or branches outside local instruction boundaries.
Mutation cases inject PXOR, a call, an escaping branch and a removed return into
that actual callee. No transitive no-vector claim is inferred from compiler flags.

The reschedule IPI calls the scheduler and ordinary C callees. A new test queues
an actual self-IPI with IF clear, then enables interrupts for a bounded LOOP
interval. An owned test-only callback deliberately clobbers ABI caller-saved
GPRs and vector state once; an event counter verifies real IPI delivery. The
snapshot checks exact RSP, guards, GPRs, FPU state and returned IF/DF/status flags.
RCX intentionally becomes zero through LOOP. The kernel cancellation region
protects scratch ownership across this asynchronous interval.

The unchanged wrapper fails this test: exactly one IPI arrives, RSP/guards/GPRs
and flags match, but `fpu_bad=1` and the client exits 1 (10/11). The VM remains
healthy and completes checked shutdown and clean data/Nix/FAT checks. Baseline:
`build/workloads/register-resched-baseline-hooks-20261017/kernel-run/`, session
`5d677c2508c54175b144c21b0c57ade1`. The strengthened static audit also rejects its
missing save/restore contract. This test does not identify the cause of any
particular browser failure.

The production fix saves/restores x87/SSE/AVX around reschedule-IPI C calls, using
the same per-thread stack scheme as the timer. Interrupted CS is read through
the saved GPR-frame base rather than the temporary XSAVE stack pointer. The
root test batch now contains five cases, returning 5 on success; the client
still reports 11/11 because it counts the kernel batch as one case.

The NMI assembly now has ELF function type/size metadata, so it is explicitly
inventoried. It was previously an untyped zero-size symbol, outside the decoded
function inventory. Its multiple returning/terminal paths remain unsupported;
this metadata change is not NMI/IST acceptance. Current production scope is
twelve supported templates and four unsupported paths: syscall dispatcher,
fork-child trampoline, initial user switch and the panic/epoch NMI handler.

The fixed hook-enabled kernel passes all five kernel cases and the original ten
user cases (client 11/11), with actual IPI events=1, exact RSP/flags and
`fpu_bad=0`. UID 1000 is denied. Checked shutdown and all three disk checks pass.
Evidence: `build/workloads/register-resched-fixed-hooks-20261017/kernel-run/`,
session `325caf7674c449e6b96f03506d5ed0e0`.

The production build disables both test hooks and passes the standard build
gate and 31 hosted suites. Its twelve-template audit rejects 56 mutations;
the test kernel covers thirteen templates and 60 mutations. Production guest
checks pass register 10/10, copy 6/6 and file-fault 10/10. Firefox launches through
`/nix/bin/brook-firefox` as UID 1000, renders the local fixture, confirms exactly
one click and `IPI-state-restored` input through JavaScript window-title updates,
and exits 0 with no remaining job descendants after guarded WM close. A
boot-guarded 1920×1080 screenshot is retained in its acceptance directory. The
VM returns to ten processes before checked shutdown and clean disk checks.
Evidence: `build/workloads/register-resched-fixed-production-20261017/audit.json`,
session `2f3281337f9547afb29ca8c5863f9ad3`; static report
`build/register-resched-production-static-20261017.json`. Firefox audio, network
sites and full bootstrap/workload acceptance were not renewed by this local
interaction check.

The audit led to explicit ELF sizes on full exception stubs and consistent live
NT/DF/AC normalization before their IRET. Saved interrupted flags remain in the
hardware frame. Source references: [Capstone details](https://www.capstone-engine.org/features.html)
and [Python bindings](https://www.capstone-engine.org/lang_python).

The final statically audited test kernel also passes all four kernel probes and
the ten original cases, plus UID-1000 denial, followed by checked shutdown,
QEMU exit 0 and clean offline data/Nix/FAT checks. Evidence:
`build/workloads/kernel-register-final-hooks-20261017/kernel-run/`, session
`d48a7250c9984131b1ed3ec879c4f53a`. The production build disables both register and
mapping test hooks. NMI/IST and actual device IRQ delivery remain untested by
this kernel batch.

The ordinary Release build script, including its Capstone gate, and all 31
hosted suites pass (`build/register-entry-build-script-20261017.log`). The final
production ELF contains no register-test control/probe symbols. Explicit
`--require-all` returns failure with six unsupported paths and no failed
supported templates (`build/register-entry-all-coverage-20261017.json`).

### Remaining matrix

- Inventory every IDT vector, generated/shared IRQ wrapper and syscall entry
  variant. Associate each with its error-code frame, privilege transition,
  stack/IST choice, GS handling and save/restore implementation.
- Add test-kernel controlled triggers for no-error/error-code exceptions,
  software IRQ wrappers, APIC timer, shared device IRQ, IPI and NMI/IST paths.
  A software interrupt is insufficient evidence for hardware delivery semantics.
- Exercise CPL0 and CPL3 interruption, nested interrupts, preemption and migration,
  including entry/exit windows around SWAPGS. Record counters proving each path
  actually executed; do not infer coverage from elapsed time.
- Check kernel RSP before/after each controlled trigger, stack alignment at C
  entry, canaries outside owned frame/save areas, and repeated-entry high-water
  marks. Check IST restoration separately from the ordinary kernel stack.
- Extend state checks to x87 register contents/status, AVX when advertised,
  FS/GS bases and TLS. Verify signal-frame FPU restoration and intentional edits.
- Cover blocking/EINTR/restart, fork parent/child, signal delivery during syscall
  return, alternate signal stacks, and invalid frame copy faults. Exec and exit
  have replacement/termination contracts rather than return equality.
- Enumerate syscall handlers separately for argument/results and output-memory
  tests. Shared dispatcher preservation does not establish each syscall's API
  correctness. Privileged/fatal paths need disposable test VMs and explicit
  expected terminal outcomes.

Keep production test hooks disabled. Expand this matrix as tests and observed
path evidence are added; do not advertise exhaustive interrupt coverage yet.


### Full bootstrap follow-up (2026-10-18)

The [matching bootstrap refresh](bootstrap.md#matching-39-suite-refresh-after-clock-and-desktop-changes-2026-10-18)
passes all 39 suites three times on each path, including under each guest-built
kernel. Hosted `test_user_copy` exercises real CPL3 REP faults and Brook signal
return, with partial-count/RCX/R11 assertions and malformed RIP/RSP rejection.
Its hosted fault model translates the fault into the CPL0 fixup predicate; this
is not exhaustive live coverage of kernel-mode copies or interrupt vectors.
The new bootstrap fault audit preserves that distinction and does not mistake
intentional probes for application crashes.
