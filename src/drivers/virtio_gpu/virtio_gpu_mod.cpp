// virtio_gpu_mod.cpp — virtio-gpu 2D display driver for QEMU virtio-gpu-pci.
//
// Phase 3 (skeleton): modern virtio-1.0 PCI transport bring-up + controlq
// command submission, proven with GET_DISPLAY_INFO. Resource/scanout/flush
// (Phases 4-5) build on the SubmitCommand path established here.
//
// Modern virtio PCI plumbing mirrors virtio_input_mod.cpp (the correct
// template — virtio-gpu is virtio-1.0 only, no legacy I/O-port BAR).
// DisplayOps registration (Phase 5) will mirror bochs_display_mod.cpp.
//
// virtio-gpu spec: virtio 1.1 §5.7.  QEMU device: virtio-gpu-pci (1af4:1050).
// See VIRTIO_GPU_DOCS.md for the full design + command protocol.

#include "module_abi.h"
#include "pci.h"
#include "serial.h"
#include "kprintf.h"
#include "display.h"
#include "tty.h"
#include "compositor.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "memory/address.h"
#include "mem_tag.h"
#include "string.h"

MODULE_IMPORT_SYMBOL(PciFindDevice);
MODULE_IMPORT_SYMBOL(PciEnableMemSpace);
MODULE_IMPORT_SYMBOL(PciEnableBusMaster);
MODULE_IMPORT_SYMBOL(PciConfigRead32);
MODULE_IMPORT_SYMBOL(PciConfigRead16);
MODULE_IMPORT_SYMBOL(PciConfigRead8);
MODULE_IMPORT_SYMBOL(PciConfigWrite16);
MODULE_IMPORT_SYMBOL(SerialPrintf);
MODULE_IMPORT_SYMBOL(SerialPuts);
MODULE_IMPORT_SYMBOL(KPrintf);
MODULE_IMPORT_SYMBOL(VmmAllocPages);
MODULE_IMPORT_SYMBOL(VmmVirtToPhys);
MODULE_IMPORT_SYMBOL(VmmMapPage);
MODULE_IMPORT_SYMBOL(PmmAllocPages);
MODULE_IMPORT_SYMBOL(DisplayRegister);
MODULE_IMPORT_SYMBOL(TtyGetFramebuffer);
MODULE_IMPORT_SYMBOL(TtyRemap);
MODULE_IMPORT_SYMBOL(CompositorRemap);
MODULE_IMPORT_SYMBOL(PmmAllocPages);

using namespace brook;

// ---------------------------------------------------------------------------
// Modern virtio PCI capability types (virtio 1.0 §4.1.4)
// ---------------------------------------------------------------------------

static constexpr uint8_t VIRTIO_PCI_CAP_COMMON_CFG = 1;
static constexpr uint8_t VIRTIO_PCI_CAP_NOTIFY_CFG = 2;
static constexpr uint8_t VIRTIO_PCI_CAP_ISR_CFG    = 3;
static constexpr uint8_t VIRTIO_PCI_CAP_DEVICE_CFG = 4;

// Common config layout offsets (virtio 1.0 §4.1.4.3)
enum VirtioCommonReg : uint32_t {
    VIRTIO_COMMON_DFSELECT     = 0x00,
    VIRTIO_COMMON_DF           = 0x04,
    VIRTIO_COMMON_GFSELECT     = 0x08,
    VIRTIO_COMMON_GF           = 0x0C,
    VIRTIO_COMMON_NUM_QUEUES   = 0x12,
    VIRTIO_COMMON_STATUS       = 0x14,
    VIRTIO_COMMON_Q_SELECT     = 0x16,
    VIRTIO_COMMON_Q_SIZE       = 0x18,
    VIRTIO_COMMON_Q_ENABLE     = 0x1C,
    VIRTIO_COMMON_Q_NOTIFY_OFF = 0x1E,
    VIRTIO_COMMON_Q_DESC       = 0x20,
    VIRTIO_COMMON_Q_AVAIL      = 0x28,
    VIRTIO_COMMON_Q_USED       = 0x30,
};

