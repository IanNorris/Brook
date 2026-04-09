#include "virtio_blk.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "heap.h"
#include "serial.h"
#include "mem_tag.h"

namespace brook {

// ---- Port I/O ----

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

// inb — available for future 8-bit register reads (e.g. ISR status).
[[maybe_unused]] static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

// ---- virtio-blk PCI register offsets (legacy BAR0 I/O) ----

static constexpr uint8_t VIRTIO_PCI_HOST_FEATURES  = 0x00;
static constexpr uint8_t VIRTIO_PCI_GUEST_FEATURES = 0x04;
static constexpr uint8_t VIRTIO_PCI_QUEUE_PFN      = 0x08;
static constexpr uint8_t VIRTIO_PCI_QUEUE_SIZE     = 0x0C;
static constexpr uint8_t VIRTIO_PCI_QUEUE_SEL      = 0x0E;
static constexpr uint8_t VIRTIO_PCI_QUEUE_NOTIFY   = 0x10;
static constexpr uint8_t VIRTIO_PCI_STATUS         = 0x12;
// VIRTIO_PCI_ISR not used in polling mode (interrupt-free).

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

// ---- Virtqueue structures (packed for DMA) ----

struct __attribute__((packed)) VirtqDesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

static constexpr uint32_t QUEUE_SIZE = 64; // must be power of 2

struct __attribute__((packed)) VirtqAvail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QUEUE_SIZE];
    uint16_t used_event;
};

struct __attribute__((packed)) VirtqUsedElem {
    uint32_t id;
    uint32_t len;
};

struct __attribute__((packed)) VirtqUsed {
    uint16_t          flags;
    uint16_t          idx;
    VirtqUsedElem     ring[QUEUE_SIZE];
    uint16_t          avail_event;
};

// virtio-blk request header (placed before the data buffer)
struct __attribute__((packed)) VirtioBlkReq {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

// ---- Driver state ----

struct VirtioBlkState {
    uint16_t    ioBase;         // BAR0 I/O port base

    // Virtqueue memory (physically contiguous, 4KB-aligned)
    VirtqDesc*  descTable;
    VirtqAvail* availRing;
    VirtqUsed*  usedRing;
    uint64_t    queuePhys;      // physical base of descriptor table

    uint16_t    availIdx;       // next available ring index to write
    uint16_t    usedIdx;        // last consumed used ring index

    uint64_t    sectorCount;    // total sectors on the device

    // Per-request DMA buffers (reused; one request at a time)
    VirtioBlkReq* reqBuf;
    uint64_t      reqBufPhys;
    uint8_t*      statusBuf;
    uint64_t      statusBufPhys;
};

// ---- Register helpers ----

static inline uint32_t VioRead32(uint16_t base, uint8_t reg)  { return inl(base + reg); }
static inline uint16_t VioRead16(uint16_t base, uint8_t reg)  { return inw(base + reg); }
// VioRead8 reserved for future use (e.g. ISR register reads).
static inline void VioWrite32(uint16_t base, uint8_t reg, uint32_t v) { outl(base + reg, v); }
static inline void VioWrite16(uint16_t base, uint8_t reg, uint16_t v)
{
    // 16-bit I/O write
    __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(static_cast<uint16_t>(base + reg)));
}
static inline void VioWrite8 (uint16_t base, uint8_t reg, uint8_t v)  { outb(base + reg, v); }

// ---- Virtqueue DMA allocation ----
// Layout: [Desc table (1K)] [Avail ring (~140B)] [pad to 4K] [Used ring (~520B)]
// We allocate 2 pages (8KB) to hold everything.

static constexpr uint32_t DESC_TABLE_SIZE   = sizeof(VirtqDesc)  * QUEUE_SIZE;
static constexpr uint32_t AVAIL_RING_OFFSET = DESC_TABLE_SIZE;
// Used ring must start at the next page boundary after avail ring.
static constexpr uint32_t USED_RING_OFFSET  = 4096; // second page

static bool AllocVirtqueue(VirtioBlkState& s)
{
    // 2 pages for the virtqueue, 1 page for req + status buffers.
    uint64_t qVirt = VmmAllocPages(3, VMM_WRITABLE, MemTag::Device, KernelPid);
    if (!qVirt) return false;

    // Zero the pages.
    uint8_t* base = reinterpret_cast<uint8_t*>(qVirt);
    for (uint32_t i = 0; i < 3 * 4096; ++i) base[i] = 0;

    s.descTable  = reinterpret_cast<VirtqDesc*> (qVirt + 0);
    s.availRing  = reinterpret_cast<VirtqAvail*>(qVirt + AVAIL_RING_OFFSET);
    s.usedRing   = reinterpret_cast<VirtqUsed*> (qVirt + USED_RING_OFFSET);
    s.queuePhys  = VmmVirtToPhys(qVirt);

    // Request/status buffers live on page 3.
    uint64_t extraVirt = qVirt + 2 * 4096;
    s.reqBuf         = reinterpret_cast<VirtioBlkReq*>(extraVirt);
    s.reqBufPhys     = VmmVirtToPhys(extraVirt);
    s.statusBuf      = reinterpret_cast<uint8_t*>(extraVirt + sizeof(VirtioBlkReq));
    s.statusBufPhys  = s.reqBufPhys + sizeof(VirtioBlkReq);

    return true;
}

