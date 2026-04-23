// virtio_net_mod.cpp — virtio-net driver for QEMU virtio-net-pci.
//
// Uses modern virtio PCI transport (capability-based MMIO).
// Two virtqueues: 0 = receiveq (device→driver), 1 = transmitq (driver→device).
//
// Each RX/TX buffer is prefixed by a virtio_net_hdr (10 bytes without
// VIRTIO_NET_F_MRG_RXBUF, 12 bytes with).
//
// Virtio-net spec: virtio 1.0 §5.1

#include "module_abi.h"
#include "pci.h"
#include "serial.h"
#include "kprintf.h"
#include "idt.h"
#include "apic.h"
#include "net.h"
#include "spinlock.h"
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
MODULE_IMPORT_SYMBOL(IoApicRegisterHandler);
MODULE_IMPORT_SYMBOL(NetRegisterIf);
MODULE_IMPORT_SYMBOL(NetReceive);

using namespace brook;

// ---------------------------------------------------------------------------
// Modern virtio PCI capability types (same as virtio_input)
// ---------------------------------------------------------------------------

static constexpr uint8_t VIRTIO_PCI_CAP_COMMON_CFG  = 1;
static constexpr uint8_t VIRTIO_PCI_CAP_NOTIFY_CFG  = 2;
static constexpr uint8_t VIRTIO_PCI_CAP_ISR_CFG     = 3;
static constexpr uint8_t VIRTIO_PCI_CAP_DEVICE_CFG  = 4;

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

// Virtqueue descriptor flags
static constexpr uint16_t VIRTQ_DESC_F_WRITE = 2;

// Virtio-net feature bits
static constexpr uint32_t VIRTIO_NET_F_MAC         = (1 << 5);
// We explicitly do NOT negotiate VIRTIO_NET_F_MRG_RXBUF to keep header at 10 bytes.

// Virtio-net header (without mergeable rx buffers)
struct __attribute__((packed)) VirtioNetHdr {
    uint8_t  flags;
    uint8_t  gsoType;
    uint16_t hdrLen;
    uint16_t gsoSize;
    uint16_t csumStart;
    uint16_t csumOffset;
    uint16_t numBuffers;   // Always present with VIRTIO_F_VERSION_1
};

static constexpr uint32_t VIRTIO_NET_HDR_SIZE = sizeof(VirtioNetHdr); // 10

// ---------------------------------------------------------------------------
// Virtqueue structures
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

static constexpr uint32_t MAX_QUEUE_SIZE = 256;
static constexpr uint32_t RX_BUF_SIZE = 2048; // per-descriptor buffer

// ---------------------------------------------------------------------------
// MMIO register accessors
// ---------------------------------------------------------------------------

static volatile uint8_t* g_commonCfg = nullptr;
static volatile uint8_t* g_notifyCfg = nullptr;
static volatile uint8_t* g_isrCfg    = nullptr;
static volatile uint8_t* g_deviceCfg = nullptr;
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
static inline uint32_t mmio_read32(volatile uint8_t* base, uint32_t off)
{ return *reinterpret_cast<volatile uint32_t*>(base + off); }

// ---------------------------------------------------------------------------
// Per-queue state
// ---------------------------------------------------------------------------

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

static VirtqState g_rxq; // queue 0 — receiveq
static VirtqState g_txq; // queue 1 — transmitq

// RX packet buffers (one per descriptor, physically contiguous)
static uint8_t* g_rxBufs = nullptr;
static uint64_t g_rxBufsPhys = 0;

// TX packet buffer (single, reused)
static uint8_t* g_txBuf = nullptr;
static uint64_t g_txBufPhys = 0;

static NetIf g_netIf;

static constexpr uint8_t VIRTIO_NET_IRQ_VECTOR = 47;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// ---------------------------------------------------------------------------
// Queue allocation (modern: desc/avail/used on separate pages)
// ---------------------------------------------------------------------------