static constexpr uint8_t VIRTIO_STATUS_ACKNOWLEDGE = 1;
static constexpr uint8_t VIRTIO_STATUS_DRIVER      = 2;
static constexpr uint8_t VIRTIO_STATUS_DRIVER_OK   = 4;
static constexpr uint8_t VIRTIO_STATUS_FEATURES_OK = 8;

static constexpr uint16_t VIRTQ_DESC_F_NEXT  = 1;
static constexpr uint16_t VIRTQ_DESC_F_WRITE = 2;

// ---------------------------------------------------------------------------
// virtio-gpu control protocol (virtio 1.1 §5.7.6) — see VIRTIO_GPU_DOCS.md
// ---------------------------------------------------------------------------

static constexpr uint32_t VIRTIO_GPU_CMD_GET_DISPLAY_INFO      = 0x0100;
static constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_CREATE_2D    = 0x0101;
static constexpr uint32_t VIRTIO_GPU_CMD_SET_SCANOUT           = 0x0103;
static constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_FLUSH        = 0x0104;
static constexpr uint32_t VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D   = 0x0105;
static constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106;
static constexpr uint32_t VIRTIO_GPU_RESP_OK_NODATA           = 0x1100;
static constexpr uint32_t VIRTIO_GPU_RESP_OK_DISPLAY_INFO     = 0x1101;
static constexpr uint32_t VIRTIO_GPU_MAX_SCANOUTS            = 16;

// Brook framebuffer is Bgr8 (memory bytes B,G,R,X) → B8G8R8X8_UNORM.
static constexpr uint32_t VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM   = 2;

// Single framebuffer resource id used for scanout 0.
static constexpr uint32_t RESOURCE_FB = 1;

struct __attribute__((packed)) VirtioGpuCtrlHdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t  ring_idx;
    uint8_t  padding[3];
};

struct __attribute__((packed)) VirtioGpuRect {
    uint32_t x, y, width, height;
};

struct __attribute__((packed)) VirtioGpuRespDisplayInfo {
    VirtioGpuCtrlHdr hdr;
    struct __attribute__((packed)) {
        VirtioGpuRect r;
        uint32_t      enabled;
        uint32_t      flags;
    } pmodes[VIRTIO_GPU_MAX_SCANOUTS];
};

struct __attribute__((packed)) VirtioGpuResourceCreate2D {
    VirtioGpuCtrlHdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
};

struct __attribute__((packed)) VirtioGpuMemEntry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
};

// Followed immediately by nr_entries × VirtioGpuMemEntry in the same buffer.
struct __attribute__((packed)) VirtioGpuResourceAttachBacking {
    VirtioGpuCtrlHdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
};

struct __attribute__((packed)) VirtioGpuSetScanout {
    VirtioGpuCtrlHdr hdr;
    VirtioGpuRect r;
    uint32_t scanout_id;
    uint32_t resource_id;
};

struct __attribute__((packed)) VirtioGpuTransferToHost2D {
    VirtioGpuCtrlHdr hdr;
    VirtioGpuRect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
};

struct __attribute__((packed)) VirtioGpuResourceFlush {
    VirtioGpuCtrlHdr hdr;
    VirtioGpuRect r;
    uint32_t resource_id;
    uint32_t padding;
};

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

static constexpr uint32_t MAX_QUEUE_SIZE = 64;

// ---------------------------------------------------------------------------
// MMIO accessors
// ---------------------------------------------------------------------------

static inline void mmio_write8(volatile uint8_t* b, uint32_t o, uint8_t v)
{ *reinterpret_cast<volatile uint8_t*>(b + o) = v; }
static inline void mmio_write16(volatile uint8_t* b, uint32_t o, uint16_t v)
{ *reinterpret_cast<volatile uint16_t*>(b + o) = v; }
static inline void mmio_write32(volatile uint8_t* b, uint32_t o, uint32_t v)
{ *reinterpret_cast<volatile uint32_t*>(b + o) = v; }
static inline void mmio_write64(volatile uint8_t* b, uint32_t o, uint64_t v)
{ *reinterpret_cast<volatile uint64_t*>(b + o) = v; }
static inline uint8_t mmio_read8(volatile uint8_t* b, uint32_t o)
{ return *reinterpret_cast<volatile uint8_t*>(b + o); }
static inline uint16_t mmio_read16(volatile uint8_t* b, uint32_t o)
{ return *reinterpret_cast<volatile uint16_t*>(b + o); }
static inline uint32_t mmio_read32(volatile uint8_t* b, uint32_t o)
{ return *reinterpret_cast<volatile uint32_t*>(b + o); }

