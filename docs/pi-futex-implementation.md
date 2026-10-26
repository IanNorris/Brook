# PI futex implementation checkpoint (2026-10-26)

This replaces the deliberately unsupported basic PI mutex capability with an
ownership-aware implementation. Game acceptance is separate from the reduced
probe results below. PI condition-variable requeue remains ENOSYS.

## Ownership and memory admission

LOCK_PI, LOCK_PI2, TRYLOCK_PI and UNLOCK_PI use the existing private TGID/VA or
shared backing-object/offset key. Writable prefault precedes mapping admission;
the mapping transaction and IRQ-masked futex lock cover key derivation, atomic
word access and graph publication. No mapping fault or scheduler wake occurs
under the futex lock. Ordinary WAIT/WAKE cannot service PI waiters; mixed waits
are rejected. Private WAKE_OP now validates both keys before its atomic RMW;
shared WAKE_OP remains unsupported.

A caller can report successful acquisition only after its TID is installed in
the user word. Unlock checks ownership, selects by effective rank then FIFO,
transfers the word before publishing a grant, and retargets remaining donors.
Cancellation cannot revoke an already completed grant. Queue nodes retain task
incarnations and the adapter holds process references until graph use ends.
Chain cycles and self-locks are rejected. Existing process capacity bounds the
one-wait-per-thread graph; it does not add an independent small handle limit.

## Donation and scheduling scope

The graph propagates the highest donor through nested and merged dependencies.
The scheduler adapter enforces donated Brook internal ranks (0..3), including
preemption eligibility, affinity, running-on-another-CPU exclusion and generation
checks. Equal-rank dispatch rotates while donation is active. With no donation,
normal policy selection remains unchanged; CPU service is still accounted by
the selected policy. Donation is withdrawn before the last process pin drops.

These ranks are the existing initial scheduler hints, not complete Linux
nice/realtime policy support or a realtime latency guarantee. Existing priority
syscall limitations remain documented in [the priority checkpoint](scheduler-priority-abi.md).
PI requeue and broader scheduler-priority ABI work are not certified here.

## Exit and signal handling

Forced-exit cleanup now runs for userspace tasks without registered robust
lists too. Normal exit/exec cleanup processes robust-list links before waking
PI successors, because glibc reuses the acquired mutex's embedded list node.
Owner-death transfer preserves OWNER_DIED; invalid/unmapped words fail affected
waiters rather than leaving permanent references to a retired owner. The exit
admission interval retries outside publication locks until cleanup completes,
with selected-clock absolute deadline checks and fatal cancellation.

A caught signal cancels a pending graph edge and returns the internal
ERESTARTNOINTR class. Signal return preparation rewinds the saved syscall before
signal selection, independently of SA_RESTART, preserving its absolute timeout.
Ignored or no-longer-pending signals cannot leak that internal code to userspace.
A grant that wins the signal race returns ownership instead of restarting.
[Linux PI contract](https://docs.kernel.org/locking/pi-futex.html),
[Linux exit ordering](https://github.com/torvalds/linux/blob/v6.12/kernel/futex/core.c),
[Linux PI restart](https://github.com/torvalds/linux/blob/v6.12/kernel/futex/pi.c).

## Validation so far

All 112 host tests pass in the final sequential run. An earlier parallel run
had one audio-delivery timeout; it passed in isolation before the final run.
The real graph passes an independent 20,000-step donation/reference oracle.
The production scheduler adapter is compiled into its host fixture. Cleanup
queue and signal restart helpers have regressions. Exact compiled entry and
scheduler-start mutation audits pass. The module ABI remains 56.

The same glibc-linked public probe binaries pass on Linux and native Brook:

- pifutexcheck: 15 supported-mode ownership and ordinary WAIT/WAKE checks.
- picontendcheck: recursive PI initialization; 16 two-mutex robust owner-death
  runs with an arriving contender; three absolute-timeout clock modes; 1,600
  private and 1,600 shared-alias acquisitions with exclusive-owner checks;
  SIGKILL of a raw owner whose robust-list head is NULL; a caught signal without
  SA_RESTART while a pthread PI waiter must remain excluded from the mutex.

Kernel SHA256:
`4263289aa44adcb08ca2f398b525083da218fd5576ec81d27add4f6a3f079dad`.
Private evidence is under `build/workloads/pi-futex-20261026` and
`build/reviews/steam-games-pi-20261026/modal-control`. Licensed files, saves,
account data and unredacted screenshots are excluded from git.

The first unmodified Steam launch of native Graveyard Keeper gets past its old
PI mutex stall and initializes an OpenGL 4.6 device. It then aborts during Mono
assembly initialization with `mini-amd64.c:209`, an out-of-range signed-32-bit
relative call displacement. This is a new blocker, not game compatibility.
MAP_32BIT handling is under investigation; it has not yet been established as
the cause for this installed game build.

Follow-up: [bounded MAP_32BIT allocation](mmap32-checkpoint.md) records the
actual Mono requests, standalone reproducer and next native launch. The game
gets past Mono and produces graphics/audio, but its UI/resize rendering remains
unaccepted. The PI review completes checked shutdown, original-owner exit zero
and all four offline filesystem checks, with original saves unchanged.
