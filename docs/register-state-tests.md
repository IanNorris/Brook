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
