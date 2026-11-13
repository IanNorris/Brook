#pragma once
#include <stdint.h>

namespace brook {
struct MappingBacking;
struct BdwGpuBuffer;
// A zeroed, page-backed allocation. The returned object owns one backing
// reference. Handle tables, CPU VMAs and GPU submissions each retain their
// own reference; all access requires such a live reference. The caller must
// serialize handle lookup/close and establish DMA/IOMMU/cache safety before
// publishing any page to hardware. This does not expose an i915 device.
BdwGpuBuffer* BdwGpuBufferCreate(uint64_t bytes);
#ifdef BROOK_BDW_CACHE_PROBE
struct BdwCacheProbeCounters { uint32_t omit; uint64_t toDevice, toCpu; };
BdwGpuBuffer* BdwGpuBufferCreateCacheProbe(uint64_t bytes,uint32_t omit);
bool BdwGpuBufferCacheProbeCounters(const BdwGpuBuffer*,BdwCacheProbeCounters&);
bool BdwGpuBufferSkipCacheProbeFlush(BdwGpuBuffer*,bool forDevice);
void BdwGpuBufferPrimeCacheProbe(BdwGpuBuffer*);
#endif
// Kernel-owned output: CPU mappings through Intel DRM are forbidden from
// creation for its entire lifetime, including after PRIME/GEM import.
BdwGpuBuffer* BdwGpuBufferCreateHandoff(uint64_t bytes);
bool BdwGpuBufferAllowsUserMapping(const BdwGpuBuffer* buffer);
void BdwGpuBufferRef(BdwGpuBuffer* buffer);
void BdwGpuBufferUnref(BdwGpuBuffer* buffer);
uint64_t BdwGpuBufferBytes(const BdwGpuBuffer* buffer);
// Boot-local, immutable identity, shared by every PRIME alias of this buffer.
uint64_t BdwGpuBufferIdentity(const BdwGpuBuffer* buffer);
// One page at a time; a buffer's kernel virtual addresses are not guaranteed
// contiguous across chunk boundaries. Returns zero for an invalid index.
uint64_t BdwGpuBufferPageKernelAddress(const BdwGpuBuffer* buffer,uint32_t page);
const uint64_t* BdwGpuBufferPages(const BdwGpuBuffer* buffer,uint32_t& count);
MappingBacking* BdwGpuBufferBacking(BdwGpuBuffer* buffer); // borrowed; caller retains before publication
// Flush/invalidate CPU cache lines for all pages. Caller retains the buffer,
// excludes concurrent CPU writers/accesses as appropriate, and separately
// establishes device-side cache, DMA and retirement guarantees.
bool BdwGpuBufferSyncCpuCache(BdwGpuBuffer* buffer);
// CPU fallback only. The caller retains and serializes the completed ticket
// through the subsequent read. Never use on a scanout-mapped or failed output.
bool BdwGpuBufferPrepareHandoffRead(BdwGpuBuffer*,uint64_t token,uint64_t offset,uint64_t bytes);
// Excludes new GPU reservations while a CPU-domain transition flushes. It
// does not stop userspace from accessing an already mapped CPU address.
bool BdwGpuBufferBeginCpuTransition(BdwGpuBuffer* buffer);
bool BdwGpuBufferEndCpuTransition(BdwGpuBuffer* buffer);
// One attempt only: Busy does not sleep or alter the buffer. Ready proves a
// CPU-cache transition completed while new GPU reservations were excluded;
// it does not exclude already-mapped CPU writers after this function returns.
enum class BdwCpuTransitionResult { Ready, Busy, Failed };
BdwCpuTransitionResult BdwGpuBufferTryCpuTransition(BdwGpuBuffer* buffer);
// Snapshot GPU-use accounting. Each active snapshot owns one count per
// buffer until verified retirement/reset. A count is not itself a fence or
// wakeup; the scheduler must publish it before exposing a submission.
bool BdwGpuBufferBeginGpuUse(BdwGpuBuffer* buffer);
bool BdwGpuBufferEndGpuUse(BdwGpuBuffer* buffer);
// Experimental output reservation. Token must be kernel-generated, nonzero,
// and never reused during the buffer's lifetime. It excludes ordinary GPU use
// and CPU-domain transitions; exactly one matching GPU reservation may run.
// It does not revoke userspace CPU mappings or guarantee GPU retirement.
bool BdwGpuBufferReserveHandoff(BdwGpuBuffer*,uint64_t token);
bool BdwGpuBufferHandoffMatches(const BdwGpuBuffer*,uint64_t token);
// True only for the same exclusive reservation after its one GPU use ended.
bool BdwGpuBufferHandoffCompleted(const BdwGpuBuffer*,uint64_t token);
// Caller serializes with the handoff owner. A mapping hold prevents release
// and GPU reuse until the exact token's mapping is proved retired.
bool BdwGpuBufferBeginScanoutMapping(BdwGpuBuffer*,uint64_t token);
bool BdwGpuBufferEndScanoutMapping(BdwGpuBuffer*,uint64_t token);
bool BdwGpuBufferScanoutMappingMatches(const BdwGpuBuffer*,uint64_t token);
bool BdwGpuBufferBeginHandoffGpuUse(BdwGpuBuffer*,uint64_t token);
bool BdwGpuBufferReleaseHandoff(BdwGpuBuffer*,uint64_t token);
uint32_t BdwGpuBufferGpuUses(const BdwGpuBuffer* buffer);
void BdwGpuBufferMarkGpuFailed(BdwGpuBuffer* buffer);
bool BdwGpuBufferGpuFailed(const BdwGpuBuffer* buffer);
// One atomic sample prevents a completion/failure race from appearing idle.
void BdwGpuBufferGpuStatus(const BdwGpuBuffer* buffer,uint32_t& uses,bool& failed);
// Owns an additional reference until proven retirement. On uncertainty the
// pin is deliberately retained; destruction does not imply GPU retirement.
class BdwGpuBufferPin {
    BdwGpuBuffer* buffer_=nullptr;
public:
    BdwGpuBufferPin()=default;
    BdwGpuBufferPin(const BdwGpuBufferPin&)=delete;
    BdwGpuBufferPin& operator=(const BdwGpuBufferPin&)=delete;
    bool Acquire(BdwGpuBuffer* buffer);
    bool Retire(bool safelyStopped);
    BdwGpuBuffer* Get()const{return buffer_;}
};
}
