#pragma once
#include "address.h"
#include "spinlock.h"

namespace brook {
struct Vnode;
// Shared across filesystem drivers; zero means permanently exhausted.
uint32_t FilePageCacheAllocateNamespace();
struct FilePageCacheEntry {
    uint64_t cacheId = 0, pageIndex = 0;
    PhysicalAddress physical;
};
// Immutable canonical pages only, identified by never-reused namespace/inode.
// Cached bytes do not pin a vnode/mount: callers own a live vnode for each call.
// Mutable files must keep cacheId zero until a separate coherence policy exists.
struct FilePageCacheStats { uint64_t cached,scanned,reclaimed; };
class FilePageCache {
public:
    constexpr FilePageCache(FilePageCacheEntry* entries, uint32_t slots, uint32_t probes = 32)
        : entries_(entries), slots_(slots), probes_(probes < slots ? probes : slots) {}
    struct ReclaimResult { uint32_t scanned,reclaimed; };
    // Remove cache-only frames; no new lookup ref can appear under the same
    // lock. PMM release occurs after unlock. No mappings are edited here.
    ReclaimResult ReclaimStep(uint32_t maximumScan);
    uint32_t Count();
    FilePageCacheStats Statistics();
    // Each successful result owns one frame reference.
    PhysicalAddress Lookup(Vnode* vnode, uint64_t pageIndex);
    // Always consumes candidate's reference. A duplicate returns the retained
    // canonical winner, never the losing candidate. With allowUncached=false,
    // disabled identity or capacity exhaustion drops candidate and returns zero.
    // Private mappings may explicitly allow an uncached fallback; shared callers
    // must require canonical storage, in addition to implementing I/O coherence.
    PhysicalAddress Adopt(Vnode* vnode, uint64_t pageIndex, PhysicalAddress candidate,
                          bool allowUncached);
private:
    uint32_t Hash(uint64_t identity, uint64_t pageIndex) const;
    FilePageCacheEntry* entries_;
    uint32_t slots_, probes_;
    IrqSpinLock lock_;
    uint32_t reclaimCursor_=0,count_=0;
    uint64_t scanned_=0,reclaimed_=0;
};
void FilePageCacheEnableReclaim(); // boot-selected immutable filesystem only
void FilePageCacheStartReclaimWorker();
void FilePageCachePauseReclaim(bool paused); // root experiment diagnostics
FilePageCacheStats FilePageCacheStatistics();
FilePageCache::ReclaimResult FilePageCacheReclaim(uint32_t maximumScan);
uint32_t FilePageCacheCount();
PhysicalAddress FilePageCacheLookup(Vnode* vnode, uint64_t pageIndex);
PhysicalAddress FilePageCacheAdopt(Vnode* vnode, uint64_t pageIndex,
                                   PhysicalAddress candidate, bool allowUncached);
}
