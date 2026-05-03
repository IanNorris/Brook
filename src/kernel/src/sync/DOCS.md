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
| `KMutexForceUnlock` | `(KMutex*, uint32_t pid)` | Forcibly release if held by given PID. Used during process teardown. Transfers to next waiter if any. |

### Lost-Wakeup Prevention
KMutex correctly handles the race between GuardRelease and SchedulerBlock
using `Process::pendingWakeup`:
1. Locker sets `pendingWakeup = 0` before enqueuing
2. Unlocker sets `pendingWakeup = 1` before calling `SchedulerUnblock`
3. `SchedulerBlock` checks `pendingWakeup` — if set, skips the actual block

This prevents the scenario where Unblock fires between GuardRelease and
SchedulerBlock and the wake is lost.

### Quality Assessment
✅ Correct and well-commented. The pendingWakeup pattern is the right approach.

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
| `KRwLockReadLock` | `(KRwLock*)` | Grant if no writer active. **⚠️ BUG (BRO-078): doesn't check `writersWaiting` — contradicts header comment, can starve writers.** |
| `KRwLockReadUnlock` | `(KRwLock*)` | Decrement readerCount. If last reader and writers waiting, wake next writer. |
| `KRwLockWriteLock` | `(KRwLock*)` | Grant if no readers and no writer active. Otherwise enqueue and block. Increments `writersWaiting`. |
| `KRwLockWriteUnlock` | `(KRwLock*)` | Prefer waking ALL queued readers (batch, up to 128). If no readers, wake next writer. |

### Known Issues
- **BRO-077**: Missing `pendingWakeup` pattern — lost wakeup race between
  GuardRelease and SchedulerBlock. Both ReadLock and WriteLock are affected.
- **BRO-078**: `KRwLockReadLock` only checks `writerActive`, ignoring
  `writersWaiting`. New readers can starve waiting writers indefinitely.
- Batch reader wakeup caps at 128 (stack array). More than 128 queued readers
  would require multiple passes — currently not handled (excess readers stay queued).
- No `ForceUnlock` equivalent for process teardown (KMutex has `KMutexForceUnlock`).

### Current Users
Only used in tests (`test_krwlock`). No kernel subsystem uses KRwLock yet.

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
| BRO-078 | medium | KRwLock | ReadLock ignores writersWaiting — writer starvation |
| BRO-079 | low | Heap | HeapCheckIntegrity doesn't hold lock |
| BRO-080 | low | PMM | PmmSetOwner lacks lock (dead code) |
