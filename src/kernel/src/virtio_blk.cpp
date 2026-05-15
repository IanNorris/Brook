#include "virtio_blk.h"
#include "pci.h"
#include "portio.h"
#include "string.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "memory/heap.h"
#include "serial.h"
#include "mem_tag.h"
#include "idt.h"

namespace brook {

// ---- virtio-blk PCI register offsets (legacy BAR0 I/O) ----

static constexpr uint8_t VIRTIO_PCI_HOST_FEATURES  = 0x00;
static constexpr uint8_t VIRTIO_PCI_GUEST_FEATURES = 0x04;
static constexpr uint8_t VIRTIO_PCI_QUEUE_PFN      = 0x08;
static constexpr uint8_t VIRTIO_PCI_QUEUE_SIZE     = 0x0C;
static constexpr uint8_t VIRTIO_PCI_QUEUE_SEL      = 0x0E;
static constexpr uint8_t VIRTIO_PCI_QUEUE_NOTIFY   = 0x10;
static constexpr uint8_t VIRTIO_PCI_STATUS         = 0x12;
static constexpr uint8_t VIRTIO_PCI_ISR            = 0x13; // ISR status (read clears)

// Device config space starts at 0x14 for legacy; blk config has capacity first.
static constexpr uint8_t VIRTIO_PCI_BLK_CAPACITY   = 0x14; // 64-bit sector count

// Device status bits
static constexpr uint8_t VIRTIO_STATUS_ACKNOWLEDGE  = 1;
static constexpr uint8_t VIRTIO_STATUS_DRIVER       = 2;
static constexpr uint8_t VIRTIO_STATUS_DRIVER_OK    = 4;
// VIRTIO_STATUS_FAILED not used currently (no error recovery path yet).

// Virtqueue descriptor flags
static constexpr uint16_t VIRTQ_DESC_F_NEXT  = 1;
static constexpr uint16_t VIRTQ_DESC_F_WRITE = 2; // device-writable (we read from it)

// virtio-blk request types
static constexpr uint32_t VIRTIO_BLK_T_IN  = 0; // read
static constexpr uint32_t VIRTIO_BLK_T_OUT = 1; // write

static constexpr uint8_t VIRTIO_BLK_S_OK = 0;

static constexpr uint32_t VIRTIO_MAX_DEVS = 8;
static constexpr uint8_t  VIRTIO_BLK_IRQ_VECTOR = 50; // preferred IDT vector
static constexpr uint32_t VIRTIO_CACHE_BLOCK_SECTORS = 8;     // 4 KiB
static constexpr uint32_t VIRTIO_CACHE_BLOCK_SIZE    = 4096;
static constexpr uint32_t VIRTIO_CACHE_ENTRIES       = 4096;  // 16 MiB
static constexpr uint64_t VIRTIO_SMALL_READ_LIMIT    = 64 * 1024;

// ---- Virtqueue structures (packed for DMA) ----

struct __attribute__((packed)) VirtqDesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

// virtio-blk request header (placed before the data buffer)
struct __attribute__((packed)) VirtioBlkReq {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

// ---- Multi-request slot pool ----
// Each slot owns 3 contiguous descriptors, a request header, a status byte,
// and a small (4 KiB) DMA buffer used for cache-fill reads.  Up to
// MAX_INFLIGHT requests can be outstanding simultaneously.

static constexpr uint32_t MAX_INFLIGHT       = 16;
static constexpr uint32_t DESCS_PER_SLOT     = 3; // header + data + status
static constexpr uint32_t SLOT_DMA_SIZE      = VIRTIO_CACHE_BLOCK_SIZE; // 4 KiB

struct RequestSlot {
    VirtioBlkReq* reqBuf;       // 16-byte request header (DMA-visible)
    uint64_t      reqBufPhys;
    uint8_t*      statusBuf;    // 1-byte status (DMA-visible)
    uint64_t      statusBufPhys;
    uint8_t*      dmaBuf;       // 4 KiB data buffer (DMA-visible)
    uint64_t      dmaBufPhys;
    uint16_t      descBase;     // first descriptor index in the table
    volatile bool complete;     // set by completion reaper
    uint64_t      blockNumber;  // which cache block this slot is filling
};

// Maximum queue size we're willing to handle.  The device may advertise
// up to 32768; we cap to keep DMA allocation bounded.
// Must be >= MAX_INFLIGHT * DESCS_PER_SLOT + DESCS_PER_SLOT (for the legacy slot).
static constexpr uint32_t MAX_QUEUE_SIZE = 256;

struct __attribute__((packed)) VirtqUsedElem {
    uint32_t id;
    uint32_t len;
};

struct VirtioBlkCacheEntry {
    uint64_t blockNumber; // 4 KiB block number, not 512-byte sector number
    bool     valid;
};

// ---- Driver state ----

struct VirtioBlkState {
    uint16_t    ioBase;         // BAR0 I/O port base
    uint16_t    queueSize;      // negotiated queue size (power of 2)

    // Virtqueue memory (physically contiguous, 4KB-aligned).
    // Accessed via pointers — layout is size-dependent.
    VirtqDesc*  descTable;
    // Available ring: [flags(2)] [idx(2)] [ring[queueSize](2*N)] [used_event(2)]
    uint16_t*   availFlags;
    uint16_t*   availIdx;
    uint16_t*   availRing;      // points to ring[0]
    // Used ring: [flags(2)] [idx(2)] [ring[queueSize](8*N)] [avail_event(2)]
    uint16_t*       usedFlags;
    volatile uint16_t* usedIdx;
    VirtqUsedElem*  usedRing;   // points to ring[0]

    uint64_t    queuePhys;      // physical base of descriptor table

    uint16_t    availIdxShadow; // next available ring index to write
    uint16_t    usedIdxShadow;  // last consumed used ring index