// ---------------------------------------------------------------------------
// Driver state
// ---------------------------------------------------------------------------

static volatile uint8_t* g_commonCfg = nullptr;
static volatile uint8_t* g_notifyCfg = nullptr;
static volatile uint8_t* g_isrCfg    = nullptr;
static volatile uint8_t* g_deviceCfg = nullptr;
static uint32_t          g_notifyMultiplier = 0;
static uint16_t          g_queueNotifyOff = 0;

// controlq (queue 0)
static uint16_t            g_queueSize = 0;
static VirtqDesc*          g_descTable = nullptr;
static uint16_t*           g_availFlags = nullptr;
static uint16_t*           g_availIdx = nullptr;
static uint16_t*           g_availRing = nullptr;
static volatile uint16_t*  g_usedIdx = nullptr;
static VirtqUsedElem*      g_usedRing = nullptr;
static uint16_t            g_availIdxShadow = 0;
static uint16_t            g_usedIdxShadow = 0;

static uint64_t g_descPhys = 0, g_availPhys = 0, g_usedPhys = 0;

// Command request/response buffer — physically contiguous (PmmAllocPages) so a
// large RESOURCE_ATTACH_BACKING request (header + many mem-entries) is valid as
// a single descriptor. Request region at offset 0; response in the last page.
static uint8_t* g_cmdBuf = nullptr;
static uint64_t g_cmdBufPhys = 0;
static constexpr uint32_t CMD_PAGES    = 16;
static constexpr uint32_t CMD_REQ_OFF  = 0;
static constexpr uint32_t CMD_RESP_OFF = (CMD_PAGES - 1) * 4096;
static constexpr uint32_t CMD_RESP_CAP = 4096;

// Framebuffer resource backing — the front buffer the device scans out of AND
// the surface the compositor renders into. Physically contiguous (PmmAllocPages)
// so it doubles as the compositor's PhysToVirt-mapped framebuffer.
static uint8_t* g_fbBacking = nullptr;
static uint64_t g_fbBackingPhys = 0;
static uint32_t g_fbW = 0, g_fbH = 0;

// Display geometry from GET_DISPLAY_INFO (scanout 0).
static uint32_t g_dispWidth = 0, g_dispHeight = 0;

static uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// ---------------------------------------------------------------------------
// PCI capability parsing (virtio 1.0 §4.1.4) — mirror of virtio_input
// ---------------------------------------------------------------------------

struct VirtioPciCap {
    uint32_t offset;
    uint32_t length;
    uint8_t  bar;
};

static bool FindVirtioCaps(const PciDevice& dev, VirtioPciCap caps[5])
{
    uint8_t capPtr = static_cast<uint8_t>(PciConfigRead8(dev.bus, dev.dev, dev.fn, 0x34));
    int found = 0;
    while (capPtr != 0 && capPtr != 0xFF)
    {
        uint8_t capId = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr);
        if (capId == 0x09) // vendor-specific = virtio
        {
            uint8_t cfgType = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr + 3);
            uint8_t bar     = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr + 4);
            uint32_t offset = PciConfigRead32(dev.bus, dev.dev, dev.fn, capPtr + 8);
            uint32_t length = PciConfigRead32(dev.bus, dev.dev, dev.fn, capPtr + 12);
            if (cfgType >= 1 && cfgType <= 4)
            {
                caps[cfgType].bar    = bar;
                caps[cfgType].offset = offset;
                caps[cfgType].length = length;
                ++found;
                SerialPrintf("virtio_gpu: cap type %u bar %u off 0x%x len 0x%x\n",
                             cfgType, bar, offset, length);
                if (cfgType == VIRTIO_PCI_CAP_NOTIFY_CFG)
                    g_notifyMultiplier = PciConfigRead32(dev.bus, dev.dev, dev.fn, capPtr + 16);
            }
        }
        capPtr = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr + 1);
    }
    return found >= 4;
}

