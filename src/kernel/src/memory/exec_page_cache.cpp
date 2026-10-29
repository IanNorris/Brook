#include "exec_page_cache.h"
#include "physical_memory.h"
#include "virtual_memory.h"
#include "string.h"

namespace brook {
namespace {
uint64_t HashPage(PhysicalAddress frame) {
    const auto* bytes=reinterpret_cast<const uint8_t*>(PhysToVirt(frame).raw());
    uint64_t hash=14695981039346656037ULL;
    for(unsigned offset=0;offset<4096;offset+=8) {
        uint64_t word;memcpy(&word,bytes+offset,sizeof(word));
        hash^=word;hash*=1099511628211ULL;
    }
    hash^=hash>>33;hash*=0xff51afd7ed558ccdULL;
    hash^=hash>>33;hash*=0xc4ceb9fe1a85ec53ULL;
    return hash^(hash>>33);
}
}
PhysicalAddress ExecPageCache::Adopt(PhysicalAddress candidate) {
    if(!candidate || !slots_ || !capacity_ || !probes_)return candidate;
    // The owned immutable candidate is pinned; hashing needs no cache lock.
    const uint64_t hash=hasher_?hasher_(candidate):HashPage(candidate);
    const auto* bytes=reinterpret_cast<const void*>(PhysToVirt(candidate).raw());
    const uint32_t start=static_cast<uint32_t>(hash%slots_);
    uint32_t empty=slots_;
    uint64_t irq=IrqSpinLockAcquire(&lock_);
    for(uint32_t i=0;i<probes_;++i) {
        const uint32_t slot=(start+i)%slots_;
        const auto& entry=entries_[slot];
        if(!entry.physical) { if(empty==slots_)empty=slot;continue; }
        if(entry.hash!=hash)continue;
        if(memcmp(bytes,reinterpret_cast<const void*>(PhysToVirt(entry.physical).raw()),4096)) {
            ++stats_.collisions;continue;
        }
        // Entry's own ref guarantees liveness. Pin under the same lock that
        // unlinks entries; release the losing candidate outside this lock.
        const auto winner=entry.physical;PmmRefPage(winner);++stats_.hits;
        IrqSpinLockRelease(&lock_,irq);PmmUnrefPage(candidate);return winner;
    }
    ++stats_.misses;
    if(empty<slots_ && stats_.entries<capacity_) {
        PmmRefPage(candidate);entries_[empty]={hash,candidate};
        ++stats_.entries;++stats_.admissions;
    } else ++stats_.full;
    // Stable admission: don't evict active pages during sequential ELF scans.
    IrqSpinLockRelease(&lock_,irq);return candidate;
}
uint64_t ExecPageCache::Trim(bool all) {
    uint64_t removed=0;uint32_t cursor=0;
    while(cursor<slots_) {
        PhysicalAddress retired[32];uint32_t count=0,visited=0;
        const uint64_t irq=IrqSpinLockAcquire(&lock_);
        if(!stats_.entries) { IrqSpinLockRelease(&lock_,irq);break; }
        while(cursor<slots_ && count<32 && visited<128) {
            auto& entry=entries_[cursor++];++visited;
            if(!entry.physical || (!all && !PmmHasSingleReference(entry.physical)))continue;
            retired[count++]=entry.physical;entry={};--stats_.entries;
        }
        stats_.trimmed+=count;
        IrqSpinLockRelease(&lock_,irq);
        // No ref release/quarantine work while holding the cache lock.
        for(uint32_t i=0;i<count;++i)PmmUnrefPage(retired[i]);
        removed+=count;
    }
    return removed;
}
uint64_t ExecPageCache::ReclaimIdleStep(uint32_t maximumScan) {
    uint64_t removed=0;
    const uint32_t visits=maximumScan<slots_?maximumScan:slots_;
    for(uint32_t n=0;n<visits;++n) {
        PhysicalAddress retired{};
        const auto irq=IrqSpinLockAcquire(&lock_);
        if(!stats_.entries){IrqSpinLockRelease(&lock_,irq);break;}
        auto& entry=entries_[reclaimCursor_++];if(reclaimCursor_==slots_)reclaimCursor_=0;
        if(entry.physical && PmmHasSingleReference(entry.physical)) {
            retired=entry.physical;entry={};--stats_.entries;++stats_.trimmed;
        }
        IrqSpinLockRelease(&lock_,irq);
        if(retired){PmmUnrefPage(retired);++removed;}
    }
    return removed;
}
uint64_t ExecPageCache::TrimIdle() { return Trim(false); }
uint64_t ExecPageCache::Clear() { return Trim(true); }
ExecPageCacheStats ExecPageCache::Stats() {
    const uint64_t irq=IrqSpinLockAcquire(&lock_);
    auto result=stats_;result.capacity=capacity_;
    IrqSpinLockRelease(&lock_,irq);return result;
}
namespace {
// 4MiB metadata, at most 512MiB canonical frames. Sparse hashing keeps probe
// chains short near the admission limit; no permanent vnode/inode pins.
ExecPageCacheEntry entries[262144];
ExecPageCache cache(entries,262144,131072);
}
PhysicalAddress ExecPageCacheAdopt(PhysicalAddress candidate) { return cache.Adopt(candidate); }
uint64_t ExecPageCacheTrimIdle() { return cache.TrimIdle(); }
uint64_t ExecPageCacheReclaimIdleStep(uint32_t maximumScan) {return cache.ReclaimIdleStep(maximumScan);}
ExecPageCacheStats ExecPageCacheGetStats() { return cache.Stats(); }
#ifdef BROOK_HOST_TEST
void ExecPageCacheClearForTest() { cache.Clear(); }
#endif
}
