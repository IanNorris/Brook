// virtio_input_mod.cpp — virtio-input driver for QEMU virtio-tablet-pci.
//
// Provides absolute mouse positioning through the virtio-input protocol.
// Uses modern virtio PCI transport (PCI capability-based MMIO).
//
// The device reports EV_ABS events for X/Y with configurable ranges
// (typically 0–32767 for the QEMU tablet). These are scaled to the
// physical framebuffer dimensions.
//
// Virtio-input spec: virtio 1.0 §5.8
// QEMU device: virtio-tablet-pci (PCI 1af4:1052)

#include "module_abi.h"
#include "pci.h"
#include "serial.h"
#include "kprintf.h"
#include "mouse.h"
#include "input.h"
#include "compositor.h"
#include "idt.h"
#include "apic.h"
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
MODULE_IMPORT_SYMBOL(PciConfigWrite16);
MODULE_IMPORT_SYMBOL(SerialPrintf);
MODULE_IMPORT_SYMBOL(SerialPuts);
MODULE_IMPORT_SYMBOL(KPrintf);
MODULE_IMPORT_SYMBOL(MouseSetPosition);
MODULE_IMPORT_SYMBOL(MouseSetButtons);
MODULE_IMPORT_SYMBOL(MouseSetAvailable);
MODULE_IMPORT_SYMBOL(MouseSetBounds);
MODULE_IMPORT_SYMBOL(MouseGetButtons);
MODULE_IMPORT_SYMBOL(CompositorGetPhysDims);
MODULE_IMPORT_SYMBOL(CompositorWake);
MODULE_IMPORT_SYMBOL(InputRegister);
MODULE_IMPORT_SYMBOL(InputWakeWaiters);
MODULE_IMPORT_SYMBOL(VmmAllocPages);
MODULE_IMPORT_SYMBOL(VmmVirtToPhys);
MODULE_IMPORT_SYMBOL(VmmMapPage);
MODULE_IMPORT_SYMBOL(IoApicRegisterHandler);

using namespace brook;

// ---------------------------------------------------------------------------
// Modern virtio PCI capability types (virtio 1.0 §4.1.4)
// ---------------------------------------------------------------------------

static constexpr uint8_t VIRTIO_PCI_CAP_COMMON_CFG  = 1;
static constexpr uint8_t VIRTIO_PCI_CAP_NOTIFY_CFG  = 2;
static constexpr uint8_t VIRTIO_PCI_CAP_ISR_CFG     = 3;
static constexpr uint8_t VIRTIO_PCI_CAP_DEVICE_CFG  = 4;

// Common config layout offsets (virtio 1.0 §4.1.4.3)
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
    VIRTIO_COMMON_Q_MSIX_VEC   = 0x1A,
    VIRTIO_COMMON_Q_ENABLE      = 0x1C,
    VIRTIO_COMMON_Q_NOTIFY_OFF  = 0x1E,
    VIRTIO_COMMON_Q_DESC        = 0x20,
    VIRTIO_COMMON_Q_AVAIL       = 0x28,
    VIRTIO_COMMON_Q_USED        = 0x30,
};

// Device status bits
static constexpr uint8_t VIRTIO_STATUS_ACKNOWLEDGE = 1;
static constexpr uint8_t VIRTIO_STATUS_DRIVER      = 2;
static constexpr uint8_t VIRTIO_STATUS_FEATURES_OK = 8;
static constexpr uint8_t VIRTIO_STATUS_DRIVER_OK   = 4;

// Virtqueue descriptor flags
static constexpr uint16_t VIRTQ_DESC_F_WRITE = 2;

// ---------------------------------------------------------------------------
// Linux input event types/codes
// ---------------------------------------------------------------------------

static constexpr uint16_t EV_SYN = 0x00;
static constexpr uint16_t EV_KEY = 0x01;
static constexpr uint16_t EV_REL = 0x02;
static constexpr uint16_t EV_ABS = 0x03;
static constexpr uint16_t ABS_X       = 0x00;
static constexpr uint16_t ABS_Y       = 0x01;
static constexpr uint16_t REL_HWHEEL  = 0x06;
static constexpr uint16_t REL_WHEEL   = 0x08;
static constexpr uint16_t BTN_LEFT   = 0x110;
static constexpr uint16_t BTN_RIGHT  = 0x111;
static constexpr uint16_t BTN_MIDDLE = 0x112;