static void DisableMsix(const PciDevice& dev)
{
    uint8_t capPtr = static_cast<uint8_t>(PciConfigRead8(dev.bus, dev.dev, dev.fn, 0x34));
    while (capPtr != 0 && capPtr != 0xFF)
    {
        uint8_t capId = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr);
        if (capId == 0x11) // MSI-X
        {
            uint16_t msgCtrl = PciConfigRead16(dev.bus, dev.dev, dev.fn, capPtr + 2);
            if (msgCtrl & 0x8000)
                PciConfigWrite16(dev.bus, dev.dev, dev.fn, capPtr + 2,
                                 msgCtrl & ~static_cast<uint16_t>(0x8000));
            return;
        }
        capPtr = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr + 1);
    }
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
    for (uint32_t i = 0; i < pages; ++i)
        VmmMapPage(KernelPageTable,
                   VirtualAddress(vaddr.raw() + i * 4096),
                   PhysicalAddress(physBase + i * 4096),
                   VMM_WRITABLE | VMM_NO_EXEC | VMM_CACHE_DISABLE,
                   MemTag::Device, KernelPid);

    return reinterpret_cast<volatile uint8_t*>(vaddr.raw() + (regionPhys & 0xFFF));
}

// ---------------------------------------------------------------------------
// controlq allocation
// ---------------------------------------------------------------------------

static bool AllocControlQueue()
{
    uint32_t N = g_queueSize;
    uint32_t descPages  = AlignUp(16 * N, 4096) / 4096;
    uint32_t availPages = AlignUp(6 + 2 * N, 4096) / 4096;
    uint32_t usedPages  = AlignUp(6 + 8 * N, 4096) / 4096;
    uint32_t totalPages = descPages + availPages + usedPages;

    auto qAddr = VmmAllocPages(totalPages, VMM_WRITABLE, MemTag::Device, KernelPid);
    if (!qAddr) return false;

    uint8_t* base = reinterpret_cast<uint8_t*>(qAddr.raw());
    memset(base, 0, totalPages * 4096);

    uint8_t* descBase  = base;
    uint8_t* availBase = descBase + descPages * 4096;
    uint8_t* usedBase  = availBase + availPages * 4096;

    g_descTable  = reinterpret_cast<VirtqDesc*>(descBase);
    g_availFlags = reinterpret_cast<uint16_t*>(availBase);
    g_availIdx   = reinterpret_cast<uint16_t*>(availBase + 2);
    g_availRing  = reinterpret_cast<uint16_t*>(availBase + 4);
    g_usedIdx    = reinterpret_cast<volatile uint16_t*>(usedBase + 2);
    g_usedRing   = reinterpret_cast<VirtqUsedElem*>(usedBase + 4);

    g_descPhys   = VmmVirtToPhys(KernelPageTable, VirtualAddress(reinterpret_cast<uint64_t>(descBase))).raw();
    g_availPhys  = VmmVirtToPhys(KernelPageTable, VirtualAddress(reinterpret_cast<uint64_t>(availBase))).raw();
    g_usedPhys   = VmmVirtToPhys(KernelPageTable, VirtualAddress(reinterpret_cast<uint64_t>(usedBase))).raw();

    // Command buffer: physically contiguous so multi-page requests are valid.
    PhysicalAddress cmdPhys = PmmAllocPages(CMD_PAGES, MemTag::Device, KernelPid);
    if (!cmdPhys) return false;
    g_cmdBufPhys = cmdPhys.raw();
    g_cmdBuf     = reinterpret_cast<uint8_t*>(PhysToVirt(cmdPhys).raw());
    memset(g_cmdBuf, 0, CMD_PAGES * 4096);

    return true;
}

static void NotifyControlQueue()
{
    volatile uint16_t* notifyAddr = reinterpret_cast<volatile uint16_t*>(
        g_notifyCfg + g_queueNotifyOff * g_notifyMultiplier);
    *notifyAddr = 0; // queue index 0
}