    uint64_t    sectorCount;    // total sectors on the device

    // Per-request DMA buffers (reused; one request at a time)
    VirtioBlkReq* reqBuf;
    uint64_t      reqBufPhys;
    uint8_t*      statusBuf;
    uint64_t      statusBufPhys;

    // Persistent page-aligned DMA data buffer (256 KB = 512 sectors).
    // Larger buffer means fewer SubmitRequest round-trips for big reads.
    static constexpr uint32_t DMA_BUF_PAGES = 64;
    uint8_t*      dmaBuf;
    uint64_t      dmaBufPhys;

    VirtioBlkCacheEntry* cacheEntries;
    uint8_t*             cacheData;

    // I/O statistics
    volatile uint64_t readOps;
    volatile uint64_t writeOps;
    volatile uint64_t readBytes;
    volatile uint64_t writeBytes;

    // Serialises concurrent requests from multiple processes.  The current
    // driver still uses hardcoded descriptor slots 0-2 and one shared DMA
    // buffer, so only one request can be in-flight at a time.
    volatile uint32_t requestGuardNext;
    volatile uint32_t requestGuardServing;

    // Interrupt-driven completion: the ISR sets this flag; SubmitRequest
    // spins briefly then falls back to hlt until the interrupt fires.
    volatile uint32_t irqComplete;
    uint8_t           irqLine;   // PCI interrupt line (ISA IRQ number)
    uint8_t           irqVector; // actual IDT vector assigned

