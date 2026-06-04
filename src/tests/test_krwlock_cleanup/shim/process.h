#pragma once

// Host-test shim for process.h (BRO-162).
//
// The real src/kernel/src/process.h pulls in the entire kernel dependency tree
// (input.h, fd_table.h, spinlock.h, ...) and is not host-compilable. This shim
// defines just the brook::Process fields that the REAL krwlock.cpp touches, so
// the actual lock implementation can be compiled and exercised in user space.
//
// IMPORTANT: keep the rwlock-related fields below in exact sync with the real
// process.h. If you add an rwlock per-thread field in the kernel, mirror it
// here or the real krwlock.cpp will fail to compile against this shim.

#include <stdint.h>

namespace brook {

struct KRwLock;

struct Process {
    int               id;            // test-only identity (not used by krwlock.cpp)

    Process*          syncNext;
    volatile uint32_t pendingWakeup;

    // RwLock tracking for cleanup on thread exit — must match real process.h.
    KRwLock*          blockedOnRwLock;
    KRwLock*          heldWriteLock;
    KRwLock*          heldReadLock;
    bool              blockedAsWriter;
};

} // namespace brook
