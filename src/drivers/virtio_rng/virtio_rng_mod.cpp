// virtio_rng_mod.cpp — virtio-rng driver for QEMU virtio-rng-pci.
//
// Uses modern virtio PCI transport. Single virtqueue (requestq).
// The driver posts buffers to the device, which fills them with random data.
//
// Provides entropy via RngFill() which the kernel's sys_getrandom can call.
//
// Virtio spec: §5.4 (Entropy Device)

#include "module_abi.h"
#include "pci.h"
#include "serial.h"
#include "kprintf.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "memory/address.h"
#include "mem_tag.h"

MODULE_IMPORT_SYMBOL(PciFindDevice);
MODULE_IMPORT_SYMBOL(PciEnableMemSpace);
MODULE_IMPORT_SYMBOL(PciEnableBusMaster);
MODULE_IMPORT_SYMBOL(PciConfigRead32);
MODULE_IMPORT_SYMBOL(PciConfigRead16);
MODULE_IMPORT_SYMBOL(PciConfigRead8);
MODULE_IMPORT_SYMBOL(SerialPrintf);
MODULE_IMPORT_SYMBOL(SerialPuts);
MODULE_IMPORT_SYMBOL(KPrintf);
MODULE_IMPORT_SYMBOL(VmmAllocPages);
MODULE_IMPORT_SYMBOL(VmmVirtToPhys);
MODULE_IMPORT_SYMBOL(VmmMapPage);

using namespace brook;

// ---------------------------------------------------------------------------
// Modern virtio PCI capability types
// ---------------------------------------------------------------------------

static constexpr uint8_t VIRTIO_PCI_CAP_COMMON_CFG  = 1;
static constexpr uint8_t VIRTIO_PCI_CAP_NOTIFY_CFG  = 2;
static constexpr uint8_t VIRTIO_PCI_CAP_ISR_CFG     = 3;

enum VirtioCommonReg : uint32_t {
    VIRTIO_COMMON_DFSELECT      = 0x00,
    VIRTIO_COMMON_DF            = 0x04,
    VIRTIO_COMMON_GFSELECT      = 0x08,
    VIRTIO_COMMON_GF            = 0x0C,
    VIRTIO_COMMON_MSIX_CFG      = 0x10,
    VIRTIO_COMMON_NUM_QUEUES    = 0x12,
    VIRTIO_COMMON_STATUS        = 0x14,
    VIRTIO_COMMON_CFGGEN        = 0x15,
    VIRTIO_COMMON_Q_SELECT      = 0x16,
    VIRTIO_COMMON_Q_SIZE        = 0x18,
    VIRTIO_COMMON_Q_MSIX_VEC    = 0x1A,
    VIRTIO_COMMON_Q_ENABLE      = 0x1C,
    VIRTIO_COMMON_Q_NOTIFY_OFF  = 0x1E,
    VIRTIO_COMMON_Q_DESC        = 0x20,
    VIRTIO_COMMON_Q_AVAIL       = 0x28,
    VIRTIO_COMMON_Q_USED        = 0x30,
};

static constexpr uint8_t VIRTIO_STATUS_ACKNOWLEDGE = 1;
static constexpr uint8_t VIRTIO_STATUS_DRIVER      = 2;
static constexpr uint8_t VIRTIO_STATUS_FEATURES_OK = 8;
static constexpr uint8_t VIRTIO_STATUS_DRIVER_OK   = 4;

static constexpr uint16_t VIRTQ_DESC_F_WRITE = 2;

// ---------------------------------------------------------------------------
// MMIO accessors
// ---------------------------------------------------------------------------

static volatile uint8_t* g_commonCfg = nullptr;
static volatile uint8_t* g_notifyCfg = nullptr;
static volatile uint8_t* g_isrCfg    = nullptr;
static uint32_t          g_notifyMultiplier = 0;

static inline void mmio_write8(volatile uint8_t* base, uint32_t off, uint8_t v)
{ *reinterpret_cast<volatile uint8_t*>(base + off) = v; }
static inline void mmio_write16(volatile uint8_t* base, uint32_t off, uint16_t v)
{ *reinterpret_cast<volatile uint16_t*>(base + off) = v; }
static inline void mmio_write32(volatile uint8_t* base, uint32_t off, uint32_t v)
{ *reinterpret_cast<volatile uint32_t*>(base + off) = v; }
static inline void mmio_write64(volatile uint8_t* base, uint32_t off, uint64_t v)
{ *reinterpret_cast<volatile uint64_t*>(base + off) = v; }