    // Multi-request slot pool for batched cache-fill reads.
    // Slots 0..MAX_INFLIGHT-1 each own descriptors [slot*3 .. slot*3+2].
    // The legacy single-request path uses descriptors [MAX_INFLIGHT*3 .. MAX_INFLIGHT*3+2].
    RequestSlot       slots[MAX_INFLIGHT];
    uint32_t          slotsInFlight; // how many slots have been submitted but not reaped
};

// ---- Register helpers ----

static inline uint32_t VioRead32(uint16_t base, uint8_t reg)  { return inl(base + reg); }
static inline uint16_t VioRead16(uint16_t base, uint8_t reg)  { return inw(base + reg); }
static inline void VioWrite32(uint16_t base, uint8_t reg, uint32_t v) { outl(base + reg, v); }
static inline void VioWrite16(uint16_t base, uint8_t reg, uint16_t v)
{
    // 16-bit I/O write
    __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(static_cast<uint16_t>(base + reg)));
}
static inline void VioWrite8 (uint16_t base, uint8_t reg, uint8_t v)  { outb(base + reg, v); }

static inline uint32_t CacheIndex(uint64_t blockNumber)
{
    return static_cast<uint32_t>(blockNumber % VIRTIO_CACHE_ENTRIES);
}

static inline uint8_t* CacheDataFor(VirtioBlkState& s, uint32_t index)
{
    return s.cacheData + (static_cast<uint64_t>(index) * VIRTIO_CACHE_BLOCK_SIZE);
}

static bool CacheLookup(VirtioBlkState& s, uint64_t blockNumber, uint8_t** data)
{
    if (!s.cacheEntries || !s.cacheData)
        return false;

    uint32_t idx = CacheIndex(blockNumber);
    VirtioBlkCacheEntry& e = s.cacheEntries[idx];
    if (!e.valid || e.blockNumber != blockNumber)
        return false;

    *data = CacheDataFor(s, idx);
    return true;
}

static void CacheStore(VirtioBlkState& s, uint64_t blockNumber, const uint8_t* src)
{
    if (!s.cacheEntries || !s.cacheData)
        return;

    uint32_t idx = CacheIndex(blockNumber);
    memcpy(CacheDataFor(s, idx), src, VIRTIO_CACHE_BLOCK_SIZE);
    s.cacheEntries[idx].blockNumber = blockNumber;
    s.cacheEntries[idx].valid = true;
}

static void CacheInvalidateRange(VirtioBlkState& s, uint64_t startSector, uint64_t endSector)
{
    if (!s.cacheEntries || !s.cacheData || endSector <= startSector)
        return;

    uint64_t firstBlock = startSector / VIRTIO_CACHE_BLOCK_SECTORS;
    uint64_t lastBlock = (endSector - 1) / VIRTIO_CACHE_BLOCK_SECTORS;
    for (uint64_t block = firstBlock; block <= lastBlock; ++block)
    {
        uint32_t idx = CacheIndex(block);
        VirtioBlkCacheEntry& e = s.cacheEntries[idx];
        if (e.valid && e.blockNumber == block)
            e.valid = false;
    }
}

static void CacheStoreFullBlocks(VirtioBlkState& s, uint64_t firstSector,
                                 uint32_t sectorCount, const uint8_t* src)
{
    if (!s.cacheEntries || !s.cacheData)
        return;

    uint64_t endSector = firstSector + sectorCount;
    uint64_t block = (firstSector + VIRTIO_CACHE_BLOCK_SECTORS - 1) /
                     VIRTIO_CACHE_BLOCK_SECTORS;
    while ((block + 1) * VIRTIO_CACHE_BLOCK_SECTORS <= endSector)
    {
        uint64_t sectorOffset = block * VIRTIO_CACHE_BLOCK_SECTORS - firstSector;
        CacheStore(s, block, src + sectorOffset * 512);
        ++block;
    }
}

// ---- Virtqueue DMA allocation ----
// Virtio 1.0 legacy layout for a queue of size N:
//   Descriptor table:  16 * N bytes
//   Available ring:    6 + 2*N bytes  (flags + idx + ring[N] + used_event)
//   [padding to next page boundary]
//   Used ring:         6 + 8*N bytes  (flags + idx + ring[N] + avail_event)
// We also allocate 1 extra page for req header + status byte DMA buffers.

static uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

static bool AllocVirtqueue(VirtioBlkState& s)
{
    uint32_t N = s.queueSize;

    uint32_t descSize  = 16 * N;
    uint32_t availSize = 6 + 2 * N;
    uint32_t usedOff   = AlignUp(descSize + availSize, 4096);
    uint32_t usedSize  = 6 + 8 * N;
    uint32_t totalSize = AlignUp(usedOff + usedSize, 4096);
    uint32_t totalPages = totalSize / 4096 + 1; // +1 for req/status buffers

    SerialPrintf("virtio: alloc queue: N=%u descSz=%u availSz=%u usedOff=%u usedSz=%u totalPg=%u\n",
                 N, descSize, availSize, usedOff, usedSize, totalPages);

    uint64_t qVirt = VmmAllocPages(totalPages, VMM_WRITABLE, MemTag::Device, KernelPid).raw();
    if (!qVirt) return false;
    SerialPrintf("virtio: queue virt=0x%lx pages=%u usedIdx_virt=0x%lx\n",
                 qVirt, totalPages,
                 qVirt + usedOff + 2);

    uint8_t* base = reinterpret_cast<uint8_t*>(qVirt);
    memset(base, 0, totalPages * 4096);

    s.descTable  = reinterpret_cast<VirtqDesc*>(qVirt);

    uint8_t* availBase = base + descSize;
    s.availFlags = reinterpret_cast<uint16_t*>(availBase);
    s.availIdx   = reinterpret_cast<uint16_t*>(availBase + 2);
    s.availRing  = reinterpret_cast<uint16_t*>(availBase + 4);

    uint8_t* usedBase = base + usedOff;
    s.usedFlags = reinterpret_cast<uint16_t*>(usedBase);
    s.usedIdx   = reinterpret_cast<volatile uint16_t*>(usedBase + 2);
    s.usedRing  = reinterpret_cast<VirtqUsedElem*>(usedBase + 4);

    s.queuePhys = VmmVirtToPhys(KernelPageTable, VirtualAddress(qVirt)).raw();

    uint64_t extraVirt = qVirt + (totalPages - 1) * 4096;
    // Legacy single-request slot (used for writes and large DMA reads).
    // Descriptor indices: MAX_INFLIGHT*3 .. MAX_INFLIGHT*3+2.
    s.reqBuf         = reinterpret_cast<VirtioBlkReq*>(extraVirt);
    s.reqBufPhys     = VmmVirtToPhys(KernelPageTable, VirtualAddress(extraVirt)).raw();
    s.statusBuf      = reinterpret_cast<uint8_t*>(extraVirt + sizeof(VirtioBlkReq));
    s.statusBufPhys  = s.reqBufPhys + sizeof(VirtioBlkReq);

    // ---- Allocate multi-request slot pool ----
    // One page for all slot metadata (reqBuf + statusBuf per slot),
    // plus MAX_INFLIGHT pages for the 4 KiB DMA data buffers.
    uint32_t slotMetaPages = 1;
    uint32_t slotDmaPages  = MAX_INFLIGHT; // one 4 KiB page per slot
    uint32_t slotTotalPages = slotMetaPages + slotDmaPages;

    uint64_t slotVirt = VmmAllocPages(slotTotalPages, VMM_WRITABLE, MemTag::Device, KernelPid).raw();
    if (!slotVirt)
    {
        SerialPuts("virtio: slot pool alloc failed\n");
        return false;
    }
    memset(reinterpret_cast<void*>(slotVirt), 0, slotTotalPages * 4096);
    uint64_t slotPhysBase = VmmVirtToPhys(KernelPageTable, VirtualAddress(slotVirt)).raw();

    // Metadata page layout: [VirtioBlkReq(16) + status(1)] × MAX_INFLIGHT
    // Each entry is 17 bytes; 16 entries = 272 bytes (fits in one page).
    for (uint32_t i = 0; i < MAX_INFLIGHT; ++i)
    {
        RequestSlot& rs = s.slots[i];
        uint64_t metaOff = i * 32; // 32-byte stride for alignment
        rs.reqBuf      = reinterpret_cast<VirtioBlkReq*>(slotVirt + metaOff);
        rs.reqBufPhys  = slotPhysBase + metaOff;
        rs.statusBuf   = reinterpret_cast<uint8_t*>(slotVirt + metaOff + sizeof(VirtioBlkReq));
        rs.statusBufPhys = slotPhysBase + metaOff + sizeof(VirtioBlkReq);

        // DMA data buffer: one page per slot, starting after the metadata page.
        uint64_t dmaOff = (slotMetaPages + i) * 4096;
        rs.dmaBuf     = reinterpret_cast<uint8_t*>(slotVirt + dmaOff);
        rs.dmaBufPhys = VmmVirtToPhys(KernelPageTable, VirtualAddress(slotVirt + dmaOff)).raw();

        rs.descBase   = static_cast<uint16_t>(i * DESCS_PER_SLOT);
        rs.complete   = false;
        rs.blockNumber = 0;
    }
    s.slotsInFlight = 0;

    SerialPrintf("virtio: slot pool: %u slots, %u pages\n", MAX_INFLIGHT, slotTotalPages);

    SerialPrintf("virtio: queuePhys=0x%lx reqBufPhys=0x%lx\n",
                 s.queuePhys, s.reqBufPhys);
    // CRITICAL: check if any DMA physical address targets the PDPT at 0x101000
    if ((s.queuePhys & ~0xFFFULL) == 0x101000 ||
        (s.reqBufPhys & ~0xFFFULL) == 0x101000 ||
        (s.statusBufPhys & ~0xFFFULL) == 0x101000)
    {
        SerialPrintf("virtio: CRITICAL — DMA buffer overlaps PDPT at 0x101000!\n");
        SerialPrintf("  queuePhys=0x%lx reqBufPhys=0x%lx statusPhys=0x%lx\n",
                     s.queuePhys, s.reqBufPhys, s.statusBufPhys);
    }

    return true;
}

// ---- Interrupt handler ----

// Per-device state pointers indexed by slot (for ISR lookup).
static VirtioBlkState* g_devStates[VIRTIO_MAX_DEVS];
static uint32_t        g_devCount = 0;

// Plain function — called by the shared IRQ dispatch stub.
// Must NOT be __attribute__((interrupt)). Must NOT call ApicSendEoi().
static void VirtioBlkIrqBody()
{
    for (uint32_t i = 0; i < g_devCount; ++i)
    {
        VirtioBlkState* s = g_devStates[i];
        if (!s) continue;

        // Read ISR status register — clears the interrupt on the device.
        uint8_t isr = inb(s->ioBase + VIRTIO_PCI_ISR);
        if (isr & 1)
        {
            // Queue completion — signal the waiting SubmitRequest.
            __atomic_store_n(&s->irqComplete, 1, __ATOMIC_RELEASE);
        }
    }
}

// ---- Synchronous request (legacy path for writes & large DMA reads) ----
// Uses descriptors at offset MAX_INFLIGHT*3 to avoid collision with slot pool.

static constexpr uint16_t LEGACY_DESC_BASE = MAX_INFLIGHT * DESCS_PER_SLOT;

static bool SubmitRequest(VirtioBlkState& s,
                           uint32_t type, uint64_t sector,
                           uint64_t dataBufPhys, uint32_t dataLen)
{
    uint16_t d0 = LEGACY_DESC_BASE;
    uint16_t d1 = LEGACY_DESC_BASE + 1;
    uint16_t d2 = LEGACY_DESC_BASE + 2;

    s.reqBuf->type     = type;
    s.reqBuf->reserved = 0;
    s.reqBuf->sector   = sector;

    s.descTable[d0].addr  = s.reqBufPhys;
    s.descTable[d0].len   = sizeof(VirtioBlkReq);
    s.descTable[d0].flags = VIRTQ_DESC_F_NEXT;
    s.descTable[d0].next  = d1;

    s.descTable[d1].addr  = dataBufPhys;
    s.descTable[d1].len   = dataLen;
    s.descTable[d1].flags = VIRTQ_DESC_F_NEXT | (type == VIRTIO_BLK_T_IN ? VIRTQ_DESC_F_WRITE : 0);
    s.descTable[d1].next  = d2;

    *s.statusBuf         = 0xFF;
    s.descTable[d2].addr  = s.statusBufPhys;
    s.descTable[d2].len   = 1;
    s.descTable[d2].flags = VIRTQ_DESC_F_WRITE;
    s.descTable[d2].next  = 0;

    __asm__ volatile("mfence" ::: "memory");

    uint16_t ringSlot = s.availIdxShadow % s.queueSize;
    s.availRing[ringSlot] = d0;
    __asm__ volatile("mfence" ::: "memory");
    *s.availIdx = ++s.availIdxShadow;
    __asm__ volatile("mfence" ::: "memory");

    __atomic_store_n(&s.irqComplete, 0, __ATOMIC_RELEASE);
    VioWrite16(s.ioBase, VIRTIO_PCI_QUEUE_NOTIFY, 0);

    static constexpr uint32_t SPIN_LIMIT = 1024;
    for (uint32_t i = 0; i < SPIN_LIMIT; ++i) {
        if (*s.usedIdx != s.usedIdxShadow) goto done;
        __asm__ volatile("pause" ::: "memory");
    }
    for (uint32_t i = 0; i < 100000u; ++i) {
        if (*s.usedIdx != s.usedIdxShadow) goto done;
        __asm__ volatile("hlt" ::: "memory");
    }
    SerialPuts("virtio-blk: timeout waiting for response\n");
    return false;

done:
    __asm__ volatile("mfence" ::: "memory");
    ++s.usedIdxShadow;
    return (*s.statusBuf == VIRTIO_BLK_S_OK);
}

// ---- Async multi-request API (for batched cache-fill reads) ----

// Submit a 4 KiB read into a slot's DMA buffer. Non-blocking.
static void SubmitSlotRead(VirtioBlkState& s, uint32_t slotIdx, uint64_t sector)
{
    RequestSlot& rs = s.slots[slotIdx];
    uint16_t d0 = rs.descBase;
    uint16_t d1 = rs.descBase + 1;
    uint16_t d2 = rs.descBase + 2;

    rs.reqBuf->type     = VIRTIO_BLK_T_IN;
    rs.reqBuf->reserved = 0;
    rs.reqBuf->sector   = sector;
    rs.complete         = false;

    s.descTable[d0].addr  = rs.reqBufPhys;
    s.descTable[d0].len   = sizeof(VirtioBlkReq);
    s.descTable[d0].flags = VIRTQ_DESC_F_NEXT;
    s.descTable[d0].next  = d1;

    s.descTable[d1].addr  = rs.dmaBufPhys;
    s.descTable[d1].len   = SLOT_DMA_SIZE;
    s.descTable[d1].flags = VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE;
    s.descTable[d1].next  = d2;

    *rs.statusBuf        = 0xFF;
    s.descTable[d2].addr  = rs.statusBufPhys;
    s.descTable[d2].len   = 1;
    s.descTable[d2].flags = VIRTQ_DESC_F_WRITE;
    s.descTable[d2].next  = 0;

    __asm__ volatile("mfence" ::: "memory");

    uint16_t ringSlot = s.availIdxShadow % s.queueSize;
    s.availRing[ringSlot] = d0;
    __asm__ volatile("mfence" ::: "memory");
    *s.availIdx = ++s.availIdxShadow;
    // Don't notify yet — caller batches multiple submissions then notifies once.
}

// Notify the device after batching one or more SubmitSlotRead calls.
static void NotifyDevice(VirtioBlkState& s)
{
    __asm__ volatile("mfence" ::: "memory");
    __atomic_store_n(&s.irqComplete, 0, __ATOMIC_RELEASE);
    VioWrite16(s.ioBase, VIRTIO_PCI_QUEUE_NOTIFY, 0);
}

// Reap completed requests from the used ring.  Marks completed slots.
// Returns the number of completions reaped this call.
static uint32_t ReapCompletions(VirtioBlkState& s)
{
    uint32_t reaped = 0;
    while (*s.usedIdx != s.usedIdxShadow)
    {
        __asm__ volatile("mfence" ::: "memory");
        uint16_t usedSlot = s.usedIdxShadow % s.queueSize;
        uint32_t descId = s.usedRing[usedSlot].id;
        ++s.usedIdxShadow;

        // Identify which request slot completed by descriptor base.
        // Slot i uses descriptors [i*3 .. i*3+2].
        if (descId < MAX_INFLIGHT * DESCS_PER_SLOT)
        {
            uint32_t slotIdx = descId / DESCS_PER_SLOT;
            s.slots[slotIdx].complete = true;
        }
        // else: legacy slot completion (handled by SubmitRequest's wait loop)
        ++reaped;
    }
    return reaped;
}

// Wait until all submitted slots are complete.
static bool WaitAllSlots(VirtioBlkState& s, uint32_t count)
{
    // Spin first, then hlt.
    for (uint32_t spin = 0; spin < 2048; ++spin)
    {
        ReapCompletions(s);
        bool allDone = true;
        for (uint32_t i = 0; i < count; ++i)
            if (!s.slots[i].complete) { allDone = false; break; }
        if (allDone) return true;
        __asm__ volatile("pause" ::: "memory");
    }

    for (uint32_t iter = 0; iter < 100000u; ++iter)
    {
        ReapCompletions(s);
        bool allDone = true;
        for (uint32_t i = 0; i < count; ++i)
            if (!s.slots[i].complete) { allDone = false; break; }
        if (allDone) return true;
        __asm__ volatile("hlt" ::: "memory");
    }

    SerialPuts("virtio-blk: timeout waiting for batch completion\n");
    return false;
}

static void AcquireRequestLock(VirtioBlkState& s)
{
    uint32_t ticket = __atomic_fetch_add(&s.requestGuardNext, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&s.requestGuardServing, __ATOMIC_ACQUIRE) != ticket) {
        __asm__ volatile("pause" ::: "memory");
    }
}

