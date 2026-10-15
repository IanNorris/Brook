# Synchronization Primitives Documentation

## Overview

Brook provides three sleeping synchronization primitives (built on the
scheduler's Block/Unblock mechanism) plus a low-level interrupt-disabling
spinlock. All three sleeping primitives use the same internal pattern: a
brief interrupt-disabling ticket spinlock ("guard") protects queue
manipulation, then the caller yields if contended.

---

## Files

| File | Lines | Purpose |
|------|-------|---------|
| `spinlock.h` | 41 | Ticket spinlock (interrupt-disabling) |
| `kmutex.h` | 59 | Sleeping mutex declarations |
| `kmutex.cpp` | 189 | Sleeping mutex implementation |
| `krwlock.h` | 56 | Reader-writer lock declarations |
| `krwlock.cpp` | 170 | Reader-writer lock implementation |
| `ksemaphore.h` | 47 | Counting semaphore declarations |
| `ksemaphore.cpp` | 110 | Counting semaphore implementation |

---

## SpinLock (`spinlock.h`)

### Purpose
Lowest-level synchronization. Ticket-based (FIFO-fair). Disables interrupts
while held to prevent deadlock from timer preemption into code that needs
the same lock.

### Design
```
struct SpinLock {
    volatile uint32_t next;     // next ticket to issue
    volatile uint32_t serving;  // currently serving ticket
};
```

### Functions

| Function | Signature | Contract |
|----------|-----------|----------|
| `SpinLockAcquire` | `(SpinLock*) → uint64_t` | Saves RFLAGS, disables interrupts (CLI), atomically takes a ticket, spins until served. Returns saved RFLAGS. |
| `SpinLockRelease` | `(SpinLock*, uint64_t savedFlags)` | Increments `serving` (RELEASE ordering), restores interrupts if they were previously enabled (bit 9 of saved RFLAGS = IF). |

### Usage Rules
- **Never hold across a sleep** — spinlocks are for brief critical sections only.
- **Always pair Acquire/Release** — the saved RFLAGS must be passed back.
- **Nesting**: Safe to nest multiple spinlocks IF a consistent lock ordering is
  maintained to prevent ABBA deadlock. No lock ordering is enforced programmatically.
- **PAUSE hint**: Uses `pause` instruction in the spin loop, which improves
  performance on hyperthreaded cores and reduces bus traffic.

### Quality Assessment
✅ Clean, correct implementation. Memory ordering is appropriate (RELAXED for
ticket fetch, ACQUIRE for spin check, RELEASE for serving increment).

---

## KMutex (`kmutex.h`, `kmutex.cpp`)

### Purpose
Sleeping mutual-exclusion lock. When contended, the calling process blocks
(yields the CPU) instead of spinning. FIFO-fair: waiters are woken in order.

### Design
```
struct KMutex {
    volatile uint32_t locked;       // 1 = held, 0 = free
    volatile uint32_t ownerPid;     // holder's PID
    Process*          owner;        // pinned Process identity
    uint64_t          ownerIncarnation;
    Process*          waitHead;     // FIFO wait queue (singly-linked via Process::syncNext)
    Process*          waitTail;
    volatile uint32_t guardNext;    // internal ticket spinlock
    volatile uint32_t guardServing;
};
```

### Functions

| Function | Signature | Contract |
|----------|-----------|----------|
| `KMutexInit` | `(KMutex*)` | Zero all fields. |
| `KMutexLock` | `(KMutex*)` | Fast path: if unlocked, set locked=1 + ownerPid. Slow path: enqueue on wait queue, set `pendingWakeup=0`, release guard, call `SchedulerBlock`. |
| `KMutexUnlock` | `(KMutex*)` | If waiters: transfer ownership to first waiter (locked stays 1), set `pendingWakeup=1`, call `SchedulerUnblock`. If no waiters: set locked=0. |
| `KMutexTryLock` | `(KMutex*) → bool` | Non-blocking attempt. Returns true if acquired. |

### Lost-Wakeup Prevention
KMutex correctly handles the race between GuardRelease and SchedulerBlock
using `Process::pendingWakeup`:
1. Locker sets `pendingWakeup = 0` before enqueuing
2. Unlocker sets `pendingWakeup = 1` before calling `SchedulerUnblock`
3. `SchedulerBlock` checks `pendingWakeup` — if set, skips the actual block

This prevents the scenario where Unblock fires between GuardRelease and
SchedulerBlock and the wake is lost.

### Cooperative cancellation and lifetime

KMutex and KRwLock enter a scheduler-protected region before taking their internal
guards. Entry and terminal-state publication share `readyLock`; a kill either
precedes entry or becomes pending. Each queued/held acquisition owns a Process
reference. Handoffs take an additional temporary reference through the final
`SchedulerUnblock`, because a recipient can finish before that call returns.
KMutex checks the pinned owner pointer and incarnation, not merely a reusable PID.

Unlock drops the acquisition reference but keeps the operation protected until
syscall return, completed user file fault, a user-mode timer tick, or final exit
cleanup. This covers state between locks, such as a FAT cache loading placeholder.
A terminal request must not abandon a partial filesystem mutation. There is no
force-unlock recovery; a stalled operation remains pending and group-exit timeout
reports it without marking the live operation reapable. KRwLock exit cleanup is
now an invariant check, not a way to release partially mutated filesystem state.

Timer reaping only runs at user/idle boundaries, inside its own protected region,
so filesystem closes cannot re-enter a lock on an interrupted kernel stack.
Kernel-only workers need an explicit completed-operation boundary before deferred
termination can be delivered; ordinary user return boundaries cover filesystem
syscalls. A file-backed user-buffer fault can also arm protection inside a
syscall that subsequently waits on a pipe, futex, or socket; such unrelated,
unbounded waits need explicit operation-boundary review before claiming bounded
fatal-signal latency for every syscall. KSemaphore and general VM lifecycle
synchronization are separate work. Process-directed `kill(pid, SIGKILL)` now
terminates the thread group; existing `tkill`/`tgkill` fatal-group semantics and
`kill` process-group selectors remain separate compatibility work.

The `test_krwlock_cleanup` host target compiles both real lock implementations. Its
scheduler mock checks references and delayed grants; actual scheduler cancellation
requires the guest `fsexitcheck` regression and full bootstrap acceptance.

### Callers
KMutex is the primary sleeping lock used throughout the kernel:
- FatFS VFS operations
- VirtIO block device
- Process table operations
- Various subsystem locks

---

## KRwLock (`krwlock.h`, `krwlock.cpp`)

### Purpose
Reader-writer lock. Multiple readers concurrently, exclusive writers.

### Design
```
struct KRwLock {
    volatile int32_t  readerCount;     // active readers
    volatile uint32_t writerActive;    // 1 = writer holding
    volatile uint32_t writersWaiting;  // queued writer count
    Process*          readWaitHead/Tail;   // blocked readers
    Process*          writeWaitHead/Tail;  // blocked writers
    // internal guard spinlock fields
};
```

### Functions

| Function | Signature | Contract |
|----------|-----------|----------|
| `KRwLockInit` | `(KRwLock*)` | Zero all fields. |
| `KRwLockReadLock` | `(KRwLock*)` | Grant if no writer active and none waiting. Blocks if writers are waiting (writer-preference). |
| `KRwLockReadUnlock` | `(KRwLock*)` | Decrement readerCount. If last reader and writers waiting, wake next writer. |
| `KRwLockWriteLock` | `(KRwLock*)` | Grant if no readers and no writer active. Otherwise enqueue and block. Increments `writersWaiting`. |
| `KRwLockWriteUnlock` | `(KRwLock*)` | Prefer waking ALL queued readers (batch, up to 128). If no readers, wake next writer. |

### Known Issues
- Read/write waiters recheck their grant after unrelated wakes; `pendingWakeup`
  closes the guard-release-to-block window.
- **~~BRO-078~~**: Fixed — `KRwLockReadLock` now checks both `writerActive` and
  `writersWaiting`. Writers cannot be starved by new readers.
- Batch reader wakeup caps at 128 (stack array). More than 128 queued readers
  would require multiple passes — currently not handled (excess readers stay queued).
- Cancellation is deferred until the complete filesystem operation finishes.

### Current Users
Used by the ext2 filesystem and exercised by the real-code host harness.

---

## KSemaphore (`ksemaphore.h`, `ksemaphore.cpp`)

### Purpose
Counting semaphore. Count > 0 means permits are available. Blocks when
count reaches zero.

### Design
```
struct KSemaphore {
    volatile int32_t count;      // current count (>0 = available)
    Process*         waitHead;   // FIFO wait queue
    Process*         waitTail;
    // internal guard spinlock fields
};
```

### Functions

| Function | Signature | Contract |
|----------|-----------|----------|
| `KSemaphoreInit` | `(KSemaphore*, int32_t initialCount)` | Set count, null queue. |
| `KSemaphoreWait` | `(KSemaphore*)` | Decrement if count > 0. Otherwise enqueue and block. |
| `KSemaphoreSignal` | `(KSemaphore*)` | If waiters: wake first waiter (count unchanged). Otherwise: increment count. |
| `KSemaphoreTryWait` | `(KSemaphore*) → bool` | Non-blocking. Returns true if decremented. |

### Known Issues
- **BRO-077**: Missing `pendingWakeup` pattern — same lost-wakeup race as KRwLock.
  KSemaphoreWait can block forever if Signal fires between GuardRelease and
  SchedulerBlock.

### Current Users
Only used in tests (`test_ksemaphore`). No kernel subsystem uses KSemaphore yet.

---

## Cross-Cutting Issues

### Guard Lock Duplication
All three sleeping primitives (KMutex, KRwLock, KSemaphore) duplicate the same
interrupt-disabling ticket spinlock pattern inline. This is intentional (avoids
adding a SpinLock struct to each, since the guard fields are embedded) but means
a bug in the guard logic must be fixed in three places.

### Scheduler Integration
All three depend on:
- `SchedulerCurrentProcess()` — get the calling thread
- `SchedulerBlock(Process*)` — put thread to sleep
- `SchedulerUnblock(Process*)` — wake a sleeping thread
- `Process::syncNext` — linked-list linkage for wait queues
- `Process::pendingWakeup` — lost-wakeup flag (only used by KMutex currently)

### No Deadlock Detection
No lock ordering is enforced. No cycle detection. Deadlocks are silent hangs.
For a hobby OS this is acceptable, but worth noting for debugging complex
multi-lock scenarios.

### No Priority Inheritance
None of the sleeping locks implement priority inheritance. A high-priority
process blocked on a mutex held by a low-priority process will wait until the
low-priority process is scheduled. This can cause priority inversion under the
MLFQ scheduler.

---

## Summary of Open Bugs Found

| Bug | Severity | Component | Description |
|-----|----------|-----------|-------------|
| BRO-076 | low | Heap | g_freeBytes tracking drifts (diagnostic-only) |
| BRO-077 | high | KSemaphore/KRwLock | Missing pendingWakeup — lost wakeup race |
| ~~BRO-078~~ | ~~medium~~ | KRwLock | Fixed — ReadLock now checks writersWaiting |
| BRO-079 | low | Heap | HeapCheckIntegrity doesn't hold lock |
| BRO-080 | low | PMM | PmmSetOwner lacks lock (dead code) |
