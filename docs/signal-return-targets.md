# Signal handler targets and disposition snapshots

The syscall-return review found that testing only RCX bit 47 does not establish
a low-canonical user address: higher bits can be set while bit 47 remains zero.
The ordinary SYSRET guard now preserves RAX in a consumed frame slot, checks
all bits above bit 46 through a scratch copy, restores RAX/RSP and branches to
the existing invariant-failure path if needed. Signal-return IRET still uses
its separate validated frame and restores RCX/R11 as ordinary registers.

User-supplied handler targets are checked before constructing a delivery frame,
both for pending signals at syscall return and synchronous faults. A target
outside Brook's supported user address interval terminates the group with
SIGSEGV instead of returning to an invalid target from CPL0. SIG_DFL/SIG_IGN
remain separate dispositions. Canonical but unmapped targets can still fault
in user mode. This is not a proof of all user-memory accesses or nested signals.

The shared address predicate also validates rt_sigreturn RIP/RSP. Hosted tests
cover interval boundaries and noncanonical values with bit 47 clear. The native
tests exercise real register return and malformed contexts independently.

## Stable action records

The per-group disposition table is private to `process.cpp`. Readers obtain a
complete `KernelSigaction` snapshot through `ReadSignalAction`; updates use
`ExchangeSignalAction`. Fork copying, exec resets, CLONE_CLEAR_SIGHAND and group
destruction use the same short IRQ-safe lock. Timer/sender/default-action checks
also use snapshots, so delivery validates and uses the same handler/flags/mask/
restorer record even if another thread installs a different action afterwards.
The table lock owns no user pointer and does not span prefault, allocation,
sleeping or user copying. The ABI's 32-byte size and field offsets are asserted.

rt_sigaction stages input and output in kernel-local records using recoverable
copy helpers. It rejects new SIGKILL/SIGSTOP dispositions and strips those bits
from the stored mask. As on Linux, installing a new action happens before old
action copyout; a copyout EFAULT can therefore leave the new action installed.
Failed input copy does not install a partial record. These rules do not establish
atomicity of every signal queue, mask, timer or thread-group lifetime operation.

## Controls and remaining scope

Build the static Linux/guest probe inside nix-shell:

```sh
"$MUSL_CC" -static -O2 -Wall -Wextra -Werror -pthread \
  scripts/workloads/signal_return.c -o build/package-cache/signal-return
build/package-cache/signal-return
```

The eleven controls cover a normal handled return; low, noncanonical and kernel
handler targets through both async and synchronous-fault delivery; SIGKILL/STOP
rules; invalid input, output and readonly output; mask filtering; and 10,000
concurrent installs against 10,000 queries requiring complete action records.
The copy test also verifies Linux's mutation-before-copyout-error ordering.
Invalid-handler children use default SIGSEGV and must die with that signal;
their parent and the VM must remain usable. No kernel panic is an accepted
outcome for these children.

Linux passes 11/11. The hook-enabled Brook candidate passes the initial 11
controls, all existing user/kernel register probes, actual-copy assembly,
copy 6/6 and file-fault 10/10, with checked shutdown and clean offline disk
checks. The final production probe additionally includes the copyout-error
ordering subcase. Evidence is retained under
`build/workloads/register-signal-return-hooks-20261017/` and
`build/workloads/register-signal-return-production-20261017/`.

The final production candidate passes all eleven signal controls (including
copyout-error ordering), ten user register controls, actual-copy assembly,
user-copy 6/6 and file-fault 10/10. The standard release build and 31 hosted
suites pass. The compiled-entry report matches the booted kernel SHA-256
`59e020134f0987f76e4e6a69c3ed67a5584f53c88138810a574b6961b1195f16`.
Firefox launched as UID 1000, rendered the local regression page and updated its
title after a native click and typing `IPI-state-restored`. Native close
completed with exit zero and no remaining group tasks; the desktop returned
to ten tasks. Checked filesystem/device-flush shutdown and all three offline
filesystem checks pass. The production root's `audit.json` links the evidence.
The input title verifies DOM/JavaScript behavior; the immediate post-input
capture retained earlier pixels, so it is not visual proof of the typed text.

Still open: protected construction of signal frames on mutable user stacks,
full signal xstate frames/restoration, nested/alternate-stack and restart
coverage, full syscall static modeling and the wider raw user-pointer audit.
The Capstone gate still classifies the syscall dispatcher as unsupported; these
source/runtime improvements must not be reported as complete static coverage.