static bool AllocQueue(VirtqState& q, uint16_t queueSize)
{
    q.size = queueSize;
    uint32_t N = queueSize;

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

    // Compute per-section physical addresses individually (VmmAllocPages is not physically contiguous)
    q.descPhys  = VmmVirtToPhys(KernelPageTable, VirtualAddress(reinterpret_cast<uint64_t>(q.descTable))).raw();
    q.availPhys = VmmVirtToPhys(KernelPageTable, VirtualAddress(reinterpret_cast<uint64_t>(q.availFlags))).raw();
    uint8_t* usedBase = base + (descPages + availPages) * 4096;
    q.usedPhys  = VmmVirtToPhys(KernelPageTable, VirtualAddress(reinterpret_cast<uint64_t>(usedBase))).raw();

    q.availIdxShadow = 0;
    q.usedIdxShadow  = 0;

    return true;
}

// ---------------------------------------------------------------------------
// RX buffer management
// ---------------------------------------------------------------------------

static bool AllocRxBuffers()
{
    uint32_t totalSize = g_rxq.size * RX_BUF_SIZE;
    uint32_t pages = AlignUp(totalSize, 4096) / 4096;

    auto vaddr = VmmAllocPages(pages, VMM_WRITABLE, MemTag::Device, KernelPid);
    if (!vaddr) return false;

    g_rxBufs = reinterpret_cast<uint8_t*>(vaddr.raw());
    g_rxBufsPhys = VmmVirtToPhys(KernelPageTable, vaddr).raw();

    // Zero all buffers
    for (uint32_t i = 0; i < totalSize; i++) g_rxBufs[i] = 0;

    return true;
}

static void FillRxQueue()
{
    for (uint16_t i = 0; i < g_rxq.size; ++i) {
        // Compute per-buffer physical address (pages may not be physically contiguous)
        auto bufVirt = VirtualAddress(reinterpret_cast<uint64_t>(g_rxBufs + i * RX_BUF_SIZE));
        uint64_t bufPhys = VmmVirtToPhys(KernelPageTable, bufVirt).raw();
        g_rxq.descTable[i].addr  = bufPhys;
        g_rxq.descTable[i].len   = RX_BUF_SIZE;
        g_rxq.descTable[i].flags = VIRTQ_DESC_F_WRITE; // device writes here
        g_rxq.descTable[i].next  = 0;

        g_rxq.availRing[i] = i;
    }
    g_rxq.availIdxShadow = g_rxq.size;
    *g_rxq.availIdx = g_rxq.size;
    __asm__ volatile("mfence" ::: "memory");
}

static bool AllocTxBuffer()
{
    uint32_t pages = AlignUp(RX_BUF_SIZE, 4096) / 4096;
    auto vaddr = VmmAllocPages(pages, VMM_WRITABLE, MemTag::Device, KernelPid);
    if (!vaddr) return false;

    g_txBuf = reinterpret_cast<uint8_t*>(vaddr.raw());
    g_txBufPhys = VmmVirtToPhys(KernelPageTable, vaddr).raw();

    return true;
}

// ---------------------------------------------------------------------------
// Queue notification
// ---------------------------------------------------------------------------

static void NotifyQueue(uint16_t queueIdx, const VirtqState& q)
{
    volatile uint16_t* notifyAddr = reinterpret_cast<volatile uint16_t*>(
        g_notifyCfg + q.notifyOff * g_notifyMultiplier);
    *notifyAddr = queueIdx;
}

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static void ProcessRxPackets();

// ---------------------------------------------------------------------------
// Poll for received packets (for use during early boot when IRQs may not work)
// ---------------------------------------------------------------------------

static void VirtioNetPoll(NetIf* /*nif*/)
{
    ProcessRxPackets();
}

// ---------------------------------------------------------------------------
// Transmit
// ---------------------------------------------------------------------------