// virtio-input config select values
static constexpr uint8_t VIRTIO_INPUT_CFG_ID_NAME  = 0x01;
static constexpr uint8_t VIRTIO_INPUT_CFG_ABS_INFO = 0x05;

// ---------------------------------------------------------------------------
// Virtio-input event (matches struct virtio_input_event)
// ---------------------------------------------------------------------------

struct __attribute__((packed)) VirtioInputEvent {
    uint16_t type;
    uint16_t code;
    uint32_t value;
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

static constexpr uint32_t MAX_QUEUE_SIZE = 128;

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
// Driver state
// ---------------------------------------------------------------------------

static uint16_t g_queueSize = 0;

// Eventq (queue 0)
static VirtqDesc*          g_descTable = nullptr;
static uint16_t*           g_availFlags = nullptr;
static uint16_t*           g_availIdx = nullptr;
static uint16_t*           g_availRing = nullptr;
static volatile uint16_t*  g_usedIdx = nullptr;
static VirtqUsedElem*      g_usedRing = nullptr;
static uint16_t            g_availIdxShadow = 0;
static uint16_t            g_usedIdxShadow = 0;

static VirtioInputEvent*   g_eventBufs = nullptr;
static uint64_t            g_eventBufsPhys = 0;

static int32_t  g_absXMin = 0, g_absXMax = 32767;
static int32_t  g_absYMin = 0, g_absYMax = 32767;
static uint32_t g_screenW = 1920, g_screenH = 1080;
static int32_t  g_pendingX = -1, g_pendingY = -1;
static int32_t  g_lastAbsX = 0,  g_lastAbsY = 0;

static InputDevice g_inputDev;
static void VirtioInputPoll(InputDevice* dev);
static InputDeviceOps g_inputOps = { "virtio-tablet", nullptr };
static constexpr uint8_t VIRTIO_INPUT_IRQ_VECTOR = 46;

// Queue notify offset (from common config Q_NOTIFY_OFF)
static uint16_t g_queueNotifyOff = 0;

// ---------------------------------------------------------------------------
// Virtqueue allocation
// ---------------------------------------------------------------------------

static uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

static uint64_t g_descPhys = 0;
static uint64_t g_availPhys = 0;
static uint64_t g_usedPhys = 0;

static bool AllocEventQueue()
{
    uint32_t N = g_queueSize;
    uint32_t descSize  = 16 * N;
    uint32_t availSize = 6 + 2 * N;
    uint32_t usedSize  = 6 + 8 * N;

    // Each section must be page-aligned for modern virtio.
    uint32_t descPages  = AlignUp(descSize, 4096) / 4096;
    uint32_t availPages = AlignUp(availSize, 4096) / 4096;
    uint32_t usedPages  = AlignUp(usedSize, 4096) / 4096;
    uint32_t eventPages = AlignUp(N * sizeof(VirtioInputEvent), 4096) / 4096;
    uint32_t totalPages = descPages + availPages + usedPages + eventPages;

    auto qAddr = VmmAllocPages(totalPages, VMM_WRITABLE, MemTag::Device, KernelPid);
    if (!qAddr) return false;

    uint8_t* base = reinterpret_cast<uint8_t*>(qAddr.raw());
    for (uint32_t i = 0; i < totalPages * 4096; ++i) base[i] = 0;

    uint8_t* descBase  = base;
    uint8_t* availBase = base + descPages * 4096;
    uint8_t* usedBase  = availBase + availPages * 4096;
    uint8_t* eventBase = usedBase + usedPages * 4096;

    g_descTable  = reinterpret_cast<VirtqDesc*>(descBase);
    g_availFlags = reinterpret_cast<uint16_t*>(availBase);
    g_availIdx   = reinterpret_cast<uint16_t*>(availBase + 2);
    g_availRing  = reinterpret_cast<uint16_t*>(availBase + 4);
    g_usedIdx    = reinterpret_cast<volatile uint16_t*>(usedBase + 2);
    g_usedRing   = reinterpret_cast<VirtqUsedElem*>(usedBase + 4);

    g_descPhys  = VmmVirtToPhys(KernelPageTable, VirtualAddress(reinterpret_cast<uint64_t>(descBase))).raw();
    g_availPhys = VmmVirtToPhys(KernelPageTable, VirtualAddress(reinterpret_cast<uint64_t>(availBase))).raw();
    g_usedPhys  = VmmVirtToPhys(KernelPageTable, VirtualAddress(reinterpret_cast<uint64_t>(usedBase))).raw();

    g_eventBufs     = reinterpret_cast<VirtioInputEvent*>(eventBase);
    g_eventBufsPhys = VmmVirtToPhys(KernelPageTable, VirtualAddress(reinterpret_cast<uint64_t>(eventBase))).raw();

    return true;
}

static void FillEventQueue()
{
    for (uint16_t i = 0; i < g_queueSize; ++i)
    {
        g_descTable[i].addr  = g_eventBufsPhys + i * sizeof(VirtioInputEvent);
        g_descTable[i].len   = sizeof(VirtioInputEvent);
        g_descTable[i].flags = VIRTQ_DESC_F_WRITE;
        g_descTable[i].next  = 0;
        g_availRing[i] = i;
    }
    g_availIdxShadow = g_queueSize;
    *g_availIdx = g_queueSize;
    __asm__ volatile("mfence" ::: "memory");
}

static void NotifyQueue()
{
    // Modern: write queue index at notify offset.
    volatile uint16_t* notifyAddr = reinterpret_cast<volatile uint16_t*>(
        g_notifyCfg + g_queueNotifyOff * g_notifyMultiplier);
    *notifyAddr = 0; // queue 0
}

static void RepostDescriptor(uint16_t descIdx)
{
    uint16_t availSlot = g_availIdxShadow & (g_queueSize - 1);
    g_availRing[availSlot] = descIdx;
    g_availIdxShadow++;
    __asm__ volatile("sfence" ::: "memory");
    *g_availIdx = g_availIdxShadow;
    NotifyQueue();
}

// ---------------------------------------------------------------------------
// Event processing
// ---------------------------------------------------------------------------

static void ProcessEvent(const VirtioInputEvent& ev)
{
    if (ev.type == EV_ABS)
    {
        if (ev.code == ABS_X) g_pendingX = static_cast<int32_t>(ev.value);
        else if (ev.code == ABS_Y) g_pendingY = static_cast<int32_t>(ev.value);
    }
    else if (ev.type == EV_KEY)
    {
        // Some QEMU backends emit wheel events as EV_KEY with QEMU's
        // InputButton enum values (7=WHEEL_LEFT, 8=WHEEL_RIGHT) instead
        // of proper EV_REL. Translate those to scroll events on key-down.
        if ((ev.code == 7 || ev.code == 8) && ev.value)
        {
            InputEvent ie;
            ie.type = InputEventType::MouseScroll;
            ie.scanCode = 0;
            ie.ascii    = static_cast<uint8_t>(ev.code == 8 ? 1 : -1);
            ie.modifiers = MouseGetButtons();
            InputDevicePush(&g_inputDev, ie);
            InputWakeWaiters();
            return;
        }

        uint8_t btn = 0;
        uint8_t bit = 0;
        if      (ev.code == BTN_LEFT)   { btn = MOUSE_BTN_LEFT;   bit = 0; }
        else if (ev.code == BTN_RIGHT)  { btn = MOUSE_BTN_RIGHT;  bit = 1; }
        else if (ev.code == BTN_MIDDLE) { btn = MOUSE_BTN_MIDDLE; bit = 2; }

        if (btn)
        {
            uint8_t cur = MouseGetButtons();
            if (ev.value) cur |= btn;
            else          cur &= ~btn;
            MouseSetButtons(cur);

            InputEvent ie;
            ie.type = ev.value ? InputEventType::MouseButtonDown
                               : InputEventType::MouseButtonUp;
            ie.scanCode = bit; // bit index (0=L, 1=R, 2=M) — matches PS/2 path
            ie.ascii = 0;
            ie.modifiers = cur;
            InputDevicePush(&g_inputDev, ie);
            InputWakeWaiters();
        }
    }
    else if (ev.type == EV_REL)
    {
        int8_t dy = 0, dx = 0;
        if (ev.code == REL_WHEEL)       dy = static_cast<int8_t>(static_cast<int32_t>(ev.value));
        else if (ev.code == REL_HWHEEL) dx = static_cast<int8_t>(static_cast<int32_t>(ev.value));
        else return;

        InputEvent ie;
        ie.type = InputEventType::MouseScroll;
        ie.scanCode = static_cast<uint8_t>(dy);
        ie.ascii    = static_cast<uint8_t>(dx);
        ie.modifiers = MouseGetButtons();
        InputDevicePush(&g_inputDev, ie);
        InputWakeWaiters();
    }
    else if (ev.type == EV_SYN)
    {
        if (g_pendingX >= 0 || g_pendingY >= 0)
        {
            int32_t absX = (g_pendingX >= 0) ? g_pendingX : g_lastAbsX;
            int32_t absY = (g_pendingY >= 0) ? g_pendingY : g_lastAbsY;
            g_lastAbsX = absX;
            g_lastAbsY = absY;
            int32_t rangeX = g_absXMax - g_absXMin;
            int32_t rangeY = g_absYMax - g_absYMin;
            if (rangeX <= 0) rangeX = 1;
            if (rangeY <= 0) rangeY = 1;

            int32_t screenX = static_cast<int32_t>(
                static_cast<int64_t>(absX - g_absXMin) *
                static_cast<int64_t>(g_screenW - 1) / rangeX);
            int32_t screenY = static_cast<int32_t>(
                static_cast<int64_t>(absY - g_absYMin) *
                static_cast<int64_t>(g_screenH - 1) / rangeY);

            // Push a MouseMove event so userspace consumers (waylandd)
            // observe motion via the input ring, not just the cursor
            // global.  PS/2 mouse path does the same.  Only emit if the
            // mapped screen position actually changed, to avoid flooding
            // the ring on duplicate EV_SYN frames.
            static int32_t s_lastScreenX = -1, s_lastScreenY = -1;
            if (screenX != s_lastScreenX || screenY != s_lastScreenY)
            {
                InputEvent ie;
                ie.type      = InputEventType::MouseMove;
                ie.scanCode  = MouseGetButtons();
                ie.ascii     = 0;
                ie.modifiers = 0;
                InputDevicePush(&g_inputDev, ie);
                InputWakeWaiters();
                s_lastScreenX = screenX;
                s_lastScreenY = screenY;
            }

            MouseSetPosition(screenX, screenY);
            CompositorWake();
            g_pendingX = -1;
            g_pendingY = -1;
        }
    }
}

// ---------------------------------------------------------------------------
// IRQ handler (plain function — called by kernel's shared IRQ dispatch stub)
// ---------------------------------------------------------------------------

static void VirtioInputIrqBody()
{
    uint8_t isr = mmio_read8(g_isrCfg, 0);
    (void)isr;

    while (g_usedIdxShadow != *g_usedIdx)
    {
        uint16_t slot = g_usedIdxShadow & (g_queueSize - 1);
        uint32_t descIdx = g_usedRing[slot].id;
        if (descIdx < g_queueSize)
        {
            ProcessEvent(g_eventBufs[descIdx]);
            RepostDescriptor(static_cast<uint16_t>(descIdx));
        }
        g_usedIdxShadow++;
    }
}

// ---------------------------------------------------------------------------
// Poll callback — called by InputPollEvent() each compositor frame.
// Drains the virtqueue used ring without relying on IRQs.
// ---------------------------------------------------------------------------

static void VirtioInputPoll(InputDevice* /*dev*/)
{
    if (!g_usedIdx)
        return;

    while (g_usedIdxShadow != *g_usedIdx)
    {
        uint16_t slot = g_usedIdxShadow & (g_queueSize - 1);
        uint32_t descIdx = g_usedRing[slot].id;
        if (descIdx < g_queueSize)
        {
            ProcessEvent(g_eventBufs[descIdx]);
            RepostDescriptor(static_cast<uint16_t>(descIdx));
        }
        g_usedIdxShadow++;
    }
}

// ---------------------------------------------------------------------------
// PCI capability parsing — find modern virtio config regions
// ---------------------------------------------------------------------------

struct VirtioPciCap {
    volatile uint8_t* mapped;
    uint32_t offset;
    uint32_t length;
    uint8_t  bar;
};

static bool FindVirtioCaps(const PciDevice& dev, VirtioPciCap caps[5])
{
    // Walk PCI capability list.
    uint8_t capPtr = static_cast<uint8_t>(PciConfigRead8(dev.bus, dev.dev, dev.fn, 0x34));
    int found = 0;

    while (capPtr != 0 && capPtr != 0xFF)
    {
        uint8_t capId = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr);
        if (capId == 0x09) // Vendor-specific = virtio
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
                found++;

                SerialPrintf("virtio_input: cap type %u bar %u off 0x%x len 0x%x\n",
                             cfgType, bar, offset, length);

                // For notify cap, read the multiplier at capPtr + 16.
                if (cfgType == VIRTIO_PCI_CAP_NOTIFY_CFG)
                    g_notifyMultiplier = PciConfigRead32(dev.bus, dev.dev, dev.fn, capPtr + 16);
            }
        }
        capPtr = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr + 1);
    }

    return found >= 4;
}

