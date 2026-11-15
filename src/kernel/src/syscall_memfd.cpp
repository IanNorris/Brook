// syscall_memfd.cpp -- memfd objects, memfd_create and memfd-backed named shared memory.
//
// Split from syscall.cpp (see docs/agent/syscall-split-plan.md).
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "fw_cfg.h"
#include "user_access_scope.h"
#include "string.h"
#include "memory/virtual_memory.h"
#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_memfd.h"
#include "syscall_handlers.h"

namespace brook {

// memfd constants and data — defined here so sys_read/sys_write/fstat can use them
static constexpr uint32_t MFD_CLOEXEC       = 0x0001u;
static constexpr uint32_t MFD_ALLOW_SEALING = 0x0002u;

uint64_t NextMemFdInode()
{
    static uint64_t next = 1;
    uint64_t candidate=__atomic_load_n(&next,__ATOMIC_RELAXED);
    for(;;) {
        // The high owner bit distinguishes shared futex keys from task IDs.
        // Saturate instead of ever reusing an identity after wraparound.
        if(candidate>=(uint64_t{1}<<62))return 0;
        if(__atomic_compare_exchange_n(&next,&candidate,candidate+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED))return candidate;
    }
}

// Grow pageMap to cover at least `needed` bytes. Does NOT allocate any
// physical pages — those are demand-paged. Caller must hold mfd->lock.
bool MemFdGrowLocked(MemFdData* mfd, uint64_t needed)
{
    if (needed <= mfd->capacity) return true;
    // Cap at 16 GiB of address space so the index array itself stays small
    // (16 GiB / 4 KiB * 8 = 32 MiB index). Past this is almost certainly an
    // app pathology and -ENOMEM is the right answer.
    static constexpr uint64_t MEMFD_MAX = 16ULL * 1024 * 1024 * 1024;
    if (needed > MEMFD_MAX) {
        SerialPrintf("MemFdGrow: needed=%lu > MEMFD_MAX (%lu), returning ENOMEM\n",
                     needed, MEMFD_MAX);
        return false;
    }

    uint64_t newCount = (needed + 4095) / 4096;
    uint64_t bytes = newCount * sizeof(uint64_t);
    auto* newMap = static_cast<uint64_t*>(kmalloc(static_cast<uint32_t>(bytes)));
    if (!newMap) {
        SerialPrintf("MemFdGrow: kmalloc(%lu) FAILED for index of %lu pages\n",
                     bytes, newCount);
        return false;
    }
    memcpy(newMap, mfd->pageMap, mfd->pageMapCount * sizeof(uint64_t));
    memset(newMap + mfd->pageMapCount, 0, (newCount - mfd->pageMapCount) * sizeof(uint64_t));
    if (mfd->pageMap) kfree(mfd->pageMap);
    mfd->pageMap = newMap;
    mfd->pageMapCount = newCount;
    mfd->capacity = newCount * 4096;
    return true;
}

bool MemFdGrow(MemFdData* mfd, uint64_t needed)
{
    MfdLock(mfd);
    bool ok = MemFdGrowLocked(mfd, needed);
    MfdUnlock(mfd);
    return ok;
}

// Get-or-allocate the physical page backing page index `pageIdx`. Returns
// PhysicalAddress(0) on OOM or out-of-range. New pages are zero-filled.
PhysicalAddress MemFdGetOrAllocPage(MemFdData* mfd, uint64_t pageIdx, uint16_t pid)
{
    (void)pid; // Backing belongs to the object, which can outlive its creator.
    MfdLock(mfd);
    if (pageIdx >= mfd->pageMapCount) { MfdUnlock(mfd); return PhysicalAddress(0); }
    uint64_t raw = mfd->pageMap[pageIdx];
    if (raw == 0) {
        PhysicalAddress phys = PmmAllocPage(MemTag::User, 0);
        if (!phys) { MfdUnlock(mfd); return PhysicalAddress(0); }
        auto* va = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
        memset(va, 0, 4096);
        raw = phys.raw();
        mfd->pageMap[pageIdx] = raw;
    }
    MfdUnlock(mfd);
    return PhysicalAddress(raw);
}

// A backing can be shared by forked/split VMAs. Only its final destruction
// retires admission, after all mapping/fault snapshots have released it.
void MemFdReleaseMapping(void* object) {
    auto* mfd=static_cast<MemFdData*>(object);
    MfdLock(mfd);if(!mfd->seals.mappings)__builtin_trap();--mfd->seals.mappings;MfdUnlock(mfd);
    MemFdUnref(mfd);
}

// Publish a complete independent descriptor while retaining object ownership
// under fdLock, so a sibling close cannot race partially initialized flags.
int MemFdInstall(Process* proc, MemFdData* mfd, uint32_t flags)
{
    if (!proc || !proc->fds) return -EMFILE;
    FdEntry entry{};
    entry.type = FdType::MemFd;
    entry.handle = mfd;
    entry.statusFlags = flags & ~(0x80000u | 0x40u | 0x80u | 0x200u);
    entry.fdFlags = (flags & 0x80000u) ? 1 : 0;
    int fd = FdTableDuplicatePinned(proc->fds, ProcessFdLock(proc), &entry, 0,
        entry.fdFlags, [](FdEntry* e) { MemFdRef(static_cast<MemFdData*>(e->handle)); });
    return fd < 0 ? -EMFILE : fd;
}

int64_t MemFdReadAt(MemFdData* mfd, void* buffer, uint64_t count, uint64_t* offset)
{
    MfdLock(mfd);
    uint64_t pos = *offset;
    uint64_t available = pos < mfd->size ? mfd->size - pos : 0;
    uint64_t total = count < available ? count : available;
    auto* dst = static_cast<uint8_t*>(buffer);
    for (uint64_t done = 0; done < total;) {
        uint64_t index = pos / 4096, inPage = pos & 4095;
        uint64_t n = 4096 - inPage;
        if (n > total - done) n = total - done;
        uint64_t raw = index < mfd->pageMapCount ? mfd->pageMap[index] : 0;
        if (raw) memcpy(dst + done, reinterpret_cast<void*>(PhysToVirt(PhysicalAddress(raw)).raw() + inPage), n);
        else memset(dst + done, 0, n);
        pos += n; done += n;
    }
    *offset = pos;
    MfdUnlock(mfd);
    return total;
}

int64_t MemFdWriteAt(MemFdData* mfd, const void* buffer, uint64_t count,
                     uint64_t* offset, bool append)
{
    if (!count) return 0;
    MfdLock(mfd);
    uint64_t pos = append ? mfd->size : *offset;
    if (pos > 0x7fffffffffffffffULL || count > 0x7fffffffffffffffULL - pos) { MfdUnlock(mfd); return -EINVAL; }
    if(int error=MemFdSealedWrite(mfd->seals,mfd->size,pos+count)) {MfdUnlock(mfd);return error;}
    if (!MemFdGrowLocked(mfd, pos + count)) { MfdUnlock(mfd); return -ENOMEM; }
    auto* src = static_cast<const uint8_t*>(buffer);
    uint64_t done = 0;
    while (done < count) {
        uint64_t index = pos / 4096, inPage = pos & 4095;
        uint64_t n = 4096 - inPage;
        if (n > count - done) n = count - done;
        uint64_t raw = mfd->pageMap[index];
        if (!raw) {
            PhysicalAddress page = PmmAllocPage(MemTag::User, 0);
            if (!page) break;
            raw = page.raw();
            memset(reinterpret_cast<void*>(PhysToVirt(page).raw()), 0, 4096);
            mfd->pageMap[index] = raw;
        }
        memcpy(reinterpret_cast<void*>(PhysToVirt(PhysicalAddress(raw)).raw() + inPage), src + done, n);
        pos += n; done += n;
    }
    if (pos > mfd->size) mfd->size = pos;
    *offset = pos;
    MfdUnlock(mfd);
    return done || !count ? static_cast<int64_t>(done) : -ENOMEM;
}

// External wrappers for fork() in process.cpp, which doesn't see MemFdData.
void MemFdHandleRef(void* handle) { MemFdRef(static_cast<MemFdData*>(handle)); }
void MemFdHandleUnref(void* handle) { MemFdUnref(static_cast<MemFdData*>(handle)); }
void MemFdHandleStat(void* handle, VnodeStat* out)
{
    auto* mfd = static_cast<MemFdData*>(handle);
    *out = {};
    MfdLock(mfd);
    out->size = mfd->size;
    for (uint64_t index = 0; index < mfd->pageMapCount; ++index)
        if (mfd->pageMap[index]) out->blocks += 4096 / 512;
    out->ino = mfd->inode;
    MfdUnlock(mfd);
    out->dev = 0x01000000;
    out->nlink = 0;
    out->mode = 0100666;
    out->exactDescriptorMetadata = true;
}

// A name owns one backing reference independently of descriptors/mappings.
// Open and unlink serialize the name-to-object binding; final backing release
// happens after dropping this namespace lock. Names are dynamically allocated.
struct NamedShmEntry { char name[256]; MemFdData* backing; NamedShmEntry* next; };
static NamedShmEntry* g_namedShm;
static KMutex g_namedShmLock{};
struct NamedShmGuard {
    NamedShmGuard() { KMutexLock(&g_namedShmLock); }
    ~NamedShmGuard() { KMutexUnlock(&g_namedShmLock); }
};
static int NamedShmValidate(const char* name) {
    const uint64_t length=strlen(name);
    if(!length)return -EINVAL;
    if(length>=sizeof(NamedShmEntry::name))return -36;
    if(StrEq(name,".") || StrEq(name,".."))return -21; // EISDIR
    for(uint64_t i=0;i<length;++i)if(name[i]=='/')return -EINVAL;
    return 0;
}
int NamedShmOpen(Process* proc,const char* name,uint32_t flags) {
    KernelOperationGuard operation(proc);
    int error=NamedShmValidate(name);if(error)return error;
    if((flags&3)==3)return -EINVAL;
    if(flags&0x10000u)return -ENOTDIR;
    if(flags&0x200u)return -95; // mapped truncate needs coordinated invalidation
    NamedShmEntry* discard=nullptr;int fd;
    {
        NamedShmGuard held;
        NamedShmEntry* entry=g_namedShm;
        while(entry && !StrEq(entry->name,name))entry=entry->next;
        const bool create=(flags&0x40u)!=0;
        if(entry && create && (flags&0x80u))return -17;
        if(!entry && !create)return -ENOENT;
        const bool fresh=!entry;
        if(fresh) {
            entry=static_cast<NamedShmEntry*>(kmalloc(sizeof(NamedShmEntry)));
            if(!entry)return -ENOMEM;
            auto* backing=static_cast<MemFdData*>(kmalloc(sizeof(MemFdData)));
            if(!backing) { kfree(entry);return -ENOMEM; }
            *backing={NextMemFdInode(),nullptr,0,0,0,0,1};
            if(!backing->inode) {kfree(backing);kfree(entry);return -75;}
            memcpy(entry->name,name,strlen(name)+1);
            entry->backing=backing;entry->next=g_namedShm;g_namedShm=entry;
        }
        fd=MemFdInstall(proc,entry->backing,flags);
        if(fd<0 && fresh) { g_namedShm=entry->next;discard=entry; }
    }
    if(discard) { MemFdUnref(discard->backing);kfree(discard); }
    return fd;
}
int NamedShmUnlink(const char* name) {
    KernelOperationGuard operation(ProcessCurrent());
    int error=NamedShmValidate(name);if(error)return error;
    NamedShmEntry* removed;
    {
        NamedShmGuard held;auto** link=&g_namedShm;
        while(*link && !StrEq((*link)->name,name))link=&(*link)->next;
        if(!*link)return -ENOENT;
        removed=*link;*link=removed->next;
    }
    MemFdUnref(removed->backing);kfree(removed);
    return 0;
}

// ---------------------------------------------------------------------------
// sys_memfd_create (319) — anonymous in-memory file
// ---------------------------------------------------------------------------
//
// Wayland uses memfd for shared memory buffers (wl_shm).
// We implement it as a SyntheticMem fd backed by a heap allocation.

// Disposable-QEMU diagnostic. Samples are globally bounded, never enabled on
// real hardware by default, and may contain application-supplied names.
static bool memfdTraceEnabled;
static uint32_t memfdTraceSequence;
void SyscallConfigureMemfdTrace() {
    char value[2]{};
    const auto n=FwCfgReadFile("opt/memfd-trace",value,sizeof(value));
    memfdTraceEnabled=value[0]=='1' && (n==1 || (n==2 && !value[1]));
}

int64_t sys_memfd_create(uint64_t nameAddr, uint64_t flags,
                           uint64_t, uint64_t, uint64_t, uint64_t)
{
    flags = static_cast<uint32_t>(flags);
    if (flags & ~(MFD_CLOEXEC | MFD_ALLOW_SEALING)) return -EINVAL;
    Process* proc = ProcessCurrent();
    if (!proc) return -EMFILE;

    KernelOperationGuard operation(proc);

    auto* mfd = static_cast<MemFdData*>(kmalloc(sizeof(MemFdData)));
    if (!mfd) return -ENOMEM;
    mfd->inode        = NextMemFdInode();
    if(!mfd->inode) {kfree(mfd);return -75;}
    mfd->pageMap      = nullptr;
    mfd->pageMapCount = 0;
    mfd->size         = 0;
    mfd->capacity     = 0;
    mfd->lock         = 0;
    mfd->refCount     = 0; // bumped by FdAlloc below
    mfd->seals        = {flags&MFD_ALLOW_SEALING?0u:1u,0};

    int fd = MemFdInstall(proc, mfd, 2u | ((flags & MFD_CLOEXEC) ? 0x80000u : 0));
    if (fd < 0) kfree(mfd);

    if (memfdTraceEnabled) {
        uint32_t previous=__atomic_load_n(&memfdTraceSequence,__ATOMIC_RELAXED);
        while (previous<4096 && !__atomic_compare_exchange_n(&memfdTraceSequence,
               &previous,previous+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
        const auto sample=previous<4096?previous+1:0;
        if (sample && (sample<=32 || !(sample%32))) {
            char name[64]{};
            UserAccessScope access(proc);
            if (!CopyUserCString(nameAddr,name,sizeof(name)))
                memcpy(name,"<unavailable>",sizeof("<unavailable>"));
            for (char& ch:name) if (ch && (ch<' ' || ch>'~')) ch='?';
            SerialPrintf("MEMFD_TRACE n=%u pid=%u tgid=%u fd=%d rip=%lx name=%s\n",
                         sample,proc->pid,proc->tgid,fd,proc->syscallUserRip,name);
        }
    }

    return fd;
}

} // namespace brook