static int VirtioNetTransmit(NetIf* nif, const void* frame, uint32_t len)
{
    if (len + VIRTIO_NET_HDR_SIZE > RX_BUF_SIZE) return -1;

    static uint32_t s_txCount = 0;
    s_txCount++;
    if (s_txCount <= 3 || (s_txCount % 1000) == 0)
        SerialPrintf("virtio_net: TX %u bytes [pkt#%u]\n", len, s_txCount);

    // Prepare virtio-net header + frame in TX buffer
    auto* hdr = reinterpret_cast<VirtioNetHdr*>(g_txBuf);
    hdr->flags      = 0;
    hdr->gsoType    = 0; // VIRTIO_NET_HDR_GSO_NONE
    hdr->hdrLen     = 0;
    hdr->gsoSize    = 0;
    hdr->csumStart  = 0;
    hdr->csumOffset = 0;
    hdr->numBuffers = 0;

    // Copy frame data after header
    const uint8_t* src = static_cast<const uint8_t*>(frame);
    uint8_t* dst = g_txBuf + VIRTIO_NET_HDR_SIZE;
    for (uint32_t i = 0; i < len; i++) dst[i] = src[i];

    uint32_t totalLen = VIRTIO_NET_HDR_SIZE + len;

    // Use descriptor 0 for TX (single descriptor, no chaining needed)
    g_txq.descTable[0].addr  = g_txBufPhys;
    g_txq.descTable[0].len   = totalLen;
    g_txq.descTable[0].flags = 0; // device-readable
    g_txq.descTable[0].next  = 0;

    __asm__ volatile("mfence" ::: "memory");

    uint16_t slot = g_txq.availIdxShadow & (g_txq.size - 1);
    g_txq.availRing[slot] = 0;
    g_txq.availIdxShadow++;
    __asm__ volatile("sfence" ::: "memory");
    *g_txq.availIdx = g_txq.availIdxShadow;
    __asm__ volatile("mfence" ::: "memory");

    NotifyQueue(1, g_txq);

    // Wait for TX completion (poll used ring)
    uint32_t spins = 0;
    while (*g_txq.usedIdx == g_txq.usedIdxShadow) {
        __asm__ volatile("pause");
        if (++spins > 10000000u) {
            SerialPuts("virtio_net: TX timeout\n");
            return -1;
        }
    }
    __asm__ volatile("mfence" ::: "memory");
    g_txq.usedIdxShadow++;

    return 0;
}

// ---------------------------------------------------------------------------
// RX processing (called from IRQ handler AND from poll path)
//
// Critical bug fix: this function used to call NetReceive() (long-running TCP
// processing) BEFORE advancing usedIdxShadow. If a virtio-net IRQ fired while
// NetReceive was in progress — which is common, because TCP sends ACKs, which
// can drive RX interrupts — the nested ProcessRxPackets would see the same
// usedIdxShadow and replay every unprocessed used-ring slot from scratch.
// That manifested as the device apparently "retransmitting" old SYN|ACK and
// data packets thousands of times per second. It was entirely self-inflicted:
// we were re-reading our own used ring.
//
// Fix: take a ticket spinlock (which also disables local IRQs) around the
// short critical section that reads a slot and advances usedIdxShadow, then
// release before calling NetReceive. A re-entrant IRQ on the same CPU cannot
// preempt us while the lock is held; concurrent IRQs on other CPUs spin
// briefly. The actual packet processing (NetReceive) runs lock-free.
// ---------------------------------------------------------------------------

static SpinLock g_rxLock;

static void ProcessRxPackets()
{
    bool didWork = false;

    for (;;) {
        uint64_t flags = SpinLockAcquire(&g_rxLock);
        if (g_rxq.usedIdxShadow == *g_rxq.usedIdx) {
            SpinLockRelease(&g_rxLock, flags);
            break;
        }
        uint16_t slot    = g_rxq.usedIdxShadow & (g_rxq.size - 1);
        uint32_t descIdx = g_rxq.usedRing[slot].id;
        uint32_t totalLen = g_rxq.usedRing[slot].len;
        g_rxq.usedIdxShadow++;
        SpinLockRelease(&g_rxLock, flags);

        if (descIdx < g_rxq.size && totalLen > VIRTIO_NET_HDR_SIZE) {
            uint8_t* pkt = g_rxBufs + descIdx * RX_BUF_SIZE;
            uint32_t frameLen = totalLen - VIRTIO_NET_HDR_SIZE;
            uint8_t* frame = pkt + VIRTIO_NET_HDR_SIZE;

            // Deliver to network stack (may be long-running — no lock held)
            NetReceive(&g_netIf, frame, frameLen);
        }

        // Repost buffer to available ring. availIdxShadow is shared with
        // other invocations so must be advanced under the lock too.
        flags = SpinLockAcquire(&g_rxLock);
        uint16_t availSlot = g_rxq.availIdxShadow & (g_rxq.size - 1);
        g_rxq.availRing[availSlot] = static_cast<uint16_t>(descIdx);
        g_rxq.availIdxShadow++;
        __asm__ volatile("sfence" ::: "memory");
        *g_rxq.availIdx = g_rxq.availIdxShadow;
        didWork = true;
        SpinLockRelease(&g_rxLock, flags);
    }

    // Only notify QEMU when we've actually reposted buffers. Kicking with no
    // new available descriptors is wasteful (MMIO write = VM exit) and in
    // some virtio implementations triggers a spurious used-ring interrupt.
    if (didWork)
        NotifyQueue(0, g_rxq);
}