static void ReleaseRequestLock(VirtioBlkState& s)
{
    __atomic_fetch_add(&s.requestGuardServing, 1, __ATOMIC_RELEASE);
}

// ---- DeviceOps ----

static int VirtioBlkRead(Device* dev, uint64_t offset, void* buf, uint64_t len)
{
    auto* s = static_cast<VirtioBlkState*>(dev->priv);
    if (len == 0) return 0;

    static constexpr uint32_t SECTOR_SIZE = 512;
    static constexpr uint32_t DMA_BUF_SIZE = VirtioBlkState::DMA_BUF_PAGES * 4096;
    static constexpr uint32_t SECTORS_PER_DMA = DMA_BUF_SIZE / SECTOR_SIZE; // 128

    // Guard against offset+len overflow and reads past device end
    uint64_t deviceBytes = s->sectorCount * SECTOR_SIZE;
    if (offset >= deviceBytes) return 0;
    if (len > deviceBytes - offset) len = deviceBytes - offset;

    uint64_t startSector = offset / SECTOR_SIZE;
    uint64_t endSector   = (offset + len + SECTOR_SIZE - 1) / SECTOR_SIZE;

    // DMA buffers must be physically contiguous and page-aligned.
    // Use the persistent 64 KB DMA buffer (up to 128 sectors per request).
    uint8_t* dstBytes = static_cast<uint8_t*>(buf);
    uint64_t bytesRead = 0;

    // Serialise against concurrent reads/writes without masking timer IRQs
    // across device latency.
    //
    // Optimistic fast path: try to serve entirely from cache without the lock.
    // The cache is safe to read concurrently (direct-mapped, entries are
    // word-aligned, and we validate blockNumber after reading data).
    bool cacheableSmallRead =
        len <= VIRTIO_SMALL_READ_LIMIT &&
        s->cacheEntries && s->cacheData &&
        endSector <= s->sectorCount &&
        ((((endSector - 1) / VIRTIO_CACHE_BLOCK_SECTORS) + 1) *
             VIRTIO_CACHE_BLOCK_SECTORS) <= s->sectorCount;

    if (cacheableSmallRead)
    {
        // Optimistic: try all blocks from cache without lock
        bool allCached = true;
        uint64_t probe = 0;
        while (probe < len) {
            uint64_t absolute = offset + probe;
            uint64_t blockNumber = absolute / VIRTIO_CACHE_BLOCK_SIZE;
            uint8_t* cacheBlock = nullptr;
            if (!CacheLookup(*s, blockNumber, &cacheBlock)) {
                allCached = false;
                break;
            }
            probe += VIRTIO_CACHE_BLOCK_SIZE - (absolute % VIRTIO_CACHE_BLOCK_SIZE);
        }

        if (allCached) {
            // All blocks cached — serve without lock
            while (bytesRead < len) {
                uint64_t absolute = offset + bytesRead;
                uint64_t blockNumber = absolute / VIRTIO_CACHE_BLOCK_SIZE;
                uint64_t blockOffset = absolute % VIRTIO_CACHE_BLOCK_SIZE;
                uint8_t* cacheBlock = nullptr;
                CacheLookup(*s, blockNumber, &cacheBlock);

                uint64_t n = VIRTIO_CACHE_BLOCK_SIZE - blockOffset;
                if (n > len - bytesRead) n = len - bytesRead;
                memcpy(dstBytes + bytesRead, cacheBlock + blockOffset, n);
                bytesRead += n;
            }
            s->readOps++;
            s->readBytes += bytesRead;
            return static_cast<int>(bytesRead);
        }

        // Cache miss — fall through to locked path
    }

    AcquireRequestLock(*s);

    if (cacheableSmallRead)
    {
        // Batched cache-fill: collect up to MAX_INFLIGHT cache misses,
        // submit them all to the device, wait for completion, store in cache,
        // then copy from cache to the user buffer.
        //
        // First pass: identify all cache-miss blocks.
        uint64_t missBlocks[MAX_INFLIGHT];
        uint32_t missCount = 0;
        {
            uint64_t probe = 0;
            while (probe < len && missCount < MAX_INFLIGHT)
            {
                uint64_t absolute = offset + probe;
                uint64_t blockNumber = absolute / VIRTIO_CACHE_BLOCK_SIZE;
                uint8_t* cacheBlock = nullptr;
                if (!CacheLookup(*s, blockNumber, &cacheBlock))
                {
                    // Avoid duplicate entries for the same block.
                    bool dup = false;
                    for (uint32_t j = 0; j < missCount; ++j)
                        if (missBlocks[j] == blockNumber) { dup = true; break; }
                    if (!dup)
                        missBlocks[missCount++] = blockNumber;
                }
                probe += VIRTIO_CACHE_BLOCK_SIZE - (absolute % VIRTIO_CACHE_BLOCK_SIZE);
            }
        }

        // Submit all misses as async slot reads.
        if (missCount > 0)
        {
            for (uint32_t i = 0; i < missCount; ++i)
            {
                uint64_t blockSector = missBlocks[i] * VIRTIO_CACHE_BLOCK_SECTORS;
                s->slots[i].blockNumber = missBlocks[i];
                SubmitSlotRead(*s, i, blockSector);
            }

            // Single notification for the whole batch.
            NotifyDevice(*s);

            // Wait for all slots to complete.
            if (!WaitAllSlots(*s, missCount))
            {
                ReleaseRequestLock(*s);
                return -1;
            }

            // Store all completed reads into the cache.
            for (uint32_t i = 0; i < missCount; ++i)
            {
                if (*s->slots[i].statusBuf != VIRTIO_BLK_S_OK)
                {
                    brook::SerialPrintf("virtio-blk: batch slot %u failed (block %lu)\n",
                                        i, static_cast<unsigned long>(missBlocks[i]));
                    ReleaseRequestLock(*s);
                    return -1;
                }
                CacheStore(*s, missBlocks[i], s->slots[i].dmaBuf);
            }
        }

        // All blocks now in cache — copy to user buffer.
        while (bytesRead < len)
        {
            uint64_t absolute = offset + bytesRead;
            uint64_t blockNumber = absolute / VIRTIO_CACHE_BLOCK_SIZE;
            uint64_t blockOffset = absolute % VIRTIO_CACHE_BLOCK_SIZE;
            uint8_t* cacheBlock = nullptr;
            CacheLookup(*s, blockNumber, &cacheBlock);

            uint64_t n = VIRTIO_CACHE_BLOCK_SIZE - blockOffset;
            if (n > len - bytesRead) n = len - bytesRead;
            memcpy(dstBytes + bytesRead, cacheBlock + blockOffset, n);
            bytesRead += n;
        }

        ReleaseRequestLock(*s);
        s->readOps++;
        s->readBytes += bytesRead;
        return static_cast<int>(bytesRead);
    }

    uint64_t sec = startSector;
    while (sec < endSector && bytesRead < len)
    {
        uint64_t batchStart = sec;
        uint32_t batch = static_cast<uint32_t>(endSector - sec);
        if (batch > SECTORS_PER_DMA) batch = SECTORS_PER_DMA;
        uint32_t dmaLen = batch * SECTOR_SIZE;

        if (!SubmitRequest(*s, VIRTIO_BLK_T_IN, sec, s->dmaBufPhys, dmaLen))
        {
            brook::SerialPrintf("virtio-blk: read failed at sector %lu\n",
                                static_cast<unsigned long>(sec));
            ReleaseRequestLock(*s);
            return -1;
        }

        CacheStoreFullBlocks(*s, batchStart, batch, s->dmaBuf);

        // Copy relevant bytes from the DMA buffer into the output.
        for (uint32_t i = 0; i < batch && bytesRead < len; ++i, ++sec)
        {
            uint64_t sectorStart = sec * SECTOR_SIZE;
            uint64_t copyStart   = (sectorStart < offset) ? (offset - sectorStart) : 0;
            uint64_t copyEnd     = SECTOR_SIZE;
            uint64_t remaining   = len - bytesRead;
            if (copyEnd - copyStart > remaining) copyEnd = copyStart + remaining;

            uint8_t* srcSector = s->dmaBuf + (i * SECTOR_SIZE);
            uint64_t n = copyEnd - copyStart;
            memcpy(dstBytes + bytesRead, srcSector + copyStart, n);
            bytesRead += n;
        }
    }

    ReleaseRequestLock(*s);
    s->readOps++;
    s->readBytes += bytesRead;
    return static_cast<int>(bytesRead);
}