// Disable MSI-X in PCI config space so the device uses legacy INTx interrupts.
// Without this, QEMU virtio devices may not deliver legacy interrupts.
static void DisableMsix(const PciDevice& dev)
{
    uint8_t capPtr = static_cast<uint8_t>(PciConfigRead8(dev.bus, dev.dev, dev.fn, 0x34));
    while (capPtr != 0 && capPtr != 0xFF)
    {
        uint8_t capId = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr);
        if (capId == 0x11) // MSI-X capability
        {
            uint16_t msgCtrl = PciConfigRead16(dev.bus, dev.dev, dev.fn, capPtr + 2);
            if (msgCtrl & 0x8000) // MSI-X enable bit
            {
                SerialPrintf("virtio_input: disabling MSI-X (msgCtrl=0x%x)\n", msgCtrl);
                PciConfigWrite16(dev.bus, dev.dev, dev.fn, capPtr + 2,
                                 msgCtrl & ~static_cast<uint16_t>(0x8000));
            }
            return;
        }
        capPtr = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr + 1);
    }
}

static volatile uint8_t* MapBar(const PciDevice& dev, uint8_t barIdx, uint32_t offset, uint32_t length)
{
    uint64_t barPhys = PciBarMemBase32(dev.bar[barIdx]);
    if (PciBarIs64(dev.bar[barIdx]) && barIdx + 1 < 6)
        barPhys |= static_cast<uint64_t>(dev.bar[barIdx + 1]) << 32;

    uint64_t regionPhys = barPhys + offset;
    uint32_t pages = AlignUp(length + (offset & 0xFFF), 4096) / 4096;
    if (pages == 0) pages = 1;

    auto vaddr = VmmAllocPages(pages, VMM_WRITABLE, MemTag::Device, KernelPid);
    if (!vaddr) return nullptr;

    // Remap each page to the BAR physical address.
    uint64_t physBase = regionPhys & ~0xFFFULL;
    for (uint32_t i = 0; i < pages; i++)
    {
        VmmMapPage(KernelPageTable,
                   VirtualAddress(vaddr.raw() + i * 4096),
                   PhysicalAddress(physBase + i * 4096),
                   VMM_WRITABLE | VMM_NO_EXEC | VMM_CACHE_DISABLE,
                   MemTag::Device, KernelPid);
    }

    return reinterpret_cast<volatile uint8_t*>(vaddr.raw() + (regionPhys & 0xFFF));
}