// ---------------------------------------------------------------------------
// IRQ handler (plain function — called by kernel's shared IRQ dispatch stub)
// ---------------------------------------------------------------------------

static void VirtioNetIrqBody()
{
    (void)mmio_read8(g_isrCfg, 0); // acknowledge ISR

    ProcessRxPackets();
}

// ---------------------------------------------------------------------------
// PCI capability parsing (same pattern as virtio_input)
// ---------------------------------------------------------------------------

struct VirtioPciCap {
    volatile uint8_t* mapped;
    uint32_t offset;
    uint32_t length;
    uint8_t  bar;
};

static bool FindVirtioCaps(const PciDevice& dev, VirtioPciCap caps[5])
{
    uint8_t capPtr = static_cast<uint8_t>(PciConfigRead8(dev.bus, dev.dev, dev.fn, 0x34));
    int found = 0;

    while (capPtr != 0 && capPtr != 0xFF) {
        uint8_t capId = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr);
        if (capId == 0x09) { // vendor-specific = virtio
            uint8_t cfgType = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr + 3);
            uint8_t bar     = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr + 4);
            uint32_t offset = PciConfigRead32(dev.bus, dev.dev, dev.fn, capPtr + 8);
            uint32_t length = PciConfigRead32(dev.bus, dev.dev, dev.fn, capPtr + 12);

            if (cfgType >= 1 && cfgType <= 4) {
                caps[cfgType].bar    = bar;
                caps[cfgType].offset = offset;
                caps[cfgType].length = length;
                found++;

                SerialPrintf("virtio_net: cap type %u bar %u off 0x%x len 0x%x\n",
                             cfgType, bar, offset, length);

                if (cfgType == VIRTIO_PCI_CAP_NOTIFY_CFG)
                    g_notifyMultiplier = PciConfigRead32(dev.bus, dev.dev, dev.fn, capPtr + 16);
            }
        }
        capPtr = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr + 1);
    }

    return found >= 4;
}

static volatile uint8_t* MapBar(const PciDevice& dev, uint8_t barIdx,
                                 uint32_t offset, uint32_t length)
{
    uint64_t barPhys = PciBarMemBase32(dev.bar[barIdx]);
    if (PciBarIs64(dev.bar[barIdx]) && barIdx + 1 < 6)
        barPhys |= static_cast<uint64_t>(dev.bar[barIdx + 1]) << 32;

    uint64_t regionPhys = barPhys + offset;
    uint32_t pages = AlignUp(length + (offset & 0xFFF), 4096) / 4096;
    if (pages == 0) pages = 1;

    auto vaddr = VmmAllocPages(pages, VMM_WRITABLE, MemTag::Device, KernelPid);
    if (!vaddr) return nullptr;

    uint64_t physBase = regionPhys & ~0xFFFULL;
    for (uint32_t i = 0; i < pages; i++) {
        VmmMapPage(KernelPageTable,
                   VirtualAddress(vaddr.raw() + i * 4096),
                   PhysicalAddress(physBase + i * 4096),
                   VMM_WRITABLE | VMM_NO_EXEC,
                   MemTag::Device, KernelPid);
    }

    return reinterpret_cast<volatile uint8_t*>(vaddr.raw() + (regionPhys & 0xFFF));
}

// ---------------------------------------------------------------------------
// Read MAC from device config (virtio-net §5.1.4)
// ---------------------------------------------------------------------------

static void ReadMac(MacAddr* mac)
{
    for (int i = 0; i < 6; i++)
        mac->b[i] = mmio_read8(g_deviceCfg, i);
}

// ---------------------------------------------------------------------------
// Setup a single virtqueue
// ---------------------------------------------------------------------------