static int VirtioBlkWrite(Device* dev, uint64_t offset, const void* buf, uint64_t len)
{
    auto* s = static_cast<VirtioBlkState*>(dev->priv);
    if (len == 0) return 0;

    static constexpr uint32_t SECTOR_SIZE = 512;
    static constexpr uint32_t DMA_BUF_SIZE = VirtioBlkState::DMA_BUF_PAGES * 4096;
    static constexpr uint32_t SECTORS_PER_DMA = DMA_BUF_SIZE / SECTOR_SIZE;

    // Guard against offset+len overflow and writes past device end
    uint64_t deviceBytes = s->sectorCount * SECTOR_SIZE;
    if (offset >= deviceBytes) return -1;
    if (len > deviceBytes - offset) len = deviceBytes - offset;

    uint64_t startSector = offset / SECTOR_SIZE;
    uint64_t endSector   = (offset + len + SECTOR_SIZE - 1) / SECTOR_SIZE;

    // Detect partial first/last sectors that need read-modify-write.
    bool partialFirst = (offset % SECTOR_SIZE) != 0;
    bool partialLast  = ((offset + len) % SECTOR_SIZE) != 0 && endSector > startSector;
    // Single-sector write that is partial on both ends:
    if (endSector - startSector == 1 && ((offset % SECTOR_SIZE) != 0 || (offset + len) % SECTOR_SIZE != 0))
        partialFirst = true;

    const uint8_t* srcBytes = static_cast<const uint8_t*>(buf);
    uint64_t bytesWritten = 0;
    auto* dmaBuf = s->dmaBuf;

    AcquireRequestLock(*s);

    uint64_t sec = startSector;
    while (sec < endSector && bytesWritten < len)
    {
        uint32_t batch = static_cast<uint32_t>(endSector - sec);
        if (batch > SECTORS_PER_DMA) batch = SECTORS_PER_DMA;
        uint32_t dmaLen = batch * SECTOR_SIZE;

        // Read-modify-write: pre-read sectors that will be partially overwritten
        // so we don't zero out data we're not writing.
        bool needPreRead = (partialFirst && sec == startSector) ||
                           (partialLast && sec + batch == endSector);
        if (needPreRead) {
            if (!SubmitRequest(*s, VIRTIO_BLK_T_IN, sec, s->dmaBufPhys, dmaLen)) {
                ReleaseRequestLock(*s);
                return -1;
            }
        }
        // No memset needed: when !needPreRead, all sectors in this batch
        // are fully overwritten by the memcpy loop below.

        for (uint32_t i = 0; i < batch && bytesWritten < len; ++i)
        {
            uint64_t sectorStart = (sec + i) * SECTOR_SIZE;
            uint64_t copyStart   = (sectorStart < offset) ? (offset - sectorStart) : 0;
            uint64_t copyEnd     = SECTOR_SIZE;
            uint64_t remaining   = len - bytesWritten;
            if (copyEnd - copyStart > remaining) copyEnd = copyStart + remaining;

            uint8_t* dstSector = dmaBuf + (i * SECTOR_SIZE);
            uint64_t n = copyEnd - copyStart;
            memcpy(dstSector + copyStart, srcBytes + bytesWritten, n);
            bytesWritten += n;
        }

        if (!SubmitRequest(*s, VIRTIO_BLK_T_OUT, sec, s->dmaBufPhys, dmaLen)) {
            ReleaseRequestLock(*s);
            return -1;
        }

        CacheInvalidateRange(*s, sec, sec + batch);
        sec += batch;
    }

    ReleaseRequestLock(*s);
    s->writeOps++;
    s->writeBytes += bytesWritten;
    return static_cast<int>(bytesWritten);
}