static inline uint8_t mmio_read8(volatile uint8_t* base, uint32_t off)
{ return *reinterpret_cast<volatile uint8_t*>(base + off); }
static inline uint16_t mmio_read16(volatile uint8_t* base, uint32_t off)
{ return *reinterpret_cast<volatile uint16_t*>(base + off); }

// ---------------------------------------------------------------------------
// Virtqueue (single queue for RNG)
// ---------------------------------------------------------------------------

struct __attribute__((packed)) VirtqDesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct __attribute__((packed)) VirtqUsedElem {
    uint32_t id;
    uint32_t len;
};

static constexpr uint32_t QUEUE_SIZE = 16;
static constexpr uint32_t RNG_BUF_SIZE = 64; // bytes per request

struct VirtqState {
    VirtqDesc*         descTable;
    uint16_t*          availFlags;
    uint16_t*          availIdx;
    uint16_t*          availRing;
    volatile uint16_t* usedIdx;
    VirtqUsedElem*     usedRing;

    uint64_t descPhys;
    uint64_t availPhys;
    uint64_t usedPhys;

    uint16_t size;
    uint16_t availIdxShadow;
    uint16_t usedIdxShadow;
    uint16_t notifyOff;
};

static VirtqState g_rngq;

// Buffer for collecting random data
static uint8_t* g_rngBuf = nullptr;
static uint64_t g_rngBufPhys = 0;

// Entropy pool (filled by device, consumed by kernel)
static constexpr uint32_t POOL_SIZE = 4096;
static uint8_t  g_entropyPool[POOL_SIZE];
static uint32_t g_poolHead = 0;
static uint32_t g_poolCount = 0;
static bool g_initialized = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

static bool AllocQueue(VirtqState& q)
{
    q.size = QUEUE_SIZE;
    uint32_t N = QUEUE_SIZE;

    uint32_t descSize  = 16 * N;
    uint32_t availSize = 6 + 2 * N;
    uint32_t usedSize  = 6 + 8 * N;

    uint32_t descPages  = AlignUp(descSize, 4096) / 4096;
    uint32_t availPages = AlignUp(availSize, 4096) / 4096;
    uint32_t usedPages  = AlignUp(usedSize, 4096) / 4096;
    uint32_t totalPages = descPages + availPages + usedPages;

    auto qAddr = VmmAllocPages(totalPages, VMM_WRITABLE, MemTag::Device, KernelPid);
    if (!qAddr) return false;

    uint8_t* base = reinterpret_cast<uint8_t*>(qAddr.raw());
    for (uint32_t i = 0; i < totalPages * 4096; ++i) base[i] = 0;

    q.descTable  = reinterpret_cast<VirtqDesc*>(base);
    q.availFlags = reinterpret_cast<uint16_t*>(base + descPages * 4096);
    q.availIdx   = q.availFlags + 1;
    q.availRing  = q.availFlags + 2;
    q.usedIdx    = reinterpret_cast<volatile uint16_t*>(base + (descPages + availPages) * 4096 + 2);
    q.usedRing   = reinterpret_cast<VirtqUsedElem*>(base + (descPages + availPages) * 4096 + 4);

    q.descPhys  = VmmVirtToPhys(KernelPageTable, VirtualAddress(reinterpret_cast<uint64_t>(q.descTable))).raw();
    q.availPhys = VmmVirtToPhys(KernelPageTable, VirtualAddress(reinterpret_cast<uint64_t>(q.availFlags))).raw();
    uint8_t* usedBase = base + (descPages + availPages) * 4096;
    q.usedPhys  = VmmVirtToPhys(KernelPageTable, VirtualAddress(reinterpret_cast<uint64_t>(usedBase))).raw();

    q.availIdxShadow = 0;
    q.usedIdxShadow  = 0;

    return true;
}

// ---------------------------------------------------------------------------
// Submit a request for random data
// ---------------------------------------------------------------------------

static void SubmitRngRequest()
{
    uint16_t idx = g_rngq.availIdxShadow % g_rngq.size;

    g_rngq.descTable[idx].addr  = g_rngBufPhys;
    g_rngq.descTable[idx].len   = RNG_BUF_SIZE;
    g_rngq.descTable[idx].flags = VIRTQ_DESC_F_WRITE; // device writes
    g_rngq.descTable[idx].next  = 0;

    g_rngq.availRing[g_rngq.availIdxShadow % g_rngq.size] = idx;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_rngq.availIdxShadow++;
    *g_rngq.availIdx = g_rngq.availIdxShadow;
    __atomic_thread_fence(__ATOMIC_RELEASE);

    // Notify device
    mmio_write16(g_notifyCfg, g_rngq.notifyOff * g_notifyMultiplier, 0);
}