// Submit one control command: request of reqLen bytes at g_cmdBuf+CMD_REQ_OFF,
// response written to g_cmdBuf+CMD_RESP_OFF. Returns the response length, or 0
// on timeout. Synchronous poll of the used ring (no IRQs) — the display path
// runs from the compositor thread, never an ISR.
static uint32_t SubmitCommand(uint32_t reqLen, uint32_t respCap)
{
    uint16_t head = static_cast<uint16_t>(g_availIdxShadow % g_queueSize);
    uint16_t d1   = static_cast<uint16_t>((head + 1) % g_queueSize);

    g_descTable[head].addr  = g_cmdBufPhys + CMD_REQ_OFF;
    g_descTable[head].len   = reqLen;
    g_descTable[head].flags = VIRTQ_DESC_F_NEXT;
    g_descTable[head].next  = d1;

    g_descTable[d1].addr  = g_cmdBufPhys + CMD_RESP_OFF;
    g_descTable[d1].len   = respCap;
    g_descTable[d1].flags = VIRTQ_DESC_F_WRITE;
    g_descTable[d1].next  = 0;

    __asm__ volatile("mfence" ::: "memory");
    g_availRing[g_availIdxShadow % g_queueSize] = head;
    __asm__ volatile("mfence" ::: "memory");
    *g_availIdx = ++g_availIdxShadow;
    __asm__ volatile("mfence" ::: "memory");

    NotifyControlQueue();

    // Bounded spin for completion.
    for (uint64_t i = 0; i < 200000000ull; ++i)
    {
        if (*g_usedIdx != g_usedIdxShadow)
        {
            __asm__ volatile("mfence" ::: "memory");
            uint16_t slot = g_usedIdxShadow % g_queueSize;
            uint32_t len  = g_usedRing[slot].len;
            ++g_usedIdxShadow;
            return len;
        }
        __asm__ volatile("pause" ::: "memory");
    }
    SerialPuts("virtio_gpu: command timeout\n");
    return 0;
}

// ---------------------------------------------------------------------------
// GET_DISPLAY_INFO
// ---------------------------------------------------------------------------

static bool QueryDisplayInfo()
{
    memset(g_cmdBuf, 0, 4096);
    auto* req = reinterpret_cast<VirtioGpuCtrlHdr*>(g_cmdBuf + CMD_REQ_OFF);
    req->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    uint32_t respLen = SubmitCommand(sizeof(VirtioGpuCtrlHdr),
                                     sizeof(VirtioGpuRespDisplayInfo));
    if (respLen < sizeof(VirtioGpuCtrlHdr))
    {
        SerialPrintf("virtio_gpu: GET_DISPLAY_INFO short response (%u)\n", respLen);
        return false;
    }

    auto* resp = reinterpret_cast<VirtioGpuRespDisplayInfo*>(g_cmdBuf + CMD_RESP_OFF);
    if (resp->hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO)
    {
        SerialPrintf("virtio_gpu: GET_DISPLAY_INFO bad resp type 0x%x\n", resp->hdr.type);
        return false;
    }

    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; ++i)
    {
        if (!resp->pmodes[i].enabled) continue;
        SerialPrintf("virtio_gpu: scanout %u enabled %ux%u\n",
                     i, resp->pmodes[i].r.width, resp->pmodes[i].r.height);
        if (g_dispWidth == 0)
        {
            g_dispWidth  = resp->pmodes[i].r.width;
            g_dispHeight = resp->pmodes[i].r.height;
        }
    }
    return g_dispWidth != 0;
}

// ---------------------------------------------------------------------------
// 2D resource / scanout commands. Each builds its request in g_cmdBuf and
// checks the device returned RESP_OK_NODATA.
// ---------------------------------------------------------------------------

static bool CmdRespOk(uint32_t respLen)
{
    if (respLen < sizeof(VirtioGpuCtrlHdr)) return false;
    auto* resp = reinterpret_cast<VirtioGpuCtrlHdr*>(g_cmdBuf + CMD_RESP_OFF);
    return resp->type == VIRTIO_GPU_RESP_OK_NODATA;
}

static bool ResourceCreate2D(uint32_t resId, uint32_t format, uint32_t w, uint32_t h)
{
    auto* req = reinterpret_cast<VirtioGpuResourceCreate2D*>(g_cmdBuf + CMD_REQ_OFF);
    memset(req, 0, sizeof(*req));
    req->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    req->resource_id = resId;
    req->format      = format;
    req->width       = w;
    req->height      = h;
    return CmdRespOk(SubmitCommand(sizeof(*req), CMD_RESP_CAP));
}