// ---------------------------------------------------------------------------
// Query device config
// ---------------------------------------------------------------------------

struct VirtioInputAbsInfo {
    uint32_t min, max, fuzz, flat, res;
};

static bool QueryAbsInfo(uint8_t axisCode, VirtioInputAbsInfo* out)
{
    mmio_write8(g_deviceCfg, 0, VIRTIO_INPUT_CFG_ABS_INFO); // select
    mmio_write8(g_deviceCfg, 1, axisCode);                   // subsel

    uint8_t sz = mmio_read8(g_deviceCfg, 2); // size
    if (sz < sizeof(VirtioInputAbsInfo)) return false;

    auto* p = reinterpret_cast<uint32_t*>(out);
    for (uint32_t i = 0; i < sizeof(VirtioInputAbsInfo) / 4; ++i)
        p[i] = mmio_read32(g_deviceCfg, 8 + i * 4); // data starts at offset 8

    return true;
}

static void QueryDeviceName(char* buf, uint32_t bufLen)
{
    mmio_write8(g_deviceCfg, 0, VIRTIO_INPUT_CFG_ID_NAME);
    mmio_write8(g_deviceCfg, 1, 0);

    uint8_t sz = mmio_read8(g_deviceCfg, 2);
    if (sz == 0 || sz > 128) { buf[0] = 0; return; }
    if (sz >= bufLen) sz = static_cast<uint8_t>(bufLen - 1);

    for (uint8_t i = 0; i < sz; ++i)
        buf[i] = static_cast<char>(mmio_read8(g_deviceCfg, 8 + i));
    buf[sz] = 0;
}