static bool SetupQueue(uint16_t queueIdx, VirtqState& q)
{
    mmio_write16(g_commonCfg, VIRTIO_COMMON_Q_SELECT, queueIdx);

    uint16_t queueSize = mmio_read16(g_commonCfg, VIRTIO_COMMON_Q_SIZE);
    if (queueSize == 0) {
        SerialPrintf("virtio_net: queue %u size 0\n", queueIdx);
        return false;
    }
    if (queueSize > MAX_QUEUE_SIZE) {
        mmio_write16(g_commonCfg, VIRTIO_COMMON_Q_SIZE, MAX_QUEUE_SIZE);
        queueSize = MAX_QUEUE_SIZE;
    }
    // Round down to power of 2
    { uint16_t p = 1; while (p * 2 <= queueSize) p *= 2; queueSize = p; }

    SerialPrintf("virtio_net: queue %u size %u\n", queueIdx, queueSize);

    q.notifyOff = mmio_read16(g_commonCfg, VIRTIO_COMMON_Q_NOTIFY_OFF);

    if (!AllocQueue(q, queueSize)) {
        SerialPrintf("virtio_net: queue %u alloc failed\n", queueIdx);
        return false;
    }

    // Set queue addresses
    SerialPrintf("virtio_net: q%u desc=0x%lx avail=0x%lx used=0x%lx\n",
                 queueIdx, q.descPhys, q.availPhys, q.usedPhys);
    mmio_write64(g_commonCfg, VIRTIO_COMMON_Q_DESC, q.descPhys);
    mmio_write64(g_commonCfg, VIRTIO_COMMON_Q_AVAIL, q.availPhys);
    mmio_write64(g_commonCfg, VIRTIO_COMMON_Q_USED, q.usedPhys);

    // Enable queue
    mmio_write16(g_commonCfg, VIRTIO_COMMON_Q_ENABLE, 1);

    return true;
}

// ---------------------------------------------------------------------------
// Module init
// ---------------------------------------------------------------------------