// ---------------------------------------------------------------------------
// Collect completed random data
// ---------------------------------------------------------------------------

static void CollectEntropy()
{
    while (g_rngq.usedIdxShadow != *g_rngq.usedIdx)
    {
        uint16_t usedSlot = g_rngq.usedIdxShadow % g_rngq.size;
        uint32_t len = g_rngq.usedRing[usedSlot].len;
        g_rngq.usedIdxShadow++;

        if (len > RNG_BUF_SIZE) len = RNG_BUF_SIZE;

        // Copy to entropy pool (simple ring buffer)
        for (uint32_t i = 0; i < len && g_poolCount < POOL_SIZE; i++)
        {
            g_entropyPool[g_poolHead] = g_rngBuf[i];
            g_poolHead = (g_poolHead + 1) % POOL_SIZE;
            if (g_poolCount < POOL_SIZE) g_poolCount++;
        }
    }
}

// ---------------------------------------------------------------------------
// Public API: fill buffer with random data from entropy pool
// ---------------------------------------------------------------------------

extern "C" int VirtioRngFill(void* buf, uint32_t len)
{
    if (!g_initialized) return -1;

    auto* out = static_cast<uint8_t*>(buf);
    uint32_t filled = 0;

    // Try to drain from pool first
    while (filled < len && g_poolCount > 0)
    {
        uint32_t tail = (g_poolHead - g_poolCount + POOL_SIZE) % POOL_SIZE;
        out[filled++] = g_entropyPool[tail];
        g_poolCount--;
    }

    // If pool empty, submit request and busy-wait (short)
    while (filled < len)
    {
        SubmitRngRequest();

        // Busy-wait for completion (virtio-rng on QEMU is near-instant)
        for (int spin = 0; spin < 10000; spin++)
        {
            if (g_rngq.usedIdxShadow != *g_rngq.usedIdx) break;
            __asm__ volatile("pause");
        }

        CollectEntropy();

        while (filled < len && g_poolCount > 0)
        {
            uint32_t tail = (g_poolHead - g_poolCount + POOL_SIZE) % POOL_SIZE;
            out[filled++] = g_entropyPool[tail];
            g_poolCount--;
        }
    }

    return static_cast<int>(filled);
}

// ---------------------------------------------------------------------------
// PCI capability walking
// ---------------------------------------------------------------------------

static bool FindVirtioCaps(const PciDevice& dev)
{
    uint8_t capPtr = PciConfigRead8(dev.bus, dev.dev, dev.fn, 0x34) & 0xFC;

    while (capPtr)
    {
        uint8_t capId  = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr);

        if (capId == 0x09) // Vendor-specific (virtio)
        {
            uint8_t cfgType = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr + 3);
            uint8_t bar     = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr + 4);
            uint32_t offset = PciConfigRead32(dev.bus, dev.dev, dev.fn, capPtr + 8);
            uint32_t length = PciConfigRead32(dev.bus, dev.dev, dev.fn, capPtr + 12);

            uint32_t barAddr = PciConfigRead32(dev.bus, dev.dev, dev.fn, 0x10 + bar * 4) & ~0xFu;
            uint64_t mmioBase = barAddr + offset;

            // Map the MMIO range into kernel virtual address space
            uint32_t pages = AlignUp(length, 4096) / 4096;
            if (pages == 0) pages = 1;
            auto va = VmmAllocPages(pages, VMM_WRITABLE, MemTag::Device, KernelPid);
            if (!va) { capPtr = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr + 1); continue; }
            uint64_t physBase = mmioBase & ~0xFFFULL;
            for (uint32_t p = 0; p < pages; p++)
            {
                VmmMapPage(KernelPageTable,
                           VirtualAddress(va.raw() + p * 4096),
                           PhysicalAddress(physBase + p * 4096),
                           VMM_WRITABLE | VMM_CACHE_DISABLE,
                           MemTag::Device, KernelPid);
            }

            volatile uint8_t* ptr = reinterpret_cast<volatile uint8_t*>(va.raw() + (mmioBase & 0xFFF));
            switch (cfgType) {
                case VIRTIO_PCI_CAP_COMMON_CFG: g_commonCfg = ptr; break;
                case VIRTIO_PCI_CAP_NOTIFY_CFG:
                    g_notifyCfg = ptr;
                    g_notifyMultiplier = PciConfigRead32(dev.bus, dev.dev, dev.fn, capPtr + 16);
                    break;
                case VIRTIO_PCI_CAP_ISR_CFG:    g_isrCfg = ptr; break;
            }
        }

        capPtr = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr + 1);
    }

    return g_commonCfg && g_notifyCfg;
}

