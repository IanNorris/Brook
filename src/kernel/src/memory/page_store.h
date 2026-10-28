#pragma once
#include <stdint.h>

namespace brook {
class CompressedPagePool;
// Disposable raw page backing, NOT a filesystem or a resumable swap image.
// Header is little-endian on disk. No user pointers or filesystem callbacks.
struct PageStoreHeader {
    uint64_t magic;
    uint32_t version, pageBytes;
    uint64_t slots, dataOffset;
    uint8_t identity[16];
    uint64_t checksum;
};
static_assert(sizeof(PageStoreHeader)==56);
constexpr uint64_t PAGE_STORE_MAGIC=0x31305057534b5242ULL; // BRKSWP01
constexpr uint64_t PAGE_STORE_HEADER_BYTES=4096;
uint64_t PageStoreChecksum(const void* bytes,uint64_t length);
bool PageStoreHeaderValid(const PageStoreHeader& header,uint64_t deviceBytes);

struct PageStoreIo {
    void* context;
    int (*read)(void*,uint64_t,void*,uint64_t);
    int (*write)(void*,uint64_t,const void*,uint64_t);
};
enum class PageSlotState : uint8_t { Free, Reserved, Stored, Exhausted, StoredRam };
struct PageStoreSlot {
    uint64_t checksum;
    uint32_t generation, references;
    PageSlotState state;
    uint32_t ramOffset;
    uint16_t compressedBytes;
};
struct PageStoreStats {
    uint64_t slots, used, stored, writes, reads, ioErrors, corruptReads;
    uint64_t ramSlots,diskSlots,ramWrites,ramReads,diskWrites,diskReads;
    uint64_t compressionFallbacks,spillSuccesses,spillFailures,decodeErrors;
};
// Codec/context, both 4 KiB scratch buffers and arena are caller-owned resident
// storage. Encode returns bytes or zero, decode must return exactly 4096. No
// callback may allocate or enter AS locks. All calls use store serialization.
struct PageStoreCompression {
    CompressedPagePool* pool;
    void* context;
    int (*encode)(void*,const void*,void*,int);
    int (*decode)(void*,const void*,int,void*,int);
    void* encodedScratch;
    void* spillScratch;
};
// Entire API is externally serialized. Caller owns resident slot records and
// backend lifetime, and must not hold an AS/PT/spinlock across Store/Load.
// No method allocates, frees a frame, edits a PTE, or claims eviction safety.
// Tokens carry a generation; a released token cannot name reused storage.
class PageStore {
public:
    PageStore() = default;
    PageStore(const PageStore&) = delete;
    PageStore& operator=(const PageStore&) = delete;
    bool Initialize(const PageStoreHeader& header,uint64_t deviceBytes,
                    PageStoreSlot* records,uint64_t recordCount,PageStoreIo io);
    bool AttachCompression(PageStoreCompression compression); // only before first slot use
    int Spill(uint64_t token); // RAM remains authoritative unless raw write succeeds
    uint64_t Reserve(); // zero on full/uninitialized; one owning reference
    bool Retain(uint64_t token);
    bool Release(uint64_t token);
    int Store(uint64_t token,const void* page); // immutable after success
    // A failed read leaves the slot intact and the output buffer undefined.
    // Caller must never publish that output unless this returns zero.
    int Load(uint64_t token,void* page);
    PageStoreStats Stats() const { return stats_; }
private:
    PageStoreSlot* Find(uint64_t token);
    uint64_t Offset(uint64_t token) const;
    int DecodeRam(const PageStoreSlot& record,void* page);
    PageStoreSlot* records_=nullptr;
    PageStoreIo io_{};
    PageStoreStats stats_{};
    uint64_t dataOffset_=0, hint_=0;
    PageStoreCompression compression_{};
    bool started_=false;
};
}
