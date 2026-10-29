# TCP socket reuse blocking Steam shutdown

The final reverse-order raw-store Steam control reaches an authenticated Store
in 85.68 s, then its normal shutdown helper remains active. This control is a
failure, not an accepted clean-shutdown measurement. The paging/compression
pressure and animated-client controls pass independently; their evidence must
not hide this final stability failure.

## Captured dependency chain

The normal shutdown job remains the same operation throughout investigation:
`3b0f9788b9e143b9b763ad8f7b985217`, PID 312, four live tasks. Its native stack
waits in `wait4`. Original Steam leader PID 91 is dying in `exit_group`;
thread 121 is terminal-pending but correctly protected while waiting in
`KMutexLock -> EpollScanReady -> epoll_wait_impl`.

The epoll graph mutex is owned by PID/incarnation 191, on CPU 2. A bounded
1.5-second profile places all its samples in `HandleTcp+0x11b`. The live CPU
instruction is the socket ticket-lock wait at `0xffffffff8012981b` in the
exact running kernel (SHA-256
`0431db4e3a78b39f8e9a25589a1129e6ec23f9346d06de6acdb4f9e34fc5a922`).
It waits for ticket zero. A consistent paused snapshot of socket 21's lock
finds `next=3, serving=3`: nominally free, but permanently past that waiter.
The lock address is `0xffffffff8139d168` for this ELF only.

`SockClose()` clears the entire Socket, including its ticket counters, while
packet/maintenance lookups can already be using the slot. `SockCreateOwned()`
also clears the entire object on allocation/failure. Registry reservation
alone does not protect those readers. The close memset predates this branch
and remains on main; `net.cpp` has no paging-branch changes. This is a concrete
slot-lifecycle failure, not evidence that the pager or scheduler should abandon
protected operations.

Current process-layout offsets were regenerated using the exact kernel compile
command. Identity, incarnation and blocked saved context were checked around
bounded stack reads while paused. `Process.pid` is uint16; an initial diagnostic
that incorrectly read uint32 was rejected and retained. The running mutex
owner's saved context is stale, so its live CPU registers and sampled profile,
not the saved RBP chain, establish its active location.

## Coherent repair boundary

Preserving the ticket counters alone is insufficient. TCP uses the Socket after
unlocking for deferred ACK/SYNACK transmission and wakeups; UDP and listener
paths also retain it. The repair must cover those tails and buffer lifetime.

- Keep per-slot synchronization, generation, reservation/publication and borrow
  state outside resettable payload. Never reinitialize a live slot lock.
- Acquire generation-qualified callback borrows under the registry guard
  before reading a candidate; release the guard before taking the socket lock.
  Hold the borrow through the entire receive/listener/maintenance tail.
- Separate callback borrows from FD/accept-queue ownership. Callback release
  must not recursively invoke a blocking graceful close.
- Have the last owned reference claim final close once. Allow incoming ACK
  borrowers during the existing bounded FIN phase; then unpublish, drain
  existing borrowers outside all locks, detach/free buffers, reset payload,
  and finally release the reservation for reuse.
- Revalidate tuple/state under the socket lock. Decide delayed ACK state there
  and retain a bounded immutable transmit snapshot for sending after unlock;
  holding the socket lock across synchronous loopback transmission is unsafe.
- Audit public direct close callers and captured `pollWaiter` Process lifetime.
  A socket borrow does not itself retain a Process pointer.

Deterministic tests must exercise actual lifecycle code/socket layout: queued
RX ticket versus close/reset, a paused post-unlock callback versus free/reuse,
stale generation rejection, synchronous loopback FIN/ACK, listener close during
child construction, allocation failure and exact-once reference/buffer cleanup.
A test that only mirrors a memset replacement does not prove the fix.

## Evidence and disposition

Private evidence is under
`build/reviews/paging-steam-20261029/repeat-raw/`: job receipts, process list,
profile/speedscope, `stacks-verified/`, `stacks-owner/`, live CPU registers and
`socket21-lock.json`. Original signed-in source disks are unchanged.

Normal checked shutdown remains closing on the protected task. After preserving
that evidence, the agent-owned cloned failure VM was stopped through the bridge's
force path. The bridge completed a checked filesystem/device flush first, but
that is not an orderly application exit or a passing shutdown control. Preserve
the disks as failure evidence. No lock value or guest instruction was patched
to force the stranded thread past its lifetime protection.

The bounded paging/compression goal remains active until this discovered
acceptance failure is addressed and the missing clean control is rerun.

Read-only post-stop e2fsck of the failed data image returns 4: two deleted
inodes have zero deletion time, with associated inode/block bitmap and directory
count discrepancies. A checked flush does not complete process teardown or reap
open-but-unlinked objects. The image remains untouched; no repair is applied to
failure evidence. This adds no passing filesystem/shutdown result to acceptance.
Use fresh copies of the original checked source for the fixed-kernel rerun.


## Repair validation in progress

The repair keeps callback borrow counts, generation and last-close ownership
outside the resettable payload. The slot ticket lock is permanent. RX, UDP,
listener creation, maintenance and diagnostic scans pin each candidate before
reading fields; final close withdraws publication, waits outside all locks for
borrowers, then frees/reset/releases the reservation. FD and accept-queue refs
remain separate from callback borrows, whose release never starts graceful close.

RX and maintenance decide ACK state under the socket lock and capture immutable
route/header scalars before sending outside it. Stored poll waiters own Process
liveness refs, transferred through deferred wakeup or cleared on replacement,
operation return and final close. Connect/ACK-wait scoped cleanup covers timeout
with a socket owner surviving the waiting thread.

The new `test_socket_lifetime` compiles actual net.cpp/Socket/TCP code with
host allocation/scheduling stubs and a transmit-boundary substitution. It covers
queued RX ticket versus close, paused transmit tail versus buffer retirement,
borrow versus unpublish/reuse, stale generation rejection, reentrant FIN/ACK,
listener close during child allocation, partial allocation failure, concurrent
last-owner drops, UDP Process refs and blocking timeout with surviving FD owner.
Both it and existing `test_tcp` pass 100 repetitions under the Nix Clang host
configuration. Address/undefined-behavior sanitizer checks pass; the production
kernel builds and compiled entry/startup mutation audits pass. Hosted tests do
not prove real scheduler/NIC or Steam behavior. The fresh native control remains
the required acceptance gate; the original failed run remains preserved.

The first repaired raw-store VM (`socket-fixed-raw`) passes 64 native concurrent
TCP close/reuse sessions and renders the authenticated Steam Store with featured
content (Terra visual review). Steam reaches its titled window in 97.51 s; this
is a stability result, not a startup speed improvement. Its normal shutdown
helper exits with status 0, checked guest sync/shutdown completes and the QEMU
owner exits 0. Offline read-only checks of its data/Nix/tmp filesystems all exit 0. The
matching LZ4-store control is still pending. Reusable native stress is
`scripts/workloads/socket_close_reuse_check.py`; it uses only bounded loopback
connections and exact bidirectional payload checks.