static int VirtioNetModuleInit()
{
    SerialPuts("virtio_net: init\n");

    // Try modern device ID first (0x1041), then transitional (0x1000)
    PciDevice dev;
    bool found = PciFindDevice(0x1AF4, 0x1041, dev);
    if (!found) found = PciFindDevice(0x1AF4, 0x1000, dev);
    if (!found) {
        SerialPuts("virtio_net: no device found\n");
        return -1;
    }
    SerialPrintf("virtio_net: found at %02x:%02x.%x (device 0x%04x)\n",
                 dev.bus, dev.dev, dev.fn, dev.deviceId);

    PciEnableMemSpace(dev);
    PciEnableBusMaster(dev);

    // Parse PCI capabilities
    VirtioPciCap caps[5] = {};
    if (!FindVirtioCaps(dev, caps)) {
        SerialPuts("virtio_net: failed to find all virtio PCI caps\n");
        return -1;
    }

    // Map config regions
    g_commonCfg = MapBar(dev, caps[VIRTIO_PCI_CAP_COMMON_CFG].bar,
                         caps[VIRTIO_PCI_CAP_COMMON_CFG].offset,
                         caps[VIRTIO_PCI_CAP_COMMON_CFG].length);
    g_notifyCfg = MapBar(dev, caps[VIRTIO_PCI_CAP_NOTIFY_CFG].bar,
                         caps[VIRTIO_PCI_CAP_NOTIFY_CFG].offset,
                         caps[VIRTIO_PCI_CAP_NOTIFY_CFG].length);
    g_isrCfg    = MapBar(dev, caps[VIRTIO_PCI_CAP_ISR_CFG].bar,
                         caps[VIRTIO_PCI_CAP_ISR_CFG].offset,
                         caps[VIRTIO_PCI_CAP_ISR_CFG].length);
    g_deviceCfg = MapBar(dev, caps[VIRTIO_PCI_CAP_DEVICE_CFG].bar,
                         caps[VIRTIO_PCI_CAP_DEVICE_CFG].offset,
                         caps[VIRTIO_PCI_CAP_DEVICE_CFG].length);

    if (!g_commonCfg || !g_notifyCfg || !g_isrCfg || !g_deviceCfg) {
        SerialPuts("virtio_net: failed to map config regions\n");
        return -1;
    }

    // Reset device
    mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS, 0);
    mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    // Feature negotiation
    // Read device features (page 0)
    mmio_write32(g_commonCfg, VIRTIO_COMMON_DFSELECT, 0);
    uint32_t devFeatures0 = mmio_read32(g_commonCfg, VIRTIO_COMMON_DF);
    SerialPrintf("virtio_net: device features[0] = 0x%08x\n", devFeatures0);

    // We want: VIRTIO_NET_F_MAC (bit 5)
    // We do NOT want: MRG_RXBUF, CTRL_VQ, etc.
    uint32_t guestFeatures = devFeatures0 & VIRTIO_NET_F_MAC;
    mmio_write32(g_commonCfg, VIRTIO_COMMON_GFSELECT, 0);
    mmio_write32(g_commonCfg, VIRTIO_COMMON_GF, guestFeatures);

    // Page 1: virtio 1.0 requires VIRTIO_F_VERSION_1 (bit 32 = page 1 bit 0)
    mmio_write32(g_commonCfg, VIRTIO_COMMON_DFSELECT, 1);
    uint32_t devFeatures1 = mmio_read32(g_commonCfg, VIRTIO_COMMON_DF);
    SerialPrintf("virtio_net: device features[1] = 0x%08x\n", devFeatures1);

    uint32_t guestFeatures1 = devFeatures1 & 0x01; // VIRTIO_F_VERSION_1
    mmio_write32(g_commonCfg, VIRTIO_COMMON_GFSELECT, 1);
    mmio_write32(g_commonCfg, VIRTIO_COMMON_GF, guestFeatures1);

    // Set FEATURES_OK
    mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);

    uint8_t status = mmio_read8(g_commonCfg, VIRTIO_COMMON_STATUS);
    if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
        SerialPuts("virtio_net: FEATURES_OK not set by device\n");
        return -1;
    }

    // Read MAC address
    MacAddr mac;
    if (guestFeatures & VIRTIO_NET_F_MAC) {
        ReadMac(&mac);
        SerialPrintf("virtio_net: MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
                     mac.b[0], mac.b[1], mac.b[2],
                     mac.b[3], mac.b[4], mac.b[5]);
    } else {
        // Generate a random-ish MAC (locally administered)
        mac.b[0] = 0x52; mac.b[1] = 0x54; mac.b[2] = 0x00;
        mac.b[3] = 0x12; mac.b[4] = 0x34; mac.b[5] = 0x56;
    }

    // Setup queues
    if (!SetupQueue(0, g_rxq)) {
        SerialPuts("virtio_net: RX queue setup failed\n");
        return -1;
    }
    if (!SetupQueue(1, g_txq)) {
        SerialPuts("virtio_net: TX queue setup failed\n");
        return -1;
    }

    // Allocate RX buffers and fill queue
    if (!AllocRxBuffers()) {
        SerialPuts("virtio_net: RX buffer alloc failed\n");
        return -1;
    }
    FillRxQueue();

    // Allocate TX buffer
    if (!AllocTxBuffer()) {
        SerialPuts("virtio_net: TX buffer alloc failed\n");
        return -1;
    }

    // Set up IRQ
    uint32_t intLine = PciConfigRead32(dev.bus, dev.dev, dev.fn, 0x3C) & 0xFF;
    SerialPrintf("virtio_net: PCI interrupt line %u\n", intLine);

    IoApicRegisterHandler(static_cast<uint8_t>(intLine), VIRTIO_NET_IRQ_VECTOR,
                          reinterpret_cast<void*>(VirtioNetIrqBody));

    // Mark device as ready
    mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    // Notify RX queue
    NotifyQueue(0, g_rxq);

    // Register network interface
    g_netIf.mac = mac;
    g_netIf.ipAddr   = 0;
    g_netIf.netmask  = 0;
    g_netIf.gateway  = 0;
    g_netIf.dns      = 0;
    g_netIf.transmit = VirtioNetTransmit;
    g_netIf.poll     = VirtioNetPoll;
    g_netIf.driverPriv = nullptr;

    NetRegisterIf(&g_netIf);

    KPrintf("virtio_net: ready, MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
            mac.b[0], mac.b[1], mac.b[2],
            mac.b[3], mac.b[4], mac.b[5]);

    return 0;
}

static void VirtioNetModuleExit()
{
    SerialPuts("virtio_net: exit\n");
}

DECLARE_MODULE("virtio_net", VirtioNetModuleInit, VirtioNetModuleExit,
               "virtio-net network device driver");