// ---------------------------------------------------------------------------
// Module init
// ---------------------------------------------------------------------------

static int VirtioInputModuleInit()
{
    SerialPuts("virtio_input: init\n");

    PciDevice dev;
    if (!PciFindDevice(0x1AF4, 0x1052, dev))
    {
        SerialPuts("virtio_input: device 1af4:1052 not found\n");
        return -1;
    }
    SerialPrintf("virtio_input: found at %02x:%02x.%x\n", dev.bus, dev.dev, dev.fn);

    PciEnableMemSpace(dev);
    PciEnableBusMaster(dev);
    DisableMsix(dev);

    // Parse PCI capabilities to find modern config regions.
    VirtioPciCap caps[5] = {};
    if (!FindVirtioCaps(dev, caps))
    {
        SerialPuts("virtio_input: failed to find all virtio PCI caps\n");
        return -1;
    }

    // Map the four config regions.
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
        SerialPuts("virtio_input: failed to map config regions\n");
        return -1;
    }

    // Reset device.
    mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS, 0);
    mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    // Read device name.
    char devName[64];
    QueryDeviceName(devName, sizeof(devName));
    SerialPrintf("virtio_input: device name: '%s'\n", devName);

    // Query ABS ranges.
    VirtioInputAbsInfo absX, absY;
    if (QueryAbsInfo(ABS_X, &absX))
    {
        g_absXMin = static_cast<int32_t>(absX.min);
        g_absXMax = static_cast<int32_t>(absX.max);
        SerialPrintf("virtio_input: ABS_X range [%d, %d]\n", g_absXMin, g_absXMax);
    }
    if (QueryAbsInfo(ABS_Y, &absY))
    {
        g_absYMin = static_cast<int32_t>(absY.min);
        g_absYMax = static_cast<int32_t>(absY.max);
        SerialPrintf("virtio_input: ABS_Y range [%d, %d]\n", g_absYMin, g_absYMax);
    }

    CompositorGetPhysDims(&g_screenW, &g_screenH);
    SerialPrintf("virtio_input: screen %ux%u\n", g_screenW, g_screenH);

    // Feature negotiation — page 0: no device-specific features needed.
    mmio_write32(g_commonCfg, VIRTIO_COMMON_DFSELECT, 0);
    (void)mmio_read32(g_commonCfg, VIRTIO_COMMON_DF);
    mmio_write32(g_commonCfg, VIRTIO_COMMON_GFSELECT, 0);
    mmio_write32(g_commonCfg, VIRTIO_COMMON_GF, 0);

    // Page 1: VIRTIO_F_VERSION_1 (bit 32 = page 1 bit 0) — required for modern devices.
    mmio_write32(g_commonCfg, VIRTIO_COMMON_DFSELECT, 1);
    uint32_t devFeatures1 = mmio_read32(g_commonCfg, VIRTIO_COMMON_DF);
    uint32_t guestFeatures1 = devFeatures1 & 0x01; // VIRTIO_F_VERSION_1
    mmio_write32(g_commonCfg, VIRTIO_COMMON_GFSELECT, 1);
    mmio_write32(g_commonCfg, VIRTIO_COMMON_GF, guestFeatures1);

    // Set FEATURES_OK.
    mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);

    uint8_t status = mmio_read8(g_commonCfg, VIRTIO_COMMON_STATUS);
    if (!(status & VIRTIO_STATUS_FEATURES_OK))
    {
        SerialPuts("virtio_input: FEATURES_OK not set by device\n");
        return -1;
    }

    // Select queue 0 (eventq).
    mmio_write16(g_commonCfg, VIRTIO_COMMON_Q_SELECT, 0);

    g_queueSize = mmio_read16(g_commonCfg, VIRTIO_COMMON_Q_SIZE);
    if (g_queueSize == 0)
    {
        SerialPuts("virtio_input: eventq queue size 0\n");
        return -1;
    }
    if (g_queueSize > MAX_QUEUE_SIZE)
    {
        mmio_write16(g_commonCfg, VIRTIO_COMMON_Q_SIZE, MAX_QUEUE_SIZE);
        g_queueSize = MAX_QUEUE_SIZE;
    }
    // Round down to power of 2.
    { uint16_t p = 1; while (p * 2 <= g_queueSize) p *= 2; g_queueSize = p; }

    SerialPrintf("virtio_input: eventq size %u\n", g_queueSize);

    g_queueNotifyOff = mmio_read16(g_commonCfg, VIRTIO_COMMON_Q_NOTIFY_OFF);

    if (!AllocEventQueue())
    {
        SerialPuts("virtio_input: eventq alloc failed\n");
        return -1;
    }

    // Set queue addresses (modern: 64-bit physical addresses).
    SerialPrintf("virtio_input: desc=0x%lx avail=0x%lx used=0x%lx events=0x%lx\n",
                 g_descPhys, g_availPhys, g_usedPhys, g_eventBufsPhys);
    mmio_write64(g_commonCfg, VIRTIO_COMMON_Q_DESC, g_descPhys);
    mmio_write64(g_commonCfg, VIRTIO_COMMON_Q_AVAIL, g_availPhys);
    mmio_write64(g_commonCfg, VIRTIO_COMMON_Q_USED, g_usedPhys);

    FillEventQueue();

    // Enable queue.
    mmio_write16(g_commonCfg, VIRTIO_COMMON_Q_ENABLE, 1);

    // Set up IRQ.
    uint32_t intLine = PciConfigRead32(dev.bus, dev.dev, dev.fn, 0x3C) & 0xFF;
    SerialPrintf("virtio_input: PCI interrupt line %u\n", intLine);

    IoApicRegisterHandler(static_cast<uint8_t>(intLine), VIRTIO_INPUT_IRQ_VECTOR,
                          reinterpret_cast<void*>(VirtioInputIrqBody));

    // Mark device as ready.
    mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    // Notify device that eventq has buffers.
    NotifyQueue();

    // Register input device.
    g_inputOps.poll = VirtioInputPoll;  // Set at runtime — static init relocations don't work in modules
    g_inputDev.ops  = &g_inputOps;
    g_inputDev.head = 0;
    g_inputDev.tail = 0;
    g_inputDev.priv = nullptr;
    InputRegister(&g_inputDev);

    MouseSetBounds(g_screenW, g_screenH);
    MouseSetAvailable(true);

    KPrintf("virtio_input: tablet ready (%ux%u)\n", g_screenW, g_screenH);
    return 0;
}

static void VirtioInputModuleExit()
{
    SerialPuts("virtio_input: exit\n");
}

DECLARE_MODULE("virtio_input", VirtioInputModuleInit, VirtioInputModuleExit,
               "virtio-input tablet driver (absolute positioning)");