// Attach a physically-contiguous backing buffer as a single mem-entry. The
// framebuffer is allocated via PmmAllocPages (contiguous) so one entry suffices
// — and we must NOT walk it with VmmVirtToPhys, since the direct map is
// huge-page-mapped and the 4K-PTE walker returns 0 there.
static bool ResourceAttachBackingContig(uint32_t resId, uint64_t phys, uint32_t sizeBytes)
{
    auto* req = reinterpret_cast<VirtioGpuResourceAttachBacking*>(g_cmdBuf + CMD_REQ_OFF);
    auto* entries = reinterpret_cast<VirtioGpuMemEntry*>(
        g_cmdBuf + CMD_REQ_OFF + sizeof(VirtioGpuResourceAttachBacking));

    entries[0].addr    = phys;
    entries[0].length  = sizeBytes;
    entries[0].padding = 0;

    memset(&req->hdr, 0, sizeof(req->hdr));
    req->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    req->resource_id = resId;
    req->nr_entries  = 1;

    SerialPrintf("virtio_gpu: attach backing res %u — contiguous %u KB at 0x%lx\n",
                 resId, sizeBytes / 1024, phys);

    uint32_t reqLen = sizeof(VirtioGpuResourceAttachBacking) + sizeof(VirtioGpuMemEntry);
    return CmdRespOk(SubmitCommand(reqLen, CMD_RESP_CAP));
}

static bool SetScanout(uint32_t scanoutId, uint32_t resId, uint32_t w, uint32_t h)
{
    auto* req = reinterpret_cast<VirtioGpuSetScanout*>(g_cmdBuf + CMD_REQ_OFF);
    memset(req, 0, sizeof(*req));
    req->hdr.type     = VIRTIO_GPU_CMD_SET_SCANOUT;
    req->r.width      = w;
    req->r.height     = h;
    req->scanout_id   = scanoutId;
    req->resource_id  = resId;
    return CmdRespOk(SubmitCommand(sizeof(*req), CMD_RESP_CAP));
}

static bool TransferToHost2D(uint32_t resId, uint32_t x, uint32_t y,
                             uint32_t w, uint32_t h, uint64_t offsetBytes)
{
    auto* req = reinterpret_cast<VirtioGpuTransferToHost2D*>(g_cmdBuf + CMD_REQ_OFF);
    memset(req, 0, sizeof(*req));
    req->hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    req->r.x = x; req->r.y = y; req->r.width = w; req->r.height = h;
    req->offset      = offsetBytes;
    req->resource_id = resId;
    return CmdRespOk(SubmitCommand(sizeof(*req), CMD_RESP_CAP));
}

static bool ResourceFlush(uint32_t resId, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    auto* req = reinterpret_cast<VirtioGpuResourceFlush*>(g_cmdBuf + CMD_REQ_OFF);
    memset(req, 0, sizeof(*req));
    req->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    req->r.x = x; req->r.y = y; req->r.width = w; req->r.height = h;
    req->resource_id = resId;
    return CmdRespOk(SubmitCommand(sizeof(*req), CMD_RESP_CAP));
}

// ---------------------------------------------------------------------------
// DisplayOps integration (primary-display path)
// ---------------------------------------------------------------------------

// Push a dirty scanline span [minY, maxY) of the framebuffer to the device.
// Called from the compositor flip via DisplayFlush. The compositor renders into
// g_fbBacking; here we transfer just the damaged rows to the host resource and
// flush them to the scanout. Runs on the compositor thread (never an ISR), so
// the synchronous controlq poll in SubmitCommand is safe.
static void VirtioGpuFlush(uint32_t minY, uint32_t maxY)
{
    if (!g_fbBacking || maxY <= minY) return;
    if (maxY > g_fbH) maxY = g_fbH;
    uint32_t rows   = maxY - minY;
    uint64_t offset = static_cast<uint64_t>(minY) * g_fbW * 4;
    if (!TransferToHost2D(RESOURCE_FB, 0, minY, g_fbW, rows, offset)) return;
    ResourceFlush(RESOURCE_FB, 0, minY, g_fbW, rows);
}

static bool VgpuSetMode(uint32_t /*w*/, uint32_t /*h*/) { return false; } // Phase 6
static void VgpuGetMode(brook::DisplayMode* m)
{
    m->width = g_fbW; m->height = g_fbH; m->stride = g_fbW * 4; m->bpp = 32;
}
static volatile uint32_t* VgpuGetFramebuffer()
{ return reinterpret_cast<volatile uint32_t*>(g_fbBacking); }
static uint64_t VgpuGetFramebufferPhys() { return g_fbBackingPhys; }

