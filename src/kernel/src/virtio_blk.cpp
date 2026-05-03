#include "virtio_blk.h"
#include "pci.h"
#include "portio.h"
#include "string.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "memory/heap.h"
#include "serial.h"
#include "mem_tag.h"
#include "process.h"
#include "scheduler.h"

namespace brook {

extern volatile uint64_t g_lapicTickCount;

// ---- virtio-blk PCI register offsets (legacy BAR0 I/O) ----

static constexpr uint8_t VIRTIO_PCI_HOST_FEATURES  = 0x00;
static constexpr uint8_t VIRTIO_PCI_GUEST_FEATURES = 0x04;
static constexpr uint8_t VIRTIO_PCI_QUEUE_PFN      = 0x08;
static constexpr uint8_t VIRTIO_PCI_QUEUE_SIZE     = 0x0C;
static constexpr uint8_t VIRTIO_PCI_QUEUE_SEL      = 0x0E;
static constexpr uint8_t VIRTIO_PCI_QUEUE_NOTIFY   = 0x10;
static constexpr uint8_t VIRTIO_PCI_STATUS         = 0x12;

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
static constexpr uint32_t VIRTIO_CACHE_BLOCK_SECTORS = 8;     // 4 KiB
static constexpr uint32_t VIRTIO_CACHE_BLOCK_SIZE    = 4096;
static constexpr uint32_t VIRTIO_CACHE_ENTRIES       = 4096;  // 16 MiB
static constexpr uint64_t VIRTIO_SMALL_READ_LIMIT    = 16 * 1024;

// ---- Virtqueue structures (packed for DMA) ----

struct __attribute__((packed)) VirtqDesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

// Maximum queue size we're willing to handle.  The device may advertise
// up to 32768; we cap to keep DMA allocation bounded (~32 KB total).
static constexpr uint32_t MAX_QUEUE_SIZE = 256;

struct __attribute__((packed)) VirtqUsedElem {
    uint32_t id;
    uint32_t len;
};

// virtio-blk request header (placed before the data buffer)
struct __attribute__((packed)) VirtioBlkReq {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
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

    // Persistent page-aligned DMA data buffer (64 KB = 128 sectors).
    // Larger buffer means fewer SubmitRequest round-trips for big reads.
    static constexpr uint32_t DMA_BUF_PAGES = 16;
    uint8_t*      dmaBuf;
    uint64_t      dmaBufPhys;

    VirtioBlkCacheEntry* cacheEntries;
    uint8_t*             cacheData;

    // Serialises concurrent requests from multiple processes.  The current
    // driver still uses hardcoded descriptor slots 0-2 and one shared DMA
    // buffer, so only one request can be in-flight at a time.
    volatile uint32_t requestGuardNext;
    volatile uint32_t requestGuardServing;
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
    for (uint32_t i = 0; i < totalPages * 4096; ++i) base[i] = 0;

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
    s.reqBuf         = reinterpret_cast<VirtioBlkReq*>(extraVirt);
    s.reqBufPhys     = VmmVirtToPhys(KernelPageTable, VirtualAddress(extraVirt)).raw();
    s.statusBuf      = reinterpret_cast<uint8_t*>(extraVirt + sizeof(VirtioBlkReq));
    s.statusBufPhys  = s.reqBufPhys + sizeof(VirtioBlkReq);

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

// ---- Synchronous request (polling, interrupts left enabled) ----

static bool SubmitRequest(VirtioBlkState& s,
                           uint32_t type, uint64_t sector,
                           uint64_t dataBufPhys, uint32_t dataLen)
{
    // Descriptor 0: request header (device-readable)
    s.reqBuf->type     = type;
    s.reqBuf->reserved = 0;
    s.reqBuf->sector   = sector;

    s.descTable[0].addr  = s.reqBufPhys;
    s.descTable[0].len   = sizeof(VirtioBlkReq);
    s.descTable[0].flags = VIRTQ_DESC_F_NEXT;
    s.descTable[0].next  = 1;

    // Descriptor 1: data buffer
    // For reads (VIRTIO_BLK_T_IN), the device writes data → VIRTQ_DESC_F_WRITE.
    // For writes (VIRTIO_BLK_T_OUT), we write data → no write flag.
    s.descTable[1].addr  = dataBufPhys;
    s.descTable[1].len   = dataLen;
    s.descTable[1].flags = VIRTQ_DESC_F_NEXT | (type == VIRTIO_BLK_T_IN ? VIRTQ_DESC_F_WRITE : 0);
    s.descTable[1].next  = 2;

    // Descriptor 2: status byte (device writes result)
    *s.statusBuf         = 0xFF; // sentinel
    s.descTable[2].addr  = s.statusBufPhys;
    s.descTable[2].len   = 1;
    s.descTable[2].flags = VIRTQ_DESC_F_WRITE;
    s.descTable[2].next  = 0;

    // Memory barrier before making descriptors visible.
    __asm__ volatile("mfence" ::: "memory");

    // Add head descriptor (0) to available ring.
    uint16_t slot = s.availIdxShadow % s.queueSize;
    s.availRing[slot] = 0;
    __asm__ volatile("mfence" ::: "memory");
    *s.availIdx = ++s.availIdxShadow;
    __asm__ volatile("mfence" ::: "memory");

    // Notify device that queue 0 has work.
    VioWrite16(s.ioBase, VIRTIO_PCI_QUEUE_NOTIFY, 0);

    // Hybrid wait: brief spin for fast completions, then yield CPU.
    // Phase 1: poll ~200 iterations (~2-5µs on modern CPUs). Catches
    // most QEMU completions without a context switch.
    for (uint32_t i = 0; i < 200; ++i)
    {
        if (*s.usedIdx != s.usedIdxShadow)
            goto done;
        __asm__ volatile("pause" ::: "memory");
    }

    // Phase 2: yield and re-check each timer tick (~1ms).
    {
        Process* self = ProcessCurrent();
        uint32_t yieldCount = 0;
        while (*s.usedIdx == s.usedIdxShadow)
        {
            if (++yieldCount > 5000) // ~5s timeout
            {
                SerialPuts("virtio-blk: timeout waiting for response\n");
                return false;
            }
            if (self)
            {
                self->wakeupTick = g_lapicTickCount + 1;
                SchedulerBlock(self);
            }
            else
            {
                // Early boot / no process context — fall back to pause
                for (uint32_t j = 0; j < 10000; ++j)
                    __asm__ volatile("pause" ::: "memory");
            }
        }
    }

done:
    __asm__ volatile("mfence" ::: "memory");
    ++s.usedIdxShadow;

    return (*s.statusBuf == VIRTIO_BLK_S_OK);
}

static void AcquireRequestLock(VirtioBlkState& s)
{
    uint32_t ticket = __atomic_fetch_add(&s.requestGuardNext, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&s.requestGuardServing, __ATOMIC_ACQUIRE) != ticket)
        __asm__ volatile("pause" ::: "memory");
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

