#pragma once

// syscall_memfd.h -- internal declarations shared from syscall_memfd.cpp.
// Internal to the syscall layer; split from syscall.cpp.

#include "memfd_seals.h"

#include "scheduler.h"
#include "memory/physical_memory.h"

namespace brook {

// Sparse / lazy memfd backing.
//
// GTK's gdk-wayland speculatively ftruncates ~700 MiB for an SHM cache and
// then mmap()s the whole region, but only ever touches a tiny slice. Eagerly
// allocating the full backing would OOM the guest. Instead we keep a per-fd
// page-index array: pageMap[i] is either 0 (unbacked, reads-as-zero) or the
// physical address of a 4 KiB page. Pages are allocated on first access —
// either via kernel sys_read/sys_write to the fd, or via a user #PF on a
// MAP_SHARED mapping (handled in idt.cpp). Multiple processes sharing the
// fd share the same physical pages, which is the wl_shm contract.
struct MemFdData {
    uint64_t  inode;            // stable backing-object identity, shared by dup/fork/SCM_RIGHTS
    uint64_t* pageMap;          // [pageMapCount] entries; raw PhysicalAddress.value, 0 = unbacked
    uint64_t  pageMapCount;     // number of slots; covers `capacity` bytes
    uint64_t  size;             // logical size set by truncate/write/fallocate
    uint64_t  capacity;         // pageMapCount * 4096
    volatile uint32_t lock;     // spinlock protecting pageMap allocations + grow
    volatile uint32_t refCount; // +1 per fd, +1 per live mmap VMA
    MemFdSealState seals{};     // protected by lock; one lease per mapping backing
    uint64_t secretSpace = 0;  // creator address-space identity, or 0 for normal memfd
};

uint64_t NextMemFdInode();

static inline void MfdLock(MemFdData* mfd)
{
    if (Process* self = ProcessCurrent()) SchedulerKernelRegionEnter(self);
    while (__atomic_exchange_n(&mfd->lock, 1, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
}

static inline void MfdUnlock(MemFdData* mfd)
{
    __atomic_store_n(&mfd->lock, 0, __ATOMIC_RELEASE);
    if (Process* self = ProcessCurrent()) SchedulerKernelRegionLeave(self);
}

bool MemFdGrowLocked(MemFdData* mfd, uint64_t needed);
bool MemFdGrow(MemFdData* mfd, uint64_t needed);
PhysicalAddress MemFdGetOrAllocPage(MemFdData* mfd, uint64_t pageIdx, uint16_t pid);

static inline void MemFdRef(MemFdData* mfd)
{
    if (mfd) __atomic_fetch_add(&mfd->refCount, 1, __ATOMIC_RELEASE);
}

static inline void MemFdUnref(MemFdData* mfd)
{
    if (!mfd) return;
    uint32_t prev = __atomic_fetch_sub(&mfd->refCount, 1, __ATOMIC_ACQ_REL);
    if (prev <= 1) {
        if (mfd->pageMap) {
            for (uint64_t i = 0; i < mfd->pageMapCount; i++) {
                if (mfd->pageMap[i]) PmmFreePage(PhysicalAddress(mfd->pageMap[i]));
            }
            kfree(mfd->pageMap);
        }
        kfree(mfd);
    }
}

void MemFdReleaseMapping(void* object);
int MemFdInstall(Process* proc, MemFdData* mfd, uint32_t flags);
// `user` selects a user address (checked copies, never under MfdLock) or a
// trusted kernel buffer passed as an address.
int64_t MemFdReadAt(MemFdData* mfd, uint64_t destination, uint64_t count, uint64_t* offset, bool user);
int64_t MemFdWriteAt(MemFdData* mfd, uint64_t source, uint64_t count,
                     uint64_t* offset, bool append, bool user);

int NamedShmOpen(Process* proc,const char* name,uint32_t flags);
int NamedShmUnlink(const char* name);

} // namespace brook