static int VirtioBlkIoctl(Device* dev, uint32_t cmd, void* arg)
{
    (void)dev; (void)cmd; (void)arg;
    return -1;
}

static void VirtioBlkClose(Device* /*dev*/) {}

static uint64_t VirtioBlkBlockCount(Device* dev)
{
    auto* s = static_cast<VirtioBlkState*>(dev->priv);
    return s->sectorCount;
}

static uint32_t VirtioBlkBlockSize(Device* /*dev*/)
{
    return 512;
}

static const BlockDeviceOps g_virtioBlkOps = {
    .read        = VirtioBlkRead,
    .write       = VirtioBlkWrite,
    .ioctl       = VirtioBlkIoctl,
    .close       = VirtioBlkClose,
    .block_count = VirtioBlkBlockCount,
    .block_size  = VirtioBlkBlockSize,
};

// ---- Per-device init (internal) ----

// Device names are static strings indexed by slot.
static const char* const g_virtioNames[] = {
    "virtio0", "virtio1", "virtio2", "virtio3",
    "virtio4", "virtio5", "virtio6", "virtio7",
};

static Device* InitOnePciDevice(const PciDevice& pci, uint32_t slot)
{
    if (!PciBarIsIo(pci.bar[0]))
    {
        SerialPuts("virtio-blk: BAR0 is not I/O space (not legacy device?)\n");
        return nullptr;
    }

    uint16_t ioBase = PciBarIoBase(pci.bar[0]);
    PciEnableBusMaster(pci);

    SerialPrintf("virtio-blk: found %02x:%02x.%x as %s, I/O base 0x%x\n",
                 pci.bus, pci.dev, pci.fn,
                 g_virtioNames[slot],
                 static_cast<unsigned>(ioBase));

    // 1. Reset device.
    VioWrite8(ioBase, VIRTIO_PCI_STATUS, 0);

    // 2. Acknowledge + driver.
    VioWrite8(ioBase, VIRTIO_PCI_STATUS,
              VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    // 3. Feature negotiation — accept everything the host offers.
    uint32_t features = VioRead32(ioBase, VIRTIO_PCI_HOST_FEATURES);
    VioWrite32(ioBase, VIRTIO_PCI_GUEST_FEATURES, features);

    // 4. Set up virtqueue 0.
    VioWrite16(ioBase, VIRTIO_PCI_QUEUE_SEL, 0);
    uint16_t qSize = VioRead16(ioBase, VIRTIO_PCI_QUEUE_SIZE);
    if (qSize == 0)
    {
        SerialPuts("virtio-blk: queue size is 0, skipping\n");
        return nullptr;
    }
    // Cap to our maximum — the device advertises its max, we use the smaller.
    if (qSize > MAX_QUEUE_SIZE) qSize = MAX_QUEUE_SIZE;
    SerialPrintf("virtio-blk: queue size=%u (device advertised, capped to max=%u)\n",
                 qSize, MAX_QUEUE_SIZE);

    auto* state = static_cast<VirtioBlkState*>(kmalloc(sizeof(VirtioBlkState)));
    if (!state) return nullptr;
    memset(state, 0, sizeof(VirtioBlkState));
    state->ioBase         = ioBase;
    state->queueSize      = qSize;
    state->availIdxShadow = 0;
    state->usedIdxShadow  = 0;

    if (!AllocVirtqueue(*state))
    {
        SerialPuts("virtio-blk: virtqueue allocation failed\n");
        kfree(state);
        return nullptr;
    }

    // Allocate persistent page-aligned DMA data buffer (64 KB).
    state->dmaBufPhys = PmmAllocPages(VirtioBlkState::DMA_BUF_PAGES, MemTag::KernelData).raw();
    if (state->dmaBufPhys == 0)
    {
        SerialPuts("virtio-blk: DMA buffer allocation failed\n");
        kfree(state);
        return nullptr;
    }
    state->dmaBuf = reinterpret_cast<uint8_t*>(PhysToVirt(PhysicalAddress(state->dmaBufPhys)).raw());

    state->cacheEntries = static_cast<VirtioBlkCacheEntry*>(
        kmalloc(sizeof(VirtioBlkCacheEntry) * VIRTIO_CACHE_ENTRIES));
    if (state->cacheEntries)
    {
        memset(state->cacheEntries, 0,
               sizeof(VirtioBlkCacheEntry) * VIRTIO_CACHE_ENTRIES);
        state->cacheData = reinterpret_cast<uint8_t*>(
            VmmAllocPages(VIRTIO_CACHE_ENTRIES, VMM_WRITABLE,
                          MemTag::KernelData, KernelPid).raw());
        if (!state->cacheData)
        {
            kfree(state->cacheEntries);
            state->cacheEntries = nullptr;
        }
        else
        {
            SerialPrintf("virtio-blk: read cache enabled (%u KiB)\n",
                         (VIRTIO_CACHE_ENTRIES * VIRTIO_CACHE_BLOCK_SIZE) / 1024);
        }
    }

    // Write queue PFN.
    uint32_t pfn = static_cast<uint32_t>(state->queuePhys >> 12);
    VioWrite32(ioBase, VIRTIO_PCI_QUEUE_PFN, pfn);

    // 5. Driver OK.
    VioWrite8(ioBase, VIRTIO_PCI_STATUS,
              VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    // 6. Register interrupt handler.
    // Read PCI interrupt line (offset 0x3C, low byte).
    uint8_t intLine = static_cast<uint8_t>(PciConfigRead32(pci.bus, pci.dev, pci.fn, 0x3C) & 0xFF);
    state->irqLine = intLine;
    state->irqComplete = 0;

    // Store state pointer for ISR lookup before registering.
    g_devStates[slot] = state;
    if (slot >= g_devCount) g_devCount = slot + 1;

    state->irqVector = IoApicRegisterHandler(intLine, VIRTIO_BLK_IRQ_VECTOR,
                                             reinterpret_cast<void*>(VirtioBlkIrqBody));
    SerialPrintf("virtio-blk: %s — IRQ %u, vector %u (interrupt-driven)\n",
                 g_virtioNames[slot], intLine, state->irqVector);

    // Read capacity (two 32-bit reads for the 64-bit sector count).
    uint32_t capLo = inl(ioBase + VIRTIO_PCI_BLK_CAPACITY);
    uint32_t capHi = inl(ioBase + VIRTIO_PCI_BLK_CAPACITY + 4);
    state->sectorCount = (static_cast<uint64_t>(capHi) << 32) | capLo;
    if (state->sectorCount == 0)
    {
        SerialPrintf("virtio-blk: %s — zero sector count, skipping\n",
                     g_virtioNames[slot]);
        kfree(state);
        return nullptr;
    }
    SerialPrintf("virtio-blk: %s — %lu sectors (%lu MB)\n",
                 g_virtioNames[slot],
                 state->sectorCount,
                 (state->sectorCount * 512) / (1024 * 1024));

    auto* dev = static_cast<Device*>(kmalloc(sizeof(Device)));
    if (!dev) { SerialPuts("virtio-blk: OOM allocating Device\n"); return nullptr; }
    dev->ops  = reinterpret_cast<const DeviceOps*>(&g_virtioBlkOps);
    dev->name = g_virtioNames[slot];
    dev->type = DeviceType::Block;
    dev->priv = state;

    if (!DeviceRegister(dev))
    {
        kfree(dev);
        kfree(state);
        return nullptr;
    }

    return dev;
}

// ---- Public init ----

uint32_t VirtioBlkInitAll()
{
    uint32_t count = 0;
    PciDevice pci;

    if (!PciFindDevice(0x1AF4, 0x1001, pci)) return 0;

    for (;;)
    {
        if (count >= VIRTIO_MAX_DEVS) break;
        InitOnePciDevice(pci, count);
        ++count;

        PciDevice next;
        if (!PciFindNextDevice(0x1AF4, 0x1001, pci, next)) break;
        pci = next;
    }

    return count;
}

// I/O statistics for procfs /proc/diskstats
void VirtioBlkGetStats(Device* dev, uint64_t& readOps, uint64_t& writeOps,
                       uint64_t& readBytes, uint64_t& writeBytes)
{
    readOps = writeOps = readBytes = writeBytes = 0;
    if (!dev || !dev->priv) return;
    auto* s = static_cast<VirtioBlkState*>(dev->priv);
    readOps    = s->readOps;
    writeOps   = s->writeOps;
    readBytes  = s->readBytes;
    writeBytes = s->writeBytes;
}

} // namespace brook
