// syscall_memory.cpp -- memory syscalls: mmap/munmap/mprotect/mremap/brk/madvise/mincore and mapping faults.
//
// Split from syscall.cpp.
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "cpu.h"
#include "user_access_scope.h"
#include "drm_map_tokens.h"
#include "intel_bdw_buffer.h"
#include "string.h"
#include "memory/file_page_cache.h"
#include "memory/file_object.h"
#include "memory/user_mapping.h"
#include "memory/pageable_memory.h"
#include "memory/mapping_fault_test.h"
#include "tty.h"
#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_memfd.h"
#include "syscall_drm.h"
#include "syscall_memory.h"
#include "syscall_handlers.h"

namespace brook {

// User #PF demand-paging hook for memfd MAP_SHARED mappings. Returns true
// if the fault was a memfd-backed page that we just paged in; false if
// this fault is unrelated (caller should fall through to the kill path).
extern "C" bool MemFdHandleUserFault(uint64_t cr2, uint64_t errCode);
extern "C" bool FileMapHandleUserFault(uint64_t cr2, uint64_t errCode);

struct FileMappingBacking {
    Vnode* vnode;
    uint64_t size;
    bool pinned;
};
// Historical fault entrypoints share one resolver: the mapping's backing kind
// may change while preparation sleeps, so retries must dispatch afresh.

extern "C" bool MemFdHandleUserFault(uint64_t address,uint64_t error) {
    return ResolveUserMappingFault(address,error)==MappingFaultResult::Resolved;
}

// ---------------------------------------------------------------------------
// File Page Cache — global cache of clean file-backed pages.
// Key: (globally unique vnode cacheId, file_page_index). Maps to a physical page.
// Used by demand-paged MAP_PRIVATE file mappings to share identical read-only
// pages across processes and enable 64KB read-ahead batches.
// ---------------------------------------------------------------------------

// Preload file pages into the cache in bulk.
// Reads directly into physical pages via 64KB VfsRead chunks, avoiding a
// single large bounce buffer. Handles files up to 4MB.
static uint32_t PageCachePreload(Vnode* vn, uint64_t startPageIdx,
                                  uint64_t fileSize, uint32_t maxPages)
{
    // FileObject-backed faults use that inode's coherent resident cache and
    // never consult this legacy cache. Preloading here would create unused
    // duplicate pages in an unrelated cache.
    // Uncached vnodes likewise must not read batches that Adopt will discard.
    if (!vn->cacheId || (vn->ops->file_object && vn->ops->file_object(vn))) return 0;
    static constexpr uint32_t MAX_PRELOAD_PAGES = 4096; // 16MB
    static constexpr uint32_t CHUNK_PAGES = 16;         // 64KB per VfsRead

    if (maxPages > MAX_PRELOAD_PAGES) maxPages = MAX_PRELOAD_PAGES;

    // Cap by file size
    uint64_t maxFilePages = (fileSize + 4095) / 4096;
    if (startPageIdx >= maxFilePages) return 0;
    if (startPageIdx + maxPages > maxFilePages)
        maxPages = static_cast<uint32_t>(maxFilePages - startPageIdx);

    // Fast check: if first page is already cached, entire file likely is
    PhysicalAddress probe = FilePageCacheLookup(vn, startPageIdx);
    if (probe) { PmmUnrefPage(probe); return maxPages; }

    // 64KB bounce buffer for disk reads
    auto* readBuf = static_cast<uint8_t*>(kmalloc(CHUNK_PAGES * 4096));
    if (!readBuf) return 0;

    uint32_t totalCached = 0;

    for (uint32_t base = 0; base < maxPages; base += CHUNK_PAGES) {
        uint32_t chunkPages = maxPages - base;
        if (chunkPages > CHUNK_PAGES) chunkPages = CHUNK_PAGES;

        // Allocate physical pages for this chunk
        PhysicalAddress pages[CHUNK_PAGES];
        for (uint32_t i = 0; i < chunkPages; ++i) {
            pages[i] = PmmAllocPageAccounted(MemTag::User, KernelPid, PmmOwner::Shmem);
            if (!pages[i]) { chunkPages = i; break; }
        }
        if (chunkPages == 0) break;

        // Bulk read from disk
        uint64_t off = (startPageIdx + base) * 4096;
        uint64_t chunkBytes = static_cast<uint64_t>(chunkPages) * 4096;
        __builtin_memset(readBuf, 0, chunkBytes);
        int got = VfsRead(vn, readBuf, chunkBytes, &off);
        // Cache only complete verified file pages. A reader may return a
        // verified prefix before a later block fails; errors/short reads must
        // never become zero-filled cached data that hides a subsequent EIO.
        uint32_t cachedThisChunk = 0;
        for (uint32_t i = 0; i < chunkPages; ++i) {
            const uint64_t srcOff = i * 4096;
            const uint64_t fileOff = (startPageIdx + base + i) * 4096;
            const uint64_t required = fileSize - fileOff < 4096 ? fileSize - fileOff : 4096;
            if (got < 0 || static_cast<uint64_t>(got) < srcOff + required) {
                PmmUnrefPage(pages[i]);
                continue;
            }
            auto* kp = reinterpret_cast<uint8_t*>(PhysToVirt(pages[i]).raw());
            if (srcOff < static_cast<uint64_t>(got)) {
                uint64_t toCopy = static_cast<uint64_t>(got) - srcOff;
                if (toCopy > 4096) toCopy = 4096;
                __builtin_memcpy(kp, readBuf + srcOff, toCopy);
                if (toCopy < 4096)
                    __builtin_memset(kp + toCopy, 0, 4096 - toCopy);
            } else {
                __builtin_memset(kp, 0, 4096);
            }
            // Preload needs only the cache's reference, not a caller alias.
            auto canonical = FilePageCacheAdopt(vn, startPageIdx + base + i, pages[i], false);
            if (canonical) { PmmUnrefPage(canonical); ++cachedThisChunk; }
        }
        totalCached += cachedThisChunk;
        if (got < 0 || static_cast<uint64_t>(got) < chunkBytes) break;
    }

    kfree(readBuf);
    __atomic_fetch_add(&g_profPreloadPages, totalCached, __ATOMIC_RELAXED);
    return totalCached;
}

// Demand-page the current user mapping. File VMAs own a Vnode reference,
// so the backing remains valid even after userspace closes the original fd.
// Uses the global page cache for sharing and 64KB read-ahead.
// No metadata pointer survives unlocking. The transaction owns both prepared
// frames and the immutable backing pin through I/O, validation and retirement.
MappingFaultResult ResolveMappingFaultForSpace(Process* actor,AddressSpaceState* space,uint64_t address,uint64_t error,uint32_t retryBudget,bool allowPageableReclaim) {
    if (error & 1) return MappingFaultResult::Unhandled;
    if (!actor || !space || space->kernel) return MappingFaultResult::Unhandled;
    const uint64_t page=address&~uint64_t{4095};
    const uint32_t access=(error&2)?2u:((error&16)?4u:1u);
    for(uint32_t attempt=0;!retryBudget || attempt<retryBudget;++attempt) {
        MappingTransaction operation(actor,space);
        if(!operation.LockFaultRange(page))return MappingFaultResult::Unhandled;MappingFaultSnapshot snapshot{};
        if (!operation.CaptureRange(page,access,snapshot)) return MappingFaultResult::Unhandled;
        if(snapshot.range.backing && snapshot.range.backing->file && snapshot.range.backing->file->mappingError)
            return MappingFaultResult::IoError;
        // A peer may have resolved this fault or replaced its range while we
        // prepared I/O. Use the current range and leaf, not the original fault
        // backend. Returning retries the instruction (including a subsequent
        // ordinary CoW fault), without publishing any stale prepared page.
        const uint64_t leaf=VmmReadLeaf(operation.Space()->pageTable,VirtualAddress(page));
        if (leaf & VMM_PRESENT) {
            const bool cow=(leaf&PTE_COW_BIT) && snapshot.range.sharing==MappingSharing::Private &&
                snapshot.range.leafOwnership==MappingLeafOwnership::Managed;
            return ((leaf&VMM_USER) && (!(access&4) || !(leaf&VMM_NO_EXEC)) &&
                (!(access&2) || (leaf&VMM_WRITABLE) || cow))?MappingFaultResult::Resolved:MappingFaultResult::Unhandled;
        }
        const auto kind=snapshot.range.kind;
        if(kind==MappingKind::Pageable) {
            operation.Unlock();
            return PageableMemoryFault(actor,space,page,access,
                actor->addressSpace==space && (allowPageableReclaim || UserAccessScope::CoversCurrentPage(page)));
        }
        if (kind!=MappingKind::File && kind!=MappingKind::MemFd && kind!=MappingKind::Anonymous &&
            kind!=MappingKind::Stack && kind!=MappingKind::Heap && kind!=MappingKind::Image) return MappingFaultResult::Unhandled;
        const uint64_t batch=kind==MappingKind::File?FILE_OBJECT_PAGE_BATCH:1;
        operation.Unlock();
        if (!operation.Prepare(0,batch,1)) return MappingFaultResult::OutOfMemory;
        bool cachedPrivate=false;
        auto prepared=MappingFaultResult::Resolved;
        if (!MappingFaultTestBarrier(operation.Space(),page,1)) return MappingFaultResult::Unhandled;
        if (kind==MappingKind::File) {
            const auto* file=static_cast<FileMappingBacking*>(snapshot.range.backing->object);
            FileObject* object=file->vnode->ops->file_object ? file->vnode->ops->file_object(file->vnode) : nullptr;
            if(object) {
                // These references remain cancellation-owned until staged;
                // all I/O and target-file locking precede AS publication.
                struct BatchPins {
                    PhysicalAddress pages[FILE_OBJECT_PAGE_BATCH]{};
                    ~BatchPins() { for(auto physical:pages)if(physical)PmmUnrefPage(physical); }
                } pins;
                const uint64_t offset=snapshot.range.offset+page-snapshot.range.begin;
                uint64_t remaining=(snapshot.range.end-page)/4096;
                const uint32_t count=remaining<batch?static_cast<uint32_t>(remaining):batch;
                bool loaded=false;
                prepared=FileObjectFaultPages(object,file->vnode,offset/4096,count,pins.pages,&loaded);
                for(uint32_t i=0;i<count;++i)if(pins.pages[i]) {
                    __atomic_fetch_add(i==0&&loaded?&g_profCacheMiss:&g_profCacheHit,1,__ATOMIC_RELAXED);
                    if(!operation.StagePage(i,pins.pages[i],true))return MappingFaultResult::Unhandled;
                    pins.pages[i]=PhysicalAddress{}; // transaction owns the pin
                }
            } else {
              VnodeStat stat{};
              const int statResult=VfsStat(file->vnode,&stat);
              if(statResult)prepared=statResult==-ENOMEM?MappingFaultResult::OutOfMemory:MappingFaultResult::IoError;
              for (uint64_t i=0;prepared==MappingFaultResult::Resolved && i<batch && page+i*4096<snapshot.range.end;++i) {
                const uint64_t offset=snapshot.range.offset+(page+i*4096-snapshot.range.begin);
                if (offset>=stat.size) { if (!i)prepared=MappingFaultResult::EndOfFile;break; }
                auto physical=FilePageCacheLookup(file->vnode,offset/4096);
                if (physical) {
                    __atomic_fetch_add(&g_profCacheHit,1,__ATOMIC_RELAXED);
                    if (!operation.StagePage(i,physical,true)) { PmmUnrefPage(physical);return MappingFaultResult::Unhandled; }
                } else if (!i) {
                    __atomic_fetch_add(&g_profCacheMiss,1,__ATOMIC_RELAXED);
                    physical=PmmAllocPage(MemTag::User,operation.Space()->allocationPid);
                    if (!physical) { prepared=MappingFaultResult::OutOfMemory;break; }
                    auto* bytes=reinterpret_cast<void*>(PhysToVirt(physical).raw());memset(bytes,0,4096);
                    uint64_t readOffset=offset;
                    const int readResult=VfsRead(file->vnode,bytes,4096,&readOffset);
                    const uint64_t expected=stat.size-offset<4096?stat.size-offset:4096;
                    if (readResult<0 || static_cast<uint64_t>(readResult)!=expected) {
                        PmmUnrefPage(physical);
                        prepared=readResult==-ENOMEM?MappingFaultResult::OutOfMemory:MappingFaultResult::IoError;break;
                    }
                    // Preparation is cancellation-protected by the transaction.
                    // Stage the canonical winner only after competing inserts
                    // converge, rather than retaining an uncached losing frame.
                    physical=FilePageCacheAdopt(file->vnode,offset/4096,physical,
                        snapshot.range.sharing==MappingSharing::Private ||
                        !(snapshot.range.maximumProtection&2));
                    if (!physical) { prepared=MappingFaultResult::OutOfMemory;break; }
                    if (!operation.StagePage(i,physical,true)) { PmmUnrefPage(physical);return MappingFaultResult::Unhandled; }
                }
              }
            }
            cachedPrivate=snapshot.range.sharing==MappingSharing::Private;
        } else if (kind==MappingKind::MemFd) {
            auto* backing=static_cast<MemFdData*>(snapshot.range.backing->object);
            const uint64_t offset=snapshot.range.offset+page-snapshot.range.begin;
            if (offset>=__atomic_load_n(&backing->size,__ATOMIC_ACQUIRE)) return MappingFaultResult::Unhandled;
            auto physical=MemFdGetOrAllocPage(backing,offset/4096,space->allocationPid);
            if (!physical || !operation.StagePage(0,physical,false)) return MappingFaultResult::Unhandled;
        } else if (!operation.PrepareZeroed(MemTag::User,operation.Space()->allocationPid)) return MappingFaultResult::Unhandled;
        if (!MappingFaultTestBarrier(operation.Space(),page,2)) return MappingFaultResult::Unhandled;
        if(!operation.LockFaultRange(page))return MappingFaultResult::OutOfMemory;
        if (!operation.ValidateRange(snapshot,page,access)) continue;
        // EOF/IO observations are only authoritative for this same VMA and file
        // generation. A remap, protection change or resize retries preparation.
        if(prepared!=MappingFaultResult::Resolved)return prepared;
        auto result=operation.PublishFault(snapshot,0,VirtualAddress(page),access,cachedPrivate);
        if (result==MappingFaultCommit::Stale) continue;
        if (result==MappingFaultCommit::Failed) return MappingFaultResult::Unhandled;
        for (uint64_t i=1;i<batch;++i)
            if (operation.PreparedPage(i)) (void)operation.PublishFault(snapshot,i,VirtualAddress(page+i*4096),access,cachedPrivate);
        return MappingFaultResult::Resolved;
    }
    return MappingFaultResult::Unhandled; // bounded remote cleanup under remap churn
}
MappingFaultResult ResolveUserMappingFault(uint64_t address,uint64_t error,UserFaultOrigin origin) {
    auto* actor=ProcessCurrent();
    // Experimental paging never performs disk I/O from an arbitrary kernel
    // #PF. Safe usercopy retries explicitly supply the user-access error bit.
    if(actor && actor->addressSpace && actor->addressSpace->pagingAdmission.Exclusive() && !(error&4))
        return MappingFaultResult::Unhandled;
    return ResolveMappingFaultForSpace(actor,actor?actor->addressSpace:nullptr,address,error,0,
        origin==UserFaultOrigin::UserInstruction);
}
extern "C" bool FileMapHandleUserFault(uint64_t address,uint64_t error) {
    return ResolveUserMappingFault(address,error)==MappingFaultResult::Resolved;
}

// ---------------------------------------------------------------------------
// sys_brk (12)
// ---------------------------------------------------------------------------

int64_t sys_brk(uint64_t requested,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t) {
    Process* proc=ProcessCurrent();if (!proc) return -ENOMEM;
    return UserMappingBreak(proc,proc->addressSpace,requested);
}

// ---------------------------------------------------------------------------
// sys_mmap (9)
// ---------------------------------------------------------------------------

struct DeviceMappingBacking {
    uint64_t kernelBase, physicalBase;
    bool borrowedPhysical;
    MappingBacking* resourceLifetime;
    brook::BdwGpuBuffer* intelBuffer;
    uint64_t intelPageOffset;
};
static void ReleaseFileMapping(void* object) {
    auto* backing = static_cast<FileMappingBacking*>(object);
    if (backing->pinned) VfsFileMapUnpin(backing->vnode);
    VnodeUnref(backing->vnode); kfree(backing);
}
static int PrepareAnonymousMapping(MappingTransaction& operation, const UserMappingRequest&, void*) {
    return operation.PrepareZeroed(MemTag::User,operation.Space()->allocationPid) ? 0 : -ENOMEM;
}
static int PrepareFileMapping(MappingTransaction& operation, const UserMappingRequest& request, void*) {
    if (!operation.PrepareZeroed(MemTag::User,operation.Space()->allocationPid)) return -ENOMEM;
    auto* backing = static_cast<FileMappingBacking*>(request.range.backing->object);
    uint64_t offset = request.range.offset;
    for (uint64_t page=0;page<request.preparedPages;++page) {
        auto* bytes = reinterpret_cast<uint8_t*>(PhysToVirt(operation.PreparedPage(page)).raw());
        int got = VfsRead(backing->vnode,bytes,4096,&offset);
        if (got < 0) return got;
        if (got < 4096) break; // candidates are already zeroed, including EOF tails
    }
    return 0;
}
static int PrepareDeviceMapping(MappingTransaction& operation, const UserMappingRequest& request, void*) {
    const auto* source = static_cast<DeviceMappingBacking*>(request.range.backing->object);
    for (uint64_t page=0;page<request.preparedPages;++page) {
        const uint64_t kernelAddress=source->intelBuffer ?
            brook::BdwGpuBufferPageKernelAddress(source->intelBuffer,static_cast<uint32_t>(source->intelPageOffset+page)) :
            source->kernelBase+page*4096;
        auto physical = source->borrowedPhysical ? PhysicalAddress(source->physicalBase+page*4096) :
            VmmVirtToPhys(KernelPageTable,VirtualAddress(kernelAddress));
        if (!physical) return -ENOMEM;
        if (!source->borrowedPhysical) PmmRefPage(physical);
        if (!operation.StagePage(page,physical,!source->borrowedPhysical)) {
            if (!source->borrowedPhysical) PmmUnrefPage(physical);
            return -ENOMEM;
        }
    }
    return 0;
}
static void ReleaseDeviceMapping(void* object) {
    auto* source=static_cast<DeviceMappingBacking*>(object);
    brook::BdwGpuBufferTrackUserRef(source->intelBuffer,brook::BdwBufferUserRef::Mapping,false);
    MappingBackingUnref(source->resourceLifetime);kfree(source);
}
int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                  uint64_t flags, uint64_t fd, uint64_t offset)
{
    __atomic_fetch_add(&g_profMmapCount,1,__ATOMIC_RELAXED);
    constexpr uint64_t userEnd = 0x0000800000000000ULL;
    const bool exactAddress = (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0;
    if (!length || length > userEnd || (prot & ~7ULL) ||
        (!(flags & MAP_PRIVATE) == !(flags & MAP_SHARED)) ||
        (exactAddress && ((addr & 4095) || addr < VIRTUAL_NULL_GUARD || addr>=userEnd || length > userEnd-addr)) ||
        (!(flags & MAP_ANONYMOUS) && (offset & 4095))) return -EINVAL;
    length = (length+4095)&~uint64_t{4095};
    if (!(flags & MAP_ANONYMOUS) && offset > ~uint64_t{0}-length) return -EINVAL;
    Process* proc = ProcessCurrent(); if (!proc) return -ESRCH;
    KernelOperationGuard protectedOperation(proc);
    MappingBackingGuard backing;
    UserMappingRequest request{};
    request.hint = addr; request.length = length; request.fixed = exactAddress;
    request.noReplace = (flags & MAP_FIXED_NOREPLACE) != 0;
    request.low32 = (flags & MAP_32BIT) != 0 && !exactAddress;
    request.range = {0,length,offset,0,nullptr,static_cast<uint32_t>(prot),7,MappingKind::Anonymous,
                     (flags & MAP_SHARED) ? MappingSharing::Shared : MappingSharing::Private,MappingLeafOwnership::Managed};
    if (flags & MAP_ANONYMOUS) {
        request.range.offset = 0;
        if (flags & MAP_SHARED) {
            auto* mfd = static_cast<MemFdData*>(kmalloc(sizeof(MemFdData)));
            if (!mfd) return -ENOMEM;
            memset(mfd,0,sizeof(*mfd));mfd->inode=NextMemFdInode();if(!mfd->inode) {kfree(mfd);return -75;}mfd->refCount=1;mfd->seals={};
            if (!MemFdGrow(mfd,length)) { MemFdUnref(mfd); return -ENOMEM; }
            mfd->size=length;
            if(int error=MemFdAdmitMapping(mfd->seals,true,static_cast<uint32_t>(prot),&request.range.maximumProtection)) {MemFdUnref(mfd);return error;}
            backing.value=MappingBackingCreate(mfd,MemFdReleaseMapping);
            if (!backing.value) { MemFdReleaseMapping(mfd); return -ENOMEM; }
            request.range.kind=MappingKind::MemFd;request.range.leafOwnership=MappingLeafOwnership::Borrowed;
            request.range.backing=backing.value;
        } else if (prot && (flags & MAP_POPULATE)) {
            // Ordinary private anonymous mappings reserve address space;
            // ResolveMappingFault supplies zeroed pages on first access.
            // Keep explicit population requests eager.
            request.preparedPages=length/4096;request.prepare=PrepareAnonymousMapping;
        }
        int64_t pageableResult=0;
        if(PageableMemoryMapAnonymous(proc,request,flags,pageableResult))return pageableResult;
        return UserMappingCreate(proc,proc->addressSpace,request);
    }
    FdRef descriptor(proc,static_cast<int>(fd));
    if (!descriptor) return -EBADF;
    auto* entry=descriptor.get();
    if(entry->type==FdType::Path)return -EBADF;
    if (entry->type == FdType::MemFd && entry->handle) {
        if ((FdStatus(entry) & 3) == 1 ||
            ((flags & MAP_SHARED) && (prot & 2) && (FdStatus(entry) & 3) == 0)) return -13;
        if ((flags & MAP_PRIVATE) && (prot & 2)) return -95;
        auto* mfd=static_cast<MemFdData*>(entry->handle);
        uint32_t maximum=5u|(((flags & MAP_SHARED) && (FdStatus(entry) & 3) != 0)?2u:0u);
        MfdLock(mfd);
        if((flags&MAP_SHARED) && (prot&2) && (mfd->seals.bits&8)) {MfdUnlock(mfd);return -EPERM;}
        if (!MemFdGrowLocked(mfd,offset+length)) {MfdUnlock(mfd);return -ENOMEM;}
        int admitted=MemFdAdmitMapping(mfd->seals,flags&MAP_SHARED,static_cast<uint32_t>(prot),&maximum);
        if(admitted) {MfdUnlock(mfd);return admitted;}
        MemFdRef(mfd);MfdUnlock(mfd);
        backing.value=MappingBackingCreate(mfd,MemFdReleaseMapping);
        if (!backing.value) { MemFdReleaseMapping(mfd);return -ENOMEM; }
        request.range.kind=MappingKind::MemFd;request.range.leafOwnership=MappingLeafOwnership::Borrowed;
        request.range.maximumProtection=maximum;
        request.range.backing=backing.value;
        return UserMappingCreate(proc,proc->addressSpace,request);
    }
    if (entry->type == FdType::DevDri || entry->type == FdType::DevIntelDri || entry->type == FdType::DevFramebuf) {
        auto* source=static_cast<DeviceMappingBacking*>(kmalloc(sizeof(DeviceMappingBacking)));
        if (!source) return -ENOMEM;
        *source={};backing.value=MappingBackingCreate(source,ReleaseDeviceMapping);
        if (!backing.value) { kfree(source);return -ENOMEM; }
        uint64_t available=0;
        if(entry->type == FdType::DevIntelDri){
            if(!(flags&MAP_SHARED))return -EINVAL;
            auto* file=static_cast<brook::BdwGpuFile*>(entry->handle);uint64_t relative=0;
            auto* buffer=brook::BdwGpuFileGetMapping(file,offset,request.length,relative);
            if(!buffer)return -EINVAL;
            source->resourceLifetime=brook::BdwGpuBufferBacking(buffer);
            MappingBackingRef(source->resourceLifetime);
            brook::BdwGpuBufferTrackUserRef(buffer,brook::BdwBufferUserRef::Mapping,true);
            source->intelBuffer=buffer;source->intelPageOffset=relative/4096;
            available=brook::BdwGpuBufferBytes(buffer)-relative;
            brook::BdwGpuBufferUnref(buffer);
        } else if (entry->type == FdType::DevDri) {
            auto* context=static_cast<DrmCtx*>(entry->handle);if (!context) return -ENODEV;
            DrmMappingGuard guard(context);
            auto* gem=DrmFindMapToken(context->gem,DRM_MAX_GEM,offset);
            if (!gem) return -EINVAL;
            auto* resource=gem->resource;
            if (resource->hostBlob) {
                const auto& host=resource->hostMapping;
                if (!(flags&MAP_SHARED) || !host.length || !host.physicalBase || request.length>host.length) return -EINVAL;
                switch(host.cache) {
                case 1: break;
                case 2: request.cacheFlags=VMM_CACHE_DISABLE|VMM_WRITE_THRU;break;
                case 3:
                    if (!CpuWriteCombiningReady()) return -95;
                    request.cacheFlags=VMM_LEAF_PAT;break;
                default:return -95;
                }
                source->borrowedPhysical=true;source->physicalBase=host.physicalBase;
            } else if (!resource->backingKva || !resource->backingAttached) return -EINVAL;
            MappingBackingRef(resource->lifetime); // before releasing the GEM lock
            source->resourceLifetime=resource->lifetime;
            source->kernelBase=resource->backingKva;
            available=resource->hostBlob ? resource->hostMapping.length :
                (static_cast<uint64_t>(resource->backingBytes)+4095)&~uint64_t{4095};
        } else {
            uint32_t width,height,stride;uint64_t physical;
            if (!TtyGetFramebufferPhys(&physical,&width,&height,&stride)) return -ENODEV;
            KMutexLock(&proc->framebufferLock);
            MappingBacking* framebuffer=proc->fbBacking;MappingBackingRef(framebuffer);
            source->kernelBase=reinterpret_cast<uint64_t>(proc->fbVirtual);
            available=source->kernelBase ? proc->fbVirtualSize : static_cast<uint64_t>(stride)*height;
            KMutexUnlock(&proc->framebufferLock);
            if (framebuffer) {
                if (request.length>((available+4095)&~uint64_t{4095})) {
                    MappingBackingUnref(framebuffer);
                    return -EINVAL;
                }
                MappingBackingUnref(backing.value);backing.value=framebuffer;
                request.range.offset=0;
                if (!UserMappingConfigureBuffer(request,framebuffer)) return -EINVAL;
                return UserMappingCreate(proc,proc->addressSpace,request);
            }
            source->borrowedPhysical=!source->kernelBase;source->physicalBase=physical;
            // Raw /dev/fb0 must agree with the kernel's physical cache type.
            // Managed per-process VFB RAM keeps its ordinary write-back mapping.
            if(source->borrowedPhysical && ((physical&4095) ||
               !VmmFramebufferCacheFlags(physical,request.length,request.cacheFlags))) return -EINVAL;
            available=(available+4095)&~uint64_t{4095};
        }
        if (!available) return -EINVAL;
        // Returning success for a shorter VMA lets a later mmap occupy the
        // caller's promised extent; munmap(original_length) then destroys it.
        if (request.length > available) return -EINVAL;
        request.range.kind=MappingKind::Device;request.range.sharing=MappingSharing::Shared;
        request.range.offset=0;request.range.backing=backing.value;
        request.range.leafOwnership=source->borrowedPhysical?MappingLeafOwnership::Borrowed:MappingLeafOwnership::Managed;
        request.preparedPages=request.length/4096;request.prepare=PrepareDeviceMapping;
        return UserMappingCreate(proc,proc->addressSpace,request);
    }
    if (entry->type != FdType::Vnode || !entry->handle || static_cast<Vnode*>(entry->handle)->type != VnodeType::File) return -ENODEV;
    if ((FdStatus(entry) & 3) == 1) return -13;
    if ((flags & MAP_SHARED) && (prot & 2) && (FdStatus(entry) & 3) == 0)return -13;
    auto fileFailure=[&](const char* stage,int64_t result) -> int64_t {
        // Only selected errors-only tracing emits this setup-stage evidence.
        // The mapping core reports its own detailed failure after unlocking.
        if (proc->straceEnabled && proc->straceErrorsOnly)
            SerialPrintf("[mmap-file-error:%u] stage=%s error=%ld fd=%lu offset=0x%lx length=%lu prot=0x%lx flags=0x%lx\n",
                         proc->pid,stage,result,fd,offset,length,prot,flags);
        return result;
    };
    auto* vnode=static_cast<Vnode*>(entry->handle);
    if(vnode->ops->deny_mapping)return -95;
    auto* fileObject=vnode->ops->file_object?vnode->ops->file_object(vnode):nullptr;
    if((flags&MAP_SHARED) && (prot&2) && (!fileObject || !vnode->ops->write))return -95;
    int pin=VfsFileMapPin(vnode);if (pin && pin != -95) return fileFailure("mapping-pin",pin);
    VnodeStat stat{};int error=VfsStat(vnode,&stat);
    if (error) { if (!pin) VfsFileMapUnpin(vnode);return fileFailure("stat",error); }
    auto* file=static_cast<FileMappingBacking*>(kmalloc(sizeof(FileMappingBacking)));
    if (!file) { if (!pin) VfsFileMapUnpin(vnode);return fileFailure("file-backing-storage",-ENOMEM); }
    VnodeRef(vnode);*file={vnode,stat.size,pin==0};
    backing.value=MappingBackingCreate(file,ReleaseFileMapping);
    if (!backing.value) { ReleaseFileMapping(file);return fileFailure("backing-descriptor-storage",-ENOMEM); }
    backing.value->file=fileObject;
    backing.value->fileVnode=vnode;
    request.range.kind=MappingKind::File;request.range.backing=backing.value;
    if (flags & MAP_SHARED) request.range.maximumProtection=5u|
        ((fileObject && vnode->ops->write && (FdStatus(entry)&3)!=0)?2u:0u);
    if (pin == -95) { request.preparedPages=length/4096;request.prepare=PrepareFileMapping; }
    else if (length<=16*1024*1024 && stat.size>offset)
        PageCachePreload(vnode,offset/4096,stat.size,static_cast<uint32_t>(length/4096));
    return UserMappingCreate(proc,proc->addressSpace,request);
}
int64_t sys_mprotect(uint64_t addr,uint64_t length,uint64_t prot,uint64_t,uint64_t,uint64_t) {
    Process* proc=ProcessCurrent();if (!proc) return -ESRCH;
    if (prot & ~7ULL) return -EINVAL;
    return UserMappingProtect(proc,proc->addressSpace,addr,length,static_cast<uint32_t>(prot));
}
int64_t sys_msync(uint64_t addr,uint64_t length,uint64_t flags,uint64_t,uint64_t,uint64_t) {
    Process* proc=ProcessCurrent();if(!proc)return -ESRCH;
    return UserMappingSync(proc,proc->addressSpace,addr,length,flags);
}
int64_t sys_munmap(uint64_t addr,uint64_t length,uint64_t,uint64_t,uint64_t,uint64_t) {
    Process* proc=ProcessCurrent();if (!proc) return -ESRCH;
    return UserMappingRemove(proc,proc->addressSpace,addr,length);
}

// ---------------------------------------------------------------------------
// sys_mremap (25) -- resize an existing mapping.  Minimal implementation:
//   - shrink: unmap the tail pages in place, return old_addr
//   - same:   return old_addr
//   - grow:   if MREMAP_MAYMOVE, allocate a new region, copy the old contents
//             page-by-page, unmap the old range, return new address.  Otherwise
//             return -ENOMEM (we don't try to extend in place — the next pages
//             are usually already taken by mmapNext bumping).
// Flags values from <sys/mman.h>: MREMAP_MAYMOVE=1, MREMAP_FIXED=2.
// We don't honour MREMAP_FIXED; if userspace passes new_addr we ignore it.
// ---------------------------------------------------------------------------

static int PrepareUserRemap(const MappingRange& range,uint64_t length,void*) {
    if (range.kind==MappingKind::MemFd) {
        if (range.offset>~uint64_t{0}-length) return -EINVAL;
        return MemFdGrow(static_cast<MemFdData*>(range.backing->object),range.offset+length)?0:-ENOMEM;
    }
    if (range.kind==MappingKind::File && !static_cast<FileMappingBacking*>(range.backing->object)->pinned) return -95;
    return 0;
}
int64_t sys_mremap(uint64_t source,uint64_t oldLength,uint64_t newLength,uint64_t flags,uint64_t destination,uint64_t) {
    Process* proc=ProcessCurrent();if (!proc) return -ESRCH;
    return UserMappingResize(proc,proc->addressSpace,source,oldLength,newLength,flags,destination,PrepareUserRemap,nullptr);
}

// ---------------------------------------------------------------------------
// sys_mincore (27) — report per-page residency for a mapped range.
//
// POSIX/Linux semantics that callers depend on:
//   * addr must be page-aligned            -> else -EINVAL
//   * the whole [addr, addr+length) range must be mapped, otherwise the call
//     fails with -ENOMEM (it does NOT partially fill vec).
//   * vec[i] bit0 = 1 if the page is resident in memory, 0 otherwise.
//
// The old stub ignored addr and reported every page resident with a success
// return. That broke pointer-validity probes that rely on mincore failing for
// unmapped addresses — notably Mesa's _eglPointerIsDereferenceable(), which
// uses mincore() to decide whether a wl_egl_window begins with a wl_surface*
// (old ABI) or an intptr_t version (new ABI). A bogus "resident" answer for the
// null page made Mesa treat the version field as the surface pointer and crash
// in wl_proxy_create_wrapper (BRO-204).
int64_t sys_mincore(uint64_t addr, uint64_t length, uint64_t vecAddr,
                     uint64_t, uint64_t, uint64_t)
{
    if (addr & 0xFFFULL) return -EINVAL;      // addr must be page-aligned

    brook::Process* proc = brook::ProcessCurrent();
    if (!proc) return -ENOMEM;

    uint64_t pages = (length + 4095) / 4096;
    if (pages == 0) return 0;
    if (!UserBufferWritable(vecAddr, pages)) return -EFAULT;

    uint64_t end;
    if (__builtin_add_overflow(addr, pages * 4096ULL, &end)) return -ENOMEM;
    if (end > 0x0000800000000000ULL) return -ENOMEM;

    // First pass: every page in the range must be mapped, or the whole call
    // fails with -ENOMEM (matching Linux — vec is left untouched on failure).
    for (uint64_t i = 0; i < pages; ++i)
    {
        uint64_t pg = addr + i * 4096ULL;
        uint64_t* pte = brook::VmmGetPte(proc->addressSpace->pageTable, brook::VirtualAddress(pg));
        bool present = pte && (*pte & brook::VMM_PRESENT);
        // Lazily-backed regions (demand paging / file maps) count as mapped:
        // fault them in (read intent) so residency reflects the real VMA, as
        // Linux would. 0x4 = user-mode read fault.
        if (!present && (MemFdHandleUserFault(pg, 0x4) || FileMapHandleUserFault(pg, 0x4)))
        {
            pte = brook::VmmGetPte(proc->addressSpace->pageTable, brook::VirtualAddress(pg));
            present = pte && (*pte & brook::VMM_PRESENT);
        }
        if (!present) return -ENOMEM;
    }

    // All mapped pages are treated as resident (no swap). Fill in bounded
    // kernel chunks and copy them out with the checked user copy.
    uint8_t ones[256];
    __builtin_memset(ones, 1, sizeof(ones));
    for (uint64_t done = 0; done < pages;) {
        const uint64_t n = pages - done < sizeof(ones) ? pages - done : sizeof(ones);
        if (!CopyToUser(vecAddr + done, ones, n)) return -EFAULT;
        done += n;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// sys_madvise (28) — private anonymous discard plus optional locality hints
// ---------------------------------------------------------------------------

int64_t sys_madvise(uint64_t address, uint64_t length, uint64_t advice,
                     uint64_t, uint64_t, uint64_t)
{
    auto* proc=ProcessCurrent();
    if (!proc) return -EINVAL;
    if (advice==4) return UserMappingDiscardAnonymous(proc,proc->addressSpace,address,length);
    if (advice==8) return UserMappingAdviseFree(proc,proc->addressSpace,address,length);
    if (advice==10 || advice==11 || advice==18 || advice==19)
        return UserMappingAdviseFork(proc,proc->addressSpace,address,length,uint32_t(advice));
    constexpr uint64_t limit=0x0000800000000000ULL;
    if ((address&4095) || address>=limit || length>limit-address) return -EINVAL;
    // Optional locality / hugepage hints do not promise changed contents.
    if (advice<=3 || advice==14 || advice==15) return 0;
    // REMOVE, guard regions (102/103) and other advice stay EINVAL, as on a
    // kernel without them; callers (glibc stack guards) fall back.
    return -EINVAL;
}

int64_t sys_fadvise64(uint64_t, uint64_t, uint64_t,
                       uint64_t, uint64_t, uint64_t)
{
    return 0; // advisory only
}

// ---------------------------------------------------------------------------
// sys_mbind (237) — NUMA memory binding. We are single-node, so this is
// always a successful no-op. GIMP via libmetis calls this.
// ---------------------------------------------------------------------------

int64_t sys_mbind(uint64_t, uint64_t, uint64_t,
                   uint64_t, uint64_t, uint64_t)
{
    return 0; // single-node — nothing to bind
}

} // namespace brook