// ---- Synchronous request (polling, no interrupts) ----

static bool SubmitRequest(VirtioBlkState& s,
                          uint32_t type, uint64_t sector,
                          void* dataBuf, uint32_t dataLen)
{
    // Descriptor 0: request header (device-readable)
    s.reqBuf->type     = type;
    s.reqBuf->reserved = 0;
    s.reqBuf->sector   = sector;

    uint64_t dataBufPhys = VmmVirtToPhys(reinterpret_cast<uint64_t>(dataBuf));

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
    uint16_t slot = s.availIdx % QUEUE_SIZE;
    s.availRing->ring[slot] = 0;
    __asm__ volatile("mfence" ::: "memory");
    s.availRing->idx = ++s.availIdx;
    __asm__ volatile("mfence" ::: "memory");

    // Notify device that queue 0 has work.
    VioWrite16(s.ioBase, VIRTIO_PCI_QUEUE_NOTIFY, 0);

    // Poll used ring until device consumes the request.
    uint32_t spins = 0;
    while (s.usedRing->idx == s.usedIdx)
    {
        __asm__ volatile("pause");
        if (++spins > 10000000u)
        {
            SerialPuts("virtio-blk: timeout waiting for response\n");
            return false;
        }
    }
    __asm__ volatile("mfence" ::: "memory");
    ++s.usedIdx;

    return (*s.statusBuf == VIRTIO_BLK_S_OK);
}

// ---- DeviceOps ----

static int VirtioBlkRead(Device* dev, uint64_t offset, void* buf, uint64_t len)
{
    auto* s = static_cast<VirtioBlkState*>(dev->priv);
    if (len == 0) return 0;

    uint32_t blockSize = 512;
    uint64_t startSector = offset / blockSize;
    uint64_t endSector   = (offset + len + blockSize - 1) / blockSize;
    uint64_t sectorCount = endSector - startSector;

    // Use a scratch buffer for partial-sector reads.
    auto* tmp = static_cast<uint8_t*>(kmalloc(sectorCount * blockSize));
    if (!tmp) return -1;

    bool ok = SubmitRequest(*s, VIRTIO_BLK_T_IN, startSector,
                            tmp, static_cast<uint32_t>(sectorCount * blockSize));
    if (ok)
    {
        // Copy relevant portion to caller's buffer.
        uint64_t startOffset = offset % blockSize;
        uint8_t* src = tmp + startOffset;
        uint8_t* dst = static_cast<uint8_t*>(buf);
        for (uint64_t i = 0; i < len; ++i) dst[i] = src[i];
    }
    kfree(tmp);
    return ok ? static_cast<int>(len) : -1;
}

static int VirtioBlkWrite(Device* dev, uint64_t offset, const void* buf, uint64_t len)
{
    auto* s = static_cast<VirtioBlkState*>(dev->priv);
    if (len == 0) return 0;

    uint32_t blockSize = 512;
    uint64_t startSector = offset / blockSize;
    uint64_t sectorCount = (len + blockSize - 1) / blockSize;

    auto* tmp = static_cast<uint8_t*>(kmalloc(sectorCount * blockSize));
    if (!tmp) return -1;

    // Zero-fill then copy data.
    for (uint64_t i = 0; i < sectorCount * blockSize; ++i) tmp[i] = 0;
    const uint8_t* src = static_cast<const uint8_t*>(buf);
    uint64_t startOffset = offset % blockSize;
    for (uint64_t i = 0; i < len; ++i) tmp[startOffset + i] = src[i];

    bool ok = SubmitRequest(*s, VIRTIO_BLK_T_OUT, startSector,
                            tmp, static_cast<uint32_t>(sectorCount * blockSize));
    kfree(tmp);
    return ok ? static_cast<int>(len) : -1;
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
static constexpr uint32_t VIRTIO_MAX_DEVS = 8;

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
    if (qSize == 0 || qSize > QUEUE_SIZE)
    {
        SerialPrintf("virtio-blk: unexpected queue size %u, skipping\n",
                     static_cast<unsigned>(qSize));
        return nullptr;
    }

    auto* state = static_cast<VirtioBlkState*>(kmalloc(sizeof(VirtioBlkState)));
    if (!state) return nullptr;
    for (uint32_t i = 0; i < sizeof(VirtioBlkState); ++i)
        reinterpret_cast<uint8_t*>(state)[i] = 0;
    state->ioBase   = ioBase;
    state->availIdx = 0;
    state->usedIdx  = 0;

    if (!AllocVirtqueue(*state))
    {
        SerialPuts("virtio-blk: virtqueue allocation failed\n");
        kfree(state);
        return nullptr;
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