    uint64_t startSector = offset / SECTOR_SIZE;
    uint64_t endSector   = (offset + len + SECTOR_SIZE - 1) / SECTOR_SIZE;

    // DMA buffers must be physically contiguous and page-aligned.
    // Use the persistent 64 KB DMA buffer (up to 128 sectors per request).
    uint8_t* dstBytes = static_cast<uint8_t*>(buf);
    uint64_t bytesRead = 0;

    // Serialise against concurrent reads/writes without masking timer IRQs
    // across device latency.
    AcquireRequestLock(*s);

    uint64_t roundedEndSector = endSector;
    bool cacheableSmallRead =
        len <= VIRTIO_SMALL_READ_LIMIT &&
        s->cacheEntries && s->cacheData &&
        roundedEndSector <= s->sectorCount &&
        ((((roundedEndSector - 1) / VIRTIO_CACHE_BLOCK_SECTORS) + 1) *
             VIRTIO_CACHE_BLOCK_SECTORS) <= s->sectorCount;

    if (cacheableSmallRead)
    {
        while (bytesRead < len)
        {
            uint64_t absolute = offset + bytesRead;
            uint64_t blockNumber = absolute / VIRTIO_CACHE_BLOCK_SIZE;
            uint64_t blockOffset = absolute % VIRTIO_CACHE_BLOCK_SIZE;
            uint8_t* cacheBlock = nullptr;

            if (!CacheLookup(*s, blockNumber, &cacheBlock))
            {
                uint64_t blockSector = blockNumber * VIRTIO_CACHE_BLOCK_SECTORS;
                if (!SubmitRequest(*s, VIRTIO_BLK_T_IN, blockSector,
                                   s->dmaBufPhys, VIRTIO_CACHE_BLOCK_SIZE))
                {
                    brook::SerialPrintf("virtio-blk: cached read failed at sector %lu\n",
                                        static_cast<unsigned long>(blockSector));
                    ReleaseRequestLock(*s);
                    return -1;
                }
                CacheStore(*s, blockNumber, s->dmaBuf);
                if (!CacheLookup(*s, blockNumber, &cacheBlock))
                {
                    ReleaseRequestLock(*s);
                    return -1;
                }
            }

            uint64_t n = VIRTIO_CACHE_BLOCK_SIZE - blockOffset;
            if (n > len - bytesRead) n = len - bytesRead;
            memcpy(dstBytes + bytesRead, cacheBlock + blockOffset, n);
            bytesRead += n;
        }

        ReleaseRequestLock(*s);
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
    return static_cast<int>(bytesRead);
}

static int VirtioBlkWrite(Device* dev, uint64_t offset, const void* buf, uint64_t len)
{
    auto* s = static_cast<VirtioBlkState*>(dev->priv);
    if (len == 0) return 0;

    static constexpr uint32_t SECTOR_SIZE = 512;
    static constexpr uint32_t DMA_BUF_SIZE = VirtioBlkState::DMA_BUF_PAGES * 4096;
    static constexpr uint32_t SECTORS_PER_DMA = DMA_BUF_SIZE / SECTOR_SIZE;

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
        } else {
            memset(dmaBuf, 0, dmaLen);
        }

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
    for (uint32_t i = 0; i < sizeof(VirtioBlkState); ++i)
        reinterpret_cast<uint8_t*>(state)[i] = 0;
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

    // Read capacity (two 32-bit reads for the 64-bit sector count).
    uint32_t capLo = inl(ioBase + VIRTIO_PCI_BLK_CAPACITY);
    uint32_t capHi = inl(ioBase + VIRTIO_PCI_BLK_CAPACITY + 4);
    state->sectorCount = (static_cast<uint64_t>(capHi) << 32) | capLo;
    SerialPrintf("virtio-blk: %s — %lu sectors (%lu MB)\n",
                 g_virtioNames[slot],
                 state->sectorCount,
                 (state->sectorCount * 512) / (1024 * 1024));

    auto* dev = static_cast<Device*>(kmalloc(sizeof(Device)));
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

} // namespace brook