static const brook::DisplayOps g_vgpuDisplayOps = {
    "virtio-gpu",
    VgpuSetMode,
    VgpuGetMode,
    VgpuGetFramebuffer,
    VgpuGetFramebufferPhys,
    VirtioGpuFlush,
};

// Take over the display: create a framebuffer resource backed by physically
// contiguous guest RAM, point both the TTY and compositor at that backing (so
// CompositorInit — which runs after module load — adopts it), set scanout, and
// register as the active DisplayOps. After this the compositor composites into
// g_fbBacking and each flip calls VirtioGpuFlush to present the damage rect.
static bool VirtioGpuTakeOverDisplay()
{
    // Adopt the current boot framebuffer resolution (GOP/VGA set by firmware).
    uint32_t* ttyPix; uint32_t w, h, strideBytes;
    if (!TtyGetFramebuffer(&ttyPix, &w, &h, &strideBytes) || w == 0 || h == 0)
    {
        SerialPuts("virtio_gpu: no TTY framebuffer to adopt\n");
        return false;
    }

    uint32_t bytes = w * h * 4;
    uint32_t pages = AlignUp(bytes, 4096) / 4096;

    // Physically contiguous: serves as the device backing AND the compositor's
    // PhysToVirt-mapped front buffer (CompositorRemap assumes contiguous phys).
    PhysicalAddress phys = PmmAllocPages(pages, MemTag::Device, KernelPid);
    if (!phys) { SerialPuts("virtio_gpu: fb backing alloc failed\n"); return false; }
    g_fbBackingPhys = phys.raw();
    g_fbBacking     = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
    g_fbW = w; g_fbH = h;
    memset(g_fbBacking, 0, bytes);

    if (!ResourceCreate2D(RESOURCE_FB, VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM, w, h))
    { SerialPuts("virtio_gpu: RESOURCE_CREATE_2D failed\n"); return false; }
    if (!ResourceAttachBackingContig(RESOURCE_FB, g_fbBackingPhys, bytes))
    { SerialPuts("virtio_gpu: ATTACH_BACKING failed\n"); return false; }
    if (!SetScanout(0, RESOURCE_FB, w, h))
    { SerialPuts("virtio_gpu: SET_SCANOUT failed\n"); return false; }

    // Redirect TTY + compositor into our backing, then register as the display.
    // stride == width (no row padding) since we allocated exactly w*h*4.
    TtyRemap(g_fbBackingPhys, w, h, w);
    CompositorRemap(g_fbBackingPhys, w, h, w);
    DisplayRegister(&g_vgpuDisplayOps);

    // Present the initial (cleared) frame.
    TransferToHost2D(RESOURCE_FB, 0, 0, w, h, 0);
    ResourceFlush(RESOURCE_FB, 0, 0, w, h);

    KPrintf("virtio_gpu: registered as primary display %ux%u\n", w, h);
    return true;
}