// ---------------------------------------------------------------------------
// Module init
// ---------------------------------------------------------------------------

static int VirtioRngInit()
{
    PciDevice dev;
    // Modern device ID: 0x1044
    if (!PciFindDevice(0x1AF4, 0x1044, dev))
    {
        // Transitional: 0x1005
        if (!PciFindDevice(0x1AF4, 0x1005, dev))
        {
            SerialPuts("virtio_rng: device not found\n");
            return -1;
        }
    }

    KPrintf("virtio_rng: found at PCI %02x:%02x.%x\n",
            dev.bus, dev.dev, dev.fn);

    PciEnableMemSpace(dev);
    PciEnableBusMaster(dev);

    if (!FindVirtioCaps(dev))
    {
        SerialPuts("virtio_rng: failed to find virtio capabilities\n");
        return -1;
    }

    // Reset device
    mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS, 0);
    // Acknowledge
    mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    // No features to negotiate for RNG
    mmio_write32(g_commonCfg, VIRTIO_COMMON_DFSELECT, 0);
    mmio_write32(g_commonCfg, VIRTIO_COMMON_GFSELECT, 0);
    mmio_write32(g_commonCfg, VIRTIO_COMMON_GF, 0);
    mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);

    uint8_t status = mmio_read8(g_commonCfg, VIRTIO_COMMON_STATUS);
    if (!(status & VIRTIO_STATUS_FEATURES_OK))
    {
        SerialPuts("virtio_rng: features not accepted\n");
        return -1;
    }

    // Set up queue 0 (requestq)
    mmio_write16(g_commonCfg, VIRTIO_COMMON_Q_SELECT, 0);
    uint16_t qsize = mmio_read16(g_commonCfg, VIRTIO_COMMON_Q_SIZE);
    if (qsize > QUEUE_SIZE) qsize = QUEUE_SIZE;
    mmio_write16(g_commonCfg, VIRTIO_COMMON_Q_SIZE, qsize);

    if (!AllocQueue(g_rngq))
    {
        SerialPuts("virtio_rng: queue alloc failed\n");
        return -1;
    }

    g_rngq.notifyOff = mmio_read16(g_commonCfg, VIRTIO_COMMON_Q_NOTIFY_OFF);

    // Tell device queue addresses
    mmio_write64(g_commonCfg, VIRTIO_COMMON_Q_DESC, g_rngq.descPhys);
    mmio_write64(g_commonCfg, VIRTIO_COMMON_Q_AVAIL, g_rngq.availPhys);
    mmio_write64(g_commonCfg, VIRTIO_COMMON_Q_USED, g_rngq.usedPhys);
    mmio_write16(g_commonCfg, VIRTIO_COMMON_Q_ENABLE, 1);

    // Allocate RNG buffer
    auto bufAddr = VmmAllocPages(1, VMM_WRITABLE, MemTag::Device, KernelPid);
    if (!bufAddr)
    {
        SerialPuts("virtio_rng: buffer alloc failed\n");
        return -1;
    }
    g_rngBuf = reinterpret_cast<uint8_t*>(bufAddr.raw());
    g_rngBufPhys = VmmVirtToPhys(KernelPageTable, bufAddr).raw();

    // Mark device as ready
    mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    g_initialized = true;

    // Pre-fill entropy pool
    SubmitRngRequest();
    for (int spin = 0; spin < 100000; spin++)
    {
        if (g_rngq.usedIdxShadow != *g_rngq.usedIdx) break;
        __asm__ volatile("pause");
    }
    CollectEntropy();

    KPrintf("virtio_rng: initialised, %u bytes in entropy pool\n", g_poolCount);
    return 0;
}

static void VirtioRngExit()
{
    g_initialized = false;
    if (g_commonCfg)
        mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS, 0);
    SerialPuts("virtio_rng: unloaded\n");
}

DECLARE_MODULE("virtio_rng", VirtioRngInit, VirtioRngExit,
               "Virtio entropy source (random number generator)");