static int VirtioGpuModuleInit()
{
    SerialPuts("virtio_gpu: init\n");

    PciDevice dev;
    if (!PciFindDevice(0x1AF4, 0x1050, dev)) // virtio-gpu-pci (modern)
    {
        SerialPuts("virtio_gpu: device 1af4:1050 not found\n");
        return -1;
    }
    SerialPrintf("virtio_gpu: found at %02x:%02x.%x\n", dev.bus, dev.dev, dev.fn);

    PciEnableMemSpace(dev);
    PciEnableBusMaster(dev); // device DMAs command buffers + framebuffer backing
    DisableMsix(dev);

    VirtioPciCap caps[5] = {};
    if (!FindVirtioCaps(dev, caps))
    {
        SerialPuts("virtio_gpu: missing virtio PCI caps\n");
        return -1;
    }

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
    if (!g_commonCfg || !g_notifyCfg || !g_isrCfg || !g_deviceCfg)
    {
        SerialPuts("virtio_gpu: failed to map config regions\n");
        return -1;
    }

    // Reset, then ACK + DRIVER.
    mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS, 0);
    mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    // Feature negotiation: accept only VIRTIO_F_VERSION_1 (page 1, bit 0).
    mmio_write32(g_commonCfg, VIRTIO_COMMON_DFSELECT, 0);
    (void)mmio_read32(g_commonCfg, VIRTIO_COMMON_DF);
    mmio_write32(g_commonCfg, VIRTIO_COMMON_GFSELECT, 0);
    mmio_write32(g_commonCfg, VIRTIO_COMMON_GF, 0);
    mmio_write32(g_commonCfg, VIRTIO_COMMON_DFSELECT, 1);
    uint32_t devF1 = mmio_read32(g_commonCfg, VIRTIO_COMMON_DF);
    mmio_write32(g_commonCfg, VIRTIO_COMMON_GFSELECT, 1);
    mmio_write32(g_commonCfg, VIRTIO_COMMON_GF, devF1 & 0x01);

    mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
    if (!(mmio_read8(g_commonCfg, VIRTIO_COMMON_STATUS) & VIRTIO_STATUS_FEATURES_OK))
    {
        SerialPuts("virtio_gpu: FEATURES_OK rejected by device\n");
        return -1;
    }

    // Set up controlq (queue 0).
    mmio_write16(g_commonCfg, VIRTIO_COMMON_Q_SELECT, 0);
    g_queueSize = mmio_read16(g_commonCfg, VIRTIO_COMMON_Q_SIZE);
    if (g_queueSize == 0)
    {
        SerialPuts("virtio_gpu: controlq size 0\n");
        return -1;
    }
    if (g_queueSize > MAX_QUEUE_SIZE)
    {
        mmio_write16(g_commonCfg, VIRTIO_COMMON_Q_SIZE, MAX_QUEUE_SIZE);
        g_queueSize = MAX_QUEUE_SIZE;
    }
    { uint16_t p = 1; while (p * 2u <= g_queueSize) p *= 2; g_queueSize = p; }

    g_queueNotifyOff = mmio_read16(g_commonCfg, VIRTIO_COMMON_Q_NOTIFY_OFF);

    if (!AllocControlQueue())
    {
        SerialPuts("virtio_gpu: controlq alloc failed\n");
        return -1;
    }

    mmio_write64(g_commonCfg, VIRTIO_COMMON_Q_DESC,  g_descPhys);
    mmio_write64(g_commonCfg, VIRTIO_COMMON_Q_AVAIL, g_availPhys);
    mmio_write64(g_commonCfg, VIRTIO_COMMON_Q_USED,  g_usedPhys);
    mmio_write16(g_commonCfg, VIRTIO_COMMON_Q_ENABLE, 1);

    mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    SerialPrintf("virtio_gpu: controlq size %u, notifyOff %u, mult %u\n",
                 g_queueSize, g_queueNotifyOff, g_notifyMultiplier);

    if (!QueryDisplayInfo())
    {
        SerialPuts("virtio_gpu: GET_DISPLAY_INFO failed\n");
        return -1;
    }

    // Take over the display only when we are the PRIMARY device — i.e. a
    // VGA-class device (virtio-vga, PCI subclass 0x00) that provided the boot
    // GOP. A secondary virtio-gpu-pci head (display-other, subclass 0x80) is
    // left idle so the default stdvga+bochs boot is unaffected.
    uint8_t baseClass = PciConfigRead8(dev.bus, dev.dev, dev.fn, 0x0B);
    uint8_t subClass  = PciConfigRead8(dev.bus, dev.dev, dev.fn, 0x0A);
    bool isPrimaryVga = (baseClass == 0x03 && subClass == 0x00);

    if (isPrimaryVga)
    {
        if (!VirtioGpuTakeOverDisplay())
        {
            SerialPuts("virtio_gpu: display takeover failed\n");
            return -1;
        }
    }
    else
    {
        KPrintf("virtio_gpu: secondary head (class %02x:%02x) — not driving display\n",
                baseClass, subClass);
    }
    return 0;
}

static void VirtioGpuModuleExit()
{
    SerialPuts("virtio_gpu: exit\n");
}

DECLARE_MODULE("virtio_gpu", VirtioGpuModuleInit, VirtioGpuModuleExit,
               "virtio-gpu 2D display driver (PCI 1af4:1050)");
