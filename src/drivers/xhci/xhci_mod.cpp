// xhci_mod.cpp — xHCI USB 3.0 Host Controller driver for Brook OS.
//
// Implements the extensible Host Controller Interface (xHCI) spec for USB
// device enumeration and data transfer. Targets QEMU's qemu-xhci controller
// (Vendor 0x1B36, Device 0x000D) but should work with any xHCI-compliant HW.
//
// Implementation phases:
//   Phase 1: PCI detection, MMIO mapping, capability register parsing
//   Phase 2: Controller reset, DCBAA/command/event ring allocation, start
//   Phase 3: Port status change detection, port reset
//   Phase 4: Device enumeration (Enable Slot, Address Device)
//   Phase 5: Control transfers (GET_DESCRIPTOR, SET_CONFIGURATION)
//   Phase 6: HID drivers (keyboard, mouse)
//   Phase 7: Mass storage (bulk transfers)

#include "module_abi.h"
#include "pci.h"
#include "serial.h"
#include "kprintf.h"
#include "idt.h"
#include "apic.h"
#include "spinlock.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "memory/address.h"
#include "mem_tag.h"
#include "input.h"

MODULE_IMPORT_SYMBOL(PciFindDevice);
MODULE_IMPORT_SYMBOL(PciFindNextDevice);
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
MODULE_IMPORT_SYMBOL(IoApicRegisterHandler);
MODULE_IMPORT_SYMBOL(IoApicUnregisterHandler);
MODULE_IMPORT_SYMBOL(InputRegister);
MODULE_IMPORT_SYMBOL(InputWakeWaiters);
MODULE_IMPORT_SYMBOL(kmalloc);
MODULE_IMPORT_SYMBOL(kfree);

using namespace brook;

namespace brook { extern volatile uint64_t g_lapicTickCount; }

// Suppress unused-const-variable for forward-declared constants needed in later phases
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"

// ---------------------------------------------------------------------------
// xHCI PCI class/subclass/progif
// ---------------------------------------------------------------------------

static constexpr uint8_t  PCI_CLASS_SERIAL_BUS = 0x0C;
static constexpr uint8_t  PCI_SUBCLASS_USB     = 0x03;
static constexpr uint8_t  PCI_PROGIF_XHCI      = 0x30;

// QEMU qemu-xhci identifiers
static constexpr uint16_t XHCI_VENDOR_QEMU     = 0x1B36;
static constexpr uint16_t XHCI_DEVICE_QEMU     = 0x000D;

// ---------------------------------------------------------------------------
// xHCI Register Offsets (Capability Registers at BAR0)
// ---------------------------------------------------------------------------

// Capability registers (offset from BAR0)
static constexpr uint32_t XHCI_CAP_CAPLENGTH   = 0x00; // uint8: cap reg length
static constexpr uint32_t XHCI_CAP_HCIVERSION  = 0x02; // uint16: HCI version
static constexpr uint32_t XHCI_CAP_HCSPARAMS1  = 0x04; // uint32
static constexpr uint32_t XHCI_CAP_HCSPARAMS2  = 0x08; // uint32
static constexpr uint32_t XHCI_CAP_HCSPARAMS3  = 0x0C; // uint32
static constexpr uint32_t XHCI_CAP_HCCPARAMS1  = 0x10; // uint32
static constexpr uint32_t XHCI_CAP_DBOFF       = 0x14; // uint32: doorbell offset
static constexpr uint32_t XHCI_CAP_RTSOFF      = 0x18; // uint32: runtime reg offset

// Operational registers (offset from BAR0 + capLength)
static constexpr uint32_t XHCI_OP_USBCMD       = 0x00;
static constexpr uint32_t XHCI_OP_USBSTS       = 0x04;
static constexpr uint32_t XHCI_OP_PAGESIZE      = 0x08;
static constexpr uint32_t XHCI_OP_DNCTRL       = 0x14;
static constexpr uint32_t XHCI_OP_CRCR         = 0x18; // uint64
static constexpr uint32_t XHCI_OP_DCBAAP       = 0x30; // uint64
static constexpr uint32_t XHCI_OP_CONFIG       = 0x38;
static constexpr uint32_t XHCI_OP_PORTSC_BASE  = 0x400; // port 0; each port +0x10

// USBCMD bits
static constexpr uint32_t USBCMD_RS   = (1 << 0); // Run/Stop
static constexpr uint32_t USBCMD_HCRST = (1 << 1); // Host Controller Reset
static constexpr uint32_t USBCMD_INTE = (1 << 2); // Interrupter Enable
static constexpr uint32_t USBCMD_HSEE = (1 << 3); // Host System Error Enable

// USBSTS bits
static constexpr uint32_t USBSTS_HCH  = (1 << 0); // HC Halted
static constexpr uint32_t USBSTS_HSE  = (1 << 2); // Host System Error
static constexpr uint32_t USBSTS_EINT = (1 << 3); // Event Interrupt
static constexpr uint32_t USBSTS_PCD  = (1 << 4); // Port Change Detect
static constexpr uint32_t USBSTS_CNR  = (1 << 11); // Controller Not Ready

// PORTSC bits
static constexpr uint32_t PORTSC_CCS  = (1 << 0);  // Current Connect Status
static constexpr uint32_t PORTSC_PED  = (1 << 1);  // Port Enabled/Disabled
static constexpr uint32_t PORTSC_PR   = (1 << 4);  // Port Reset
static constexpr uint32_t PORTSC_PLS_MASK = (0xF << 5); // Port Link State
static constexpr uint32_t PORTSC_PP   = (1 << 9);  // Port Power
static constexpr uint32_t PORTSC_SPEED_MASK = (0xF << 10); // Port Speed
static constexpr uint32_t PORTSC_CSC  = (1 << 17); // Connect Status Change (W1C)
static constexpr uint32_t PORTSC_PEC  = (1 << 18); // Port Enabled Change (W1C)
static constexpr uint32_t PORTSC_PRC  = (1 << 21); // Port Reset Change (W1C)
// Bits that are W1C — must preserve when writing other fields
static constexpr uint32_t PORTSC_W1C_MASK = PORTSC_CSC | PORTSC_PEC | PORTSC_PRC
                                          | (1 << 19) | (1 << 20) | (1 << 22) | (1 << 23);

// Port speed values (PORTSC bits 13:10)
static constexpr uint32_t PORT_SPEED_FULL  = 1;
static constexpr uint32_t PORT_SPEED_LOW   = 2;
static constexpr uint32_t PORT_SPEED_HIGH  = 3;
static constexpr uint32_t PORT_SPEED_SUPER = 4;

// Runtime register offsets (from runtimeBase)
static constexpr uint32_t XHCI_RT_IMAN     = 0x20; // Interrupter 0 Management
static constexpr uint32_t XHCI_RT_IMOD     = 0x24; // Interrupter 0 Moderation
static constexpr uint32_t XHCI_RT_ERSTSZ   = 0x28; // Event Ring Segment Table Size
static constexpr uint32_t XHCI_RT_ERSTBA   = 0x30; // Event Ring Segment Table Base (uint64)
static constexpr uint32_t XHCI_RT_ERDP     = 0x38; // Event Ring Dequeue Pointer (uint64)

// HCCPARAMS1 bits
static constexpr uint32_t HCC_CSZ = (1 << 2); // Context Size (0=32B, 1=64B)
static constexpr uint32_t HCC_AC64 = (1 << 0); // 64-bit addressing

// ---------------------------------------------------------------------------
// TRB (Transfer Request Block) — 16 bytes each
// ---------------------------------------------------------------------------

struct alignas(16) Trb {
    uint64_t param;     // parameter (address, data, etc.)
    uint32_t status;    // transfer length, completion code, etc.
    uint32_t control;   // TRB type, cycle bit, flags
};
static_assert(sizeof(Trb) == 16, "TRB must be 16 bytes");

// TRB types (bits 15:10 of control field)
static constexpr uint32_t TRB_TYPE_SHIFT      = 10;
static constexpr uint32_t TRB_TYPE_MASK       = (0x3F << TRB_TYPE_SHIFT);

// Transfer TRB types
static constexpr uint32_t TRB_TYPE_NORMAL     = (1 << TRB_TYPE_SHIFT);
static constexpr uint32_t TRB_TYPE_SETUP      = (2 << TRB_TYPE_SHIFT);
static constexpr uint32_t TRB_TYPE_DATA       = (3 << TRB_TYPE_SHIFT);
static constexpr uint32_t TRB_TYPE_STATUS     = (4 << TRB_TYPE_SHIFT);
static constexpr uint32_t TRB_TYPE_LINK       = (6 << TRB_TYPE_SHIFT);

// Command TRB types
static constexpr uint32_t TRB_TYPE_ENABLE_SLOT = (9 << TRB_TYPE_SHIFT);
static constexpr uint32_t TRB_TYPE_DISABLE_SLOT = (10 << TRB_TYPE_SHIFT);
static constexpr uint32_t TRB_TYPE_ADDRESS_DEV = (11 << TRB_TYPE_SHIFT);
static constexpr uint32_t TRB_TYPE_CONFIG_EP   = (12 << TRB_TYPE_SHIFT);
static constexpr uint32_t TRB_TYPE_EVAL_CTX    = (13 << TRB_TYPE_SHIFT);
static constexpr uint32_t TRB_TYPE_RESET_EP    = (14 << TRB_TYPE_SHIFT);
static constexpr uint32_t TRB_TYPE_NOOP_CMD    = (23 << TRB_TYPE_SHIFT);

// Event TRB types
static constexpr uint32_t TRB_TYPE_TRANSFER_EVENT = (32 << TRB_TYPE_SHIFT);
static constexpr uint32_t TRB_TYPE_CMD_COMPLETION = (33 << TRB_TYPE_SHIFT);
static constexpr uint32_t TRB_TYPE_PORT_STATUS_CHANGE = (34 << TRB_TYPE_SHIFT);

// TRB control flags
static constexpr uint32_t TRB_CYCLE    = (1 << 0);
static constexpr uint32_t TRB_TC       = (1 << 1); // Toggle Cycle (for Link TRBs)
static constexpr uint32_t TRB_ISP      = (1 << 2); // Interrupt on Short Packet
static constexpr uint32_t TRB_IOC      = (1 << 5); // Interrupt on Completion
static constexpr uint32_t TRB_IDT      = (1 << 6); // Immediate Data
static constexpr uint32_t TRB_BSR      = (1 << 9); // Block Set Address Request

// TRB completion codes (status field bits 31:24)
static constexpr uint32_t TRB_CC_SHIFT = 24;
static constexpr uint32_t TRB_CC_SUCCESS = 1;
static constexpr uint32_t TRB_CC_SHORT_PACKET = 13;

// ---------------------------------------------------------------------------
// Event Ring Segment Table Entry
// ---------------------------------------------------------------------------

struct alignas(16) ErstEntry {
    uint64_t ringSegBase;  // physical address of event ring segment
    uint32_t ringSegSize;  // number of TRBs in segment
    uint32_t reserved;
};
static_assert(sizeof(ErstEntry) == 16, "ERST entry must be 16 bytes");

// ---------------------------------------------------------------------------
// USB standard descriptors
// ---------------------------------------------------------------------------

struct UsbDeviceDescriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed));

struct UsbConfigDescriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed));

struct UsbInterfaceDescriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed));

struct UsbEndpointDescriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed));

// USB descriptor types
static constexpr uint8_t USB_DESC_DEVICE    = 1;
static constexpr uint8_t USB_DESC_CONFIG    = 2;
static constexpr uint8_t USB_DESC_INTERFACE = 4;
static constexpr uint8_t USB_DESC_ENDPOINT  = 5;

// USB device classes
static constexpr uint8_t USB_CLASS_HID      = 3;
static constexpr uint8_t USB_CLASS_MASS_STORAGE = 8;

// HID subclasses
static constexpr uint8_t USB_HID_SUBCLASS_BOOT = 1;
static constexpr uint8_t USB_HID_PROTOCOL_KEYBOARD = 1;
static constexpr uint8_t USB_HID_PROTOCOL_MOUSE = 2;

// USB setup packet
struct UsbSetupPacket {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));
static_assert(sizeof(UsbSetupPacket) == 8, "USB setup packet must be 8 bytes");

// Standard USB requests
static constexpr uint8_t USB_REQ_GET_DESCRIPTOR  = 6;
static constexpr uint8_t USB_REQ_SET_CONFIG       = 9;
static constexpr uint8_t USB_REQ_SET_PROTOCOL     = 11;

// ---------------------------------------------------------------------------
// Ring size constants
// ---------------------------------------------------------------------------

static constexpr uint32_t CMD_RING_SIZE   = 256;  // TRBs in command ring
static constexpr uint32_t EVT_RING_SIZE   = 256;  // TRBs in event ring
static constexpr uint32_t XFER_RING_SIZE  = 256;  // TRBs per transfer ring
static constexpr uint32_t MAX_DEVICES     = 16;   // max USB devices tracked
static constexpr uint32_t MAX_CONTROLLERS = 2;    // max xHCI controllers

// ---------------------------------------------------------------------------
// MMIO helpers
// ---------------------------------------------------------------------------

#pragma clang diagnostic ignored "-Wunused-function"

static inline uint8_t  xhci_read8(volatile uint8_t* base, uint32_t off)
    { return *reinterpret_cast<volatile uint8_t*>(base + off); }
static inline uint16_t xhci_read16(volatile uint8_t* base, uint32_t off)
    { return *reinterpret_cast<volatile uint16_t*>(base + off); }
static inline uint32_t xhci_read32(volatile uint8_t* base, uint32_t off)
    { return *reinterpret_cast<volatile uint32_t*>(base + off); }
static inline uint64_t xhci_read64(volatile uint8_t* base, uint32_t off)
    { return *reinterpret_cast<volatile uint64_t*>(base + off); }

static inline void xhci_write32(volatile uint8_t* base, uint32_t off, uint32_t val)
    { *reinterpret_cast<volatile uint32_t*>(base + off) = val; }
static inline void xhci_write64(volatile uint8_t* base, uint32_t off, uint64_t val)
    { *reinterpret_cast<volatile uint64_t*>(base + off) = val; }

// ---------------------------------------------------------------------------
// Per-controller state
// ---------------------------------------------------------------------------

struct XhciController {
    PciDevice    pciDev;
    volatile uint8_t* capBase;     // capability registers
    volatile uint8_t* opBase;      // operational registers (capBase + capLength)
    volatile uint8_t* rtBase;      // runtime registers
    volatile uint8_t* dbBase;      // doorbell registers

    // Capability parameters
    uint8_t  capLength;
    uint16_t hciVersion;
    uint32_t maxSlots;
    uint32_t maxPorts;
    uint32_t maxIntrs;
    bool     ctx64;         // true if device contexts are 64 bytes (vs 32)
    uint32_t pageSize;      // xHCI page size in bytes
    uint32_t scratchpadCount;

    // Command ring
    Trb*     cmdRing;       // virtual address of command ring
    uint64_t cmdRingPhys;   // physical address
    uint32_t cmdEnqueue;    // next slot to write
    bool     cmdCycle;      // current producer cycle bit

    // Event ring
    Trb*     evtRing;       // virtual address of event ring
    uint64_t evtRingPhys;
    uint32_t evtDequeue;    // consumer dequeue index
    bool     evtCycle;      // expected consumer cycle bit
    ErstEntry* erst;        // event ring segment table
    uint64_t erstPhys;

    // DCBAA
    uint64_t* dcbaa;        // Device Context Base Address Array (virtual)
    uint64_t  dcbaaPhys;    // physical

    // Scratchpad
    uint64_t* scratchpadArray;   // array of physical addresses
    uint64_t  scratchpadArrayPhys;

    // IRQ
    uint8_t  irqLine;
    uint8_t  irqVector;

    bool     initialized;
};

static XhciController g_controllers[MAX_CONTROLLERS];
static uint32_t       g_controllerCount = 0;

// ---------------------------------------------------------------------------
// Utility: allocate physically contiguous, page-aligned DMA buffer
// ---------------------------------------------------------------------------

// Allocates 'pages' physically contiguous pages, returns both virtual and
// physical addresses. Memory is zeroed. Returns nullptr on failure.
static void* AllocDmaBuffer(uint32_t pages, uint64_t& outPhys)
{
    auto phys = PmmAllocPages(pages);
    if (!phys) {
        SerialPrintf("xhci: DMA alloc failed (%u pages)\n", pages);
        return nullptr;
    }
    outPhys = phys.raw();

    auto virt = VmmAllocPages(pages, VMM_WRITABLE | VMM_NO_EXEC,
                              MemTag::Device, KernelPid);
    if (!virt) {
        SerialPrintf("xhci: VmmAllocPages failed (%u pages)\n", pages);
        return nullptr;
    }

    for (uint32_t i = 0; i < pages; i++) {
        VmmMapPage(KernelPageTable,
                   VirtualAddress(virt.raw() + i * 4096),
                   PhysicalAddress(outPhys + i * 4096),
                   VMM_WRITABLE | VMM_NO_EXEC,
                   MemTag::Device, KernelPid);
    }

    // Zero the buffer
    auto* ptr = reinterpret_cast<uint8_t*>(virt.raw());
    for (uint32_t i = 0; i < pages * 4096; i++)
        ptr[i] = 0;

    return ptr;
}

// ---------------------------------------------------------------------------
// Phase 1: PCI detection and MMIO mapping
// ---------------------------------------------------------------------------

static volatile uint8_t* MapBar64(const PciDevice& dev, uint32_t barIdx,
                                   uint32_t length)
{
    uint64_t barPhys = PciBarMemBase32(dev.bar[barIdx]);
    if (PciBarIs64(dev.bar[barIdx]) && barIdx + 1 < 6)
        barPhys |= static_cast<uint64_t>(dev.bar[barIdx + 1]) << 32;

    if (barPhys == 0) {
        SerialPrintf("xhci: BAR%u is zero\n", barIdx);
        return nullptr;
    }

    uint32_t pages = (length + 4095) / 4096;
    if (pages == 0) pages = 1;

    auto vaddr = VmmAllocPages(pages, VMM_WRITABLE | VMM_NO_EXEC,
                               MemTag::Device, KernelPid);
    if (!vaddr) {
        SerialPrintf("xhci: VmmAllocPages for BAR failed\n");
        return nullptr;
    }

    for (uint32_t i = 0; i < pages; i++) {
        VmmMapPage(KernelPageTable,
                   VirtualAddress(vaddr.raw() + i * 4096),
                   PhysicalAddress(barPhys + i * 4096),
                   VMM_WRITABLE | VMM_NO_EXEC | VMM_CACHE_DISABLE,
                   MemTag::Device, KernelPid);
    }

    return reinterpret_cast<volatile uint8_t*>(vaddr.raw());
}

static bool XhciPciDetect(XhciController& ctrl)
{
    // First try QEMU's specific vendor/device ID
    PciDevice dev;
    bool found = PciFindDevice(XHCI_VENDOR_QEMU, XHCI_DEVICE_QEMU, dev);

    if (!found) {
        // Scan all PCI devices for xHCI class code
        for (uint16_t bus = 0; bus < 256 && !found; bus++) {
            for (uint8_t d = 0; d < 32 && !found; d++) {
                for (uint8_t f = 0; f < 8 && !found; f++) {
                    uint32_t reg0 = PciConfigRead32(bus, d, f, 0);
                    if ((reg0 & 0xFFFF) == 0xFFFF) continue;
                    uint32_t reg8 = PciConfigRead32(bus, d, f, 8);
                    uint8_t cls = (reg8 >> 24) & 0xFF;
                    uint8_t sub = (reg8 >> 16) & 0xFF;
                    uint8_t pi  = (reg8 >> 8) & 0xFF;
                    if (cls == PCI_CLASS_SERIAL_BUS &&
                        sub == PCI_SUBCLASS_USB &&
                        pi  == PCI_PROGIF_XHCI) {
                        dev.bus = bus;
                        dev.dev = d;
                        dev.fn = f;
                        dev.vendorId = reg0 & 0xFFFF;
                        dev.deviceId = (reg0 >> 16) & 0xFFFF;
                        dev.classCode = cls;
                        dev.subclass = sub;
                        dev.progIf = pi;
                        for (int i = 0; i < 6; i++)
                            dev.bar[i] = PciConfigRead32(bus, d, f, 0x10 + i * 4);
                        found = true;
                    }
                }
            }
        }
    }

    if (!found) {
        SerialPuts("xhci: no xHCI controller found on PCI bus\n");
        return false;
    }

    ctrl.pciDev = dev;
    SerialPrintf("xhci: found controller %04x:%04x at PCI %u:%u.%u\n",
                 dev.vendorId, dev.deviceId, dev.bus, dev.dev, dev.fn);

    // Enable PCI bus master and memory space
    PciEnableBusMaster(dev);
    PciEnableMemSpace(dev);

    // Map BAR0 (64-bit MMIO) — map a generous region to cover all register spaces
    ctrl.capBase = MapBar64(dev, 0, 64 * 1024);
    if (!ctrl.capBase) {
        SerialPuts("xhci: failed to map BAR0\n");
        return false;
    }

    // Read capability registers
    ctrl.capLength  = xhci_read8(ctrl.capBase, XHCI_CAP_CAPLENGTH);
    ctrl.hciVersion = xhci_read16(ctrl.capBase, XHCI_CAP_HCIVERSION);

    uint32_t hcsParams1 = xhci_read32(ctrl.capBase, XHCI_CAP_HCSPARAMS1);
    ctrl.maxSlots = hcsParams1 & 0xFF;
    ctrl.maxIntrs = (hcsParams1 >> 8) & 0x7FF;
    ctrl.maxPorts = (hcsParams1 >> 24) & 0xFF;

    uint32_t hcsParams2 = xhci_read32(ctrl.capBase, XHCI_CAP_HCSPARAMS2);
    // Scratchpad count = bits 31:27 (high) << 5 | bits 25:21 (low)
    uint32_t spHi = (hcsParams2 >> 27) & 0x1F;
    uint32_t spLo = (hcsParams2 >> 21) & 0x1F;
    ctrl.scratchpadCount = (spHi << 5) | spLo;

    uint32_t hccParams1 = xhci_read32(ctrl.capBase, XHCI_CAP_HCCPARAMS1);
    ctrl.ctx64 = (hccParams1 & HCC_CSZ) != 0;

    // Operational registers are at capBase + capLength
    ctrl.opBase = ctrl.capBase + ctrl.capLength;

    // Doorbell offset
    uint32_t dboff = xhci_read32(ctrl.capBase, XHCI_CAP_DBOFF) & ~0x3;
    ctrl.dbBase = ctrl.capBase + dboff;

    // Runtime register offset
    uint32_t rtsoff = xhci_read32(ctrl.capBase, XHCI_CAP_RTSOFF) & ~0x1F;
    ctrl.rtBase = ctrl.capBase + rtsoff;

    // Read page size (PAGESIZE register, bit N means 2^(N+12) bytes)
    uint32_t psReg = xhci_read32(ctrl.opBase, XHCI_OP_PAGESIZE);
    ctrl.pageSize = 4096; // default
    for (int i = 0; i < 16; i++) {
        if (psReg & (1 << i)) {
            ctrl.pageSize = 1U << (i + 12);
            break;
        }
    }

    SerialPrintf("xhci: version %x.%02x, capLen=%u\n",
                 ctrl.hciVersion >> 8, ctrl.hciVersion & 0xFF, ctrl.capLength);
    SerialPrintf("xhci: maxSlots=%u maxPorts=%u maxIntrs=%u ctx=%uB page=%u scratchpad=%u\n",
                 ctrl.maxSlots, ctrl.maxPorts, ctrl.maxIntrs,
                 ctrl.ctx64 ? 64 : 32, ctrl.pageSize, ctrl.scratchpadCount);

    return true;
}

// ---------------------------------------------------------------------------
// Phase 2: Controller reset and ring allocation
// ---------------------------------------------------------------------------

static bool XhciReset(XhciController& ctrl)
{
    // Stop the controller if running
    uint32_t cmd = xhci_read32(ctrl.opBase, XHCI_OP_USBCMD);
    if (cmd & USBCMD_RS) {
        xhci_write32(ctrl.opBase, XHCI_OP_USBCMD, cmd & ~USBCMD_RS);
        // Wait for HCH (Halted)
        for (int i = 0; i < 100; i++) {
            if (xhci_read32(ctrl.opBase, XHCI_OP_USBSTS) & USBSTS_HCH)
                break;
            for (volatile int d = 0; d < 100000; d++);
        }
    }

    // Reset
    xhci_write32(ctrl.opBase, XHCI_OP_USBCMD, USBCMD_HCRST);

    // Wait for reset to complete (HCRST clears and CNR clears)
    for (int i = 0; i < 1000; i++) {
        uint32_t sts = xhci_read32(ctrl.opBase, XHCI_OP_USBSTS);
        uint32_t cmd2 = xhci_read32(ctrl.opBase, XHCI_OP_USBCMD);
        if (!(cmd2 & USBCMD_HCRST) && !(sts & USBSTS_CNR)) {
            SerialPuts("xhci: controller reset complete\n");
            return true;
        }
        for (volatile int d = 0; d < 100000; d++);
    }

    SerialPuts("xhci: controller reset timeout!\n");
    return false;
}

static bool XhciAllocRings(XhciController& ctrl)
{
    // --- DCBAA (Device Context Base Address Array) ---
    // Array of uint64_t pointers, one per slot + slot 0 (for scratchpad)
    // Size: (maxSlots + 1) * 8 bytes, 64-byte aligned, page-aligned recommended
    ctrl.dcbaa = static_cast<uint64_t*>(
        AllocDmaBuffer(1, ctrl.dcbaaPhys));
    if (!ctrl.dcbaa) return false;

    SerialPrintf("xhci: DCBAA at virt=%p phys=0x%lx (slots=%u)\n",
                 ctrl.dcbaa, ctrl.dcbaaPhys, ctrl.maxSlots);

    // --- Scratchpad buffers (if required) ---
    if (ctrl.scratchpadCount > 0) {
        // Allocate scratchpad buffer array (array of physical page addresses)
        uint32_t arrayPages = ((ctrl.scratchpadCount * 8) + 4095) / 4096;
        ctrl.scratchpadArray = static_cast<uint64_t*>(
            AllocDmaBuffer(arrayPages, ctrl.scratchpadArrayPhys));
        if (!ctrl.scratchpadArray) return false;

        // Allocate each scratchpad page
        for (uint32_t i = 0; i < ctrl.scratchpadCount; i++) {
            auto page = PmmAllocPages(1);
            if (!page) {
                SerialPrintf("xhci: scratchpad page %u alloc failed\n", i);
                return false;
            }
            ctrl.scratchpadArray[i] = page.raw();
        }

        // DCBAA[0] points to the scratchpad array
        ctrl.dcbaa[0] = ctrl.scratchpadArrayPhys;
        SerialPrintf("xhci: allocated %u scratchpad buffers\n", ctrl.scratchpadCount);
    }

    // --- Command Ring ---
    uint32_t cmdRingPages = (CMD_RING_SIZE * sizeof(Trb) + 4095) / 4096;
    ctrl.cmdRing = static_cast<Trb*>(
        AllocDmaBuffer(cmdRingPages, ctrl.cmdRingPhys));
    if (!ctrl.cmdRing) return false;
    ctrl.cmdEnqueue = 0;
    ctrl.cmdCycle = true;

    // Place a Link TRB at the last slot to wrap the ring
    Trb& link = ctrl.cmdRing[CMD_RING_SIZE - 1];
    link.param = ctrl.cmdRingPhys;
    link.status = 0;
    link.control = TRB_TYPE_LINK | TRB_TC | (ctrl.cmdCycle ? TRB_CYCLE : 0);

    SerialPrintf("xhci: command ring at phys=0x%lx (%u TRBs)\n",
                 ctrl.cmdRingPhys, CMD_RING_SIZE);

    // --- Event Ring ---
    uint32_t evtRingPages = (EVT_RING_SIZE * sizeof(Trb) + 4095) / 4096;
    ctrl.evtRing = static_cast<Trb*>(
        AllocDmaBuffer(evtRingPages, ctrl.evtRingPhys));
    if (!ctrl.evtRing) return false;
    ctrl.evtDequeue = 0;
    ctrl.evtCycle = true;

    // Event Ring Segment Table (1 entry)
    ctrl.erst = static_cast<ErstEntry*>(
        AllocDmaBuffer(1, ctrl.erstPhys));
    if (!ctrl.erst) return false;
    ctrl.erst[0].ringSegBase = ctrl.evtRingPhys;
    ctrl.erst[0].ringSegSize = EVT_RING_SIZE;
    ctrl.erst[0].reserved = 0;

    SerialPrintf("xhci: event ring at phys=0x%lx (%u TRBs)\n",
                 ctrl.evtRingPhys, EVT_RING_SIZE);

    return true;
}

static bool XhciStartController(XhciController& ctrl)
{
    // Program DCBAAP
    xhci_write64(ctrl.opBase, XHCI_OP_DCBAAP, ctrl.dcbaaPhys);

    // Program Command Ring Control Register (CRCR)
    // Physical address of command ring | cycle state in bit 0
    xhci_write64(ctrl.opBase, XHCI_OP_CRCR,
                 ctrl.cmdRingPhys | (ctrl.cmdCycle ? 1 : 0));

    // Set max device slots enabled
    uint32_t maxSlots = ctrl.maxSlots;
    if (maxSlots > MAX_DEVICES) maxSlots = MAX_DEVICES;
    xhci_write32(ctrl.opBase, XHCI_OP_CONFIG, maxSlots);

    // Program Interrupter 0
    // Set Event Ring Segment Table Size
    xhci_write32(ctrl.rtBase, XHCI_RT_ERSTSZ, 1);

    // Set Event Ring Dequeue Pointer (must be set before ERSTBA)
    xhci_write64(ctrl.rtBase, XHCI_RT_ERDP, ctrl.evtRingPhys);

    // Set Event Ring Segment Table Base Address
    xhci_write64(ctrl.rtBase, XHCI_RT_ERSTBA, ctrl.erstPhys);

    // Enable interrupts on interrupter 0
    uint32_t iman = xhci_read32(ctrl.rtBase, XHCI_RT_IMAN);
    xhci_write32(ctrl.rtBase, XHCI_RT_IMAN, iman | 0x2); // IE bit

    // Start the controller
    uint32_t cmd = xhci_read32(ctrl.opBase, XHCI_OP_USBCMD);
    cmd |= USBCMD_RS | USBCMD_INTE;
    xhci_write32(ctrl.opBase, XHCI_OP_USBCMD, cmd);

    // Wait for controller to start (HCH clears)
    for (int i = 0; i < 100; i++) {
        uint32_t sts = xhci_read32(ctrl.opBase, XHCI_OP_USBSTS);
        if (!(sts & USBSTS_HCH)) {
            SerialPuts("xhci: controller started (R/S=1)\n");
            return true;
        }
        for (volatile int d = 0; d < 100000; d++);
    }

    SerialPuts("xhci: controller failed to start!\n");
    return false;
}

// ---------------------------------------------------------------------------
// Phase 2.5: Command submission
// ---------------------------------------------------------------------------

static void XhciRingDoorbell(XhciController& ctrl, uint32_t slot, uint32_t target)
{
    // Doorbell register: slot 0 = host controller, slot N = device N
    // target: 0 = command ring, 1+ = endpoint
    xhci_write32(ctrl.dbBase, slot * 4, target);
}

static bool XhciSubmitCommand(XhciController& ctrl, uint64_t param,
                               uint32_t status, uint32_t control)
{
    uint32_t idx = ctrl.cmdEnqueue;
    if (idx >= CMD_RING_SIZE - 1) {
        // Wrap: the last TRB is a Link TRB
        // Toggle cycle bit on the link TRB
        ctrl.cmdRing[CMD_RING_SIZE - 1].control =
            TRB_TYPE_LINK | TRB_TC | (ctrl.cmdCycle ? TRB_CYCLE : 0);
        ctrl.cmdCycle = !ctrl.cmdCycle;
        ctrl.cmdEnqueue = 0;
        idx = 0;
    }

    Trb& trb = ctrl.cmdRing[idx];
    trb.param = param;
    trb.status = status;
    // Set cycle bit according to current producer cycle
    trb.control = (control & ~TRB_CYCLE) | (ctrl.cmdCycle ? TRB_CYCLE : 0);

    // Memory barrier to ensure TRB is visible before doorbell
    __asm__ volatile("mfence" ::: "memory");

    ctrl.cmdEnqueue = idx + 1;

    // Ring the host controller doorbell (slot 0, target 0 = command ring)
    XhciRingDoorbell(ctrl, 0, 0);
    return true;
}

// Poll the event ring for a command completion. Returns the completion
// TRB or nullptr after timeout.
static Trb* XhciWaitForEvent(XhciController& ctrl, uint32_t expectedType,
                              uint32_t timeoutMs)
{
    uint64_t deadline = brook::g_lapicTickCount + timeoutMs;

    while (brook::g_lapicTickCount < deadline) {
        uint32_t idx = ctrl.evtDequeue;
        Trb& evt = ctrl.evtRing[idx];

        // Check cycle bit — if it matches our expected cycle, this is a new event
        bool evtCycleBit = (evt.control & TRB_CYCLE) != 0;
        if (evtCycleBit != ctrl.evtCycle) {
            // No new event yet
            for (volatile int d = 0; d < 10000; d++);
            continue;
        }

        // We have a new event — advance dequeue
        ctrl.evtDequeue++;
        if (ctrl.evtDequeue >= EVT_RING_SIZE) {
            ctrl.evtDequeue = 0;
            ctrl.evtCycle = !ctrl.evtCycle;
        }

        // Update ERDP to tell controller we consumed this event
        uint64_t erdpPhys = ctrl.evtRingPhys + ctrl.evtDequeue * sizeof(Trb);
        // Set EHB (Event Handler Busy) bit to clear interrupt
        xhci_write64(ctrl.rtBase, XHCI_RT_ERDP, erdpPhys | (1ULL << 3));

        uint32_t type = evt.control & TRB_TYPE_MASK;

        // Log the event
        uint32_t cc = (evt.status >> TRB_CC_SHIFT) & 0xFF;

        if (type == TRB_TYPE_PORT_STATUS_CHANGE) {
            uint32_t portId = (evt.param >> 24) & 0xFF;
            SerialPrintf("xhci: port %u status change (cc=%u)\n", portId, cc);
        }

        if (expectedType == 0 || type == expectedType) {
            // Return a pointer to a static copy (event ring gets reused)
            static Trb lastEvt;
            lastEvt = evt;
            return &lastEvt;
        }
    }

    return nullptr; // timeout
}

// ---------------------------------------------------------------------------
// Phase 3: Port status and device detection
// ---------------------------------------------------------------------------

static const char* XhciSpeedString(uint32_t speed)
{
    switch (speed) {
    case PORT_SPEED_FULL:  return "Full (12 Mbps)";
    case PORT_SPEED_LOW:   return "Low (1.5 Mbps)";
    case PORT_SPEED_HIGH:  return "High (480 Mbps)";
    case PORT_SPEED_SUPER: return "Super (5 Gbps)";
    default: return "Unknown";
    }
}

// Max packet size for control EP0 based on port speed
static uint32_t XhciEp0MaxPacket(uint32_t speed)
{
    switch (speed) {
    case PORT_SPEED_LOW:   return 8;
    case PORT_SPEED_FULL:  return 64;
    case PORT_SPEED_HIGH:  return 64;
    case PORT_SPEED_SUPER: return 512;
    default: return 8;
    }
}

// Map port speed to xHCI Slot Context speed field
static uint32_t XhciSlotSpeed(uint32_t speed)
{
    // xHCI Slot Context speed values match PORTSC speed values
    return speed;
}

// ---------------------------------------------------------------------------
// Per-device state
// ---------------------------------------------------------------------------

struct XhciDevice {
    uint32_t slotId;
    uint32_t portNum;       // 1-based
    uint32_t portSpeed;     // PORTSC speed field value

    // Transfer ring for EP0 (control endpoint)
    Trb*     ep0Ring;
    uint64_t ep0RingPhys;
    uint32_t ep0Enqueue;
    bool     ep0Cycle;

    // Device context (output, written by xHC)
    void*    outputCtx;
    uint64_t outputCtxPhys;

    // Input context (set up by driver, read by xHC)
    void*    inputCtx;
    uint64_t inputCtxPhys;

    // Device info from descriptors
    uint16_t vendorId;
    uint16_t productId;
    uint8_t  deviceClass;
    uint8_t  deviceSubClass;
    uint8_t  deviceProtocol;
    uint8_t  numConfigurations;
    uint8_t  ifaceClass;
    uint8_t  ifaceSubClass;
    uint8_t  ifaceProtocol;
    uint8_t  interruptEpAddr;   // interrupt IN endpoint address
    uint16_t interruptMaxPacket;
    uint8_t  interruptInterval;

    // Interrupt IN transfer ring (for HID polling)
    Trb*     intRing;
    uint64_t intRingPhys;
    uint32_t intEnqueue;
    bool     intCycle;
    uint32_t intDci;            // device context index for interrupt EP

    bool     configured;
    bool     isKeyboard;
    bool     isMouse;

    void*    priv;          // driver-private (DMA report buffer for HID)
};

static XhciDevice g_devices[MAX_DEVICES];
static uint32_t   g_deviceCount = 0;

// ---------------------------------------------------------------------------
// Phase 4: Enable Slot
// ---------------------------------------------------------------------------

static int XhciEnableSlot(XhciController& ctrl)
{
    SerialPuts("xhci: sending Enable Slot command\n");
    XhciSubmitCommand(ctrl, 0, 0, TRB_TYPE_ENABLE_SLOT);

    Trb* evt = XhciWaitForEvent(ctrl, TRB_TYPE_CMD_COMPLETION, 1000);
    if (!evt) {
        SerialPuts("xhci: Enable Slot timeout\n");
        return -1;
    }

    uint32_t cc = (evt->status >> TRB_CC_SHIFT) & 0xFF;
    uint32_t slotId = (evt->control >> 24) & 0xFF;

    if (cc != TRB_CC_SUCCESS) {
        SerialPrintf("xhci: Enable Slot failed (cc=%u)\n", cc);
        return -1;
    }

    SerialPrintf("xhci: slot %u enabled\n", slotId);
    return static_cast<int>(slotId);
}

// ---------------------------------------------------------------------------
// Phase 4: Allocate transfer ring for an endpoint
// ---------------------------------------------------------------------------

static bool XhciAllocTransferRing(Trb*& ring, uint64_t& ringPhys,
                                   uint32_t& enqueue, bool& cycle)
{
    ring = static_cast<Trb*>(AllocDmaBuffer(1, ringPhys));
    if (!ring) return false;

    // Set up Link TRB at the end pointing back to start
    Trb& link = ring[XFER_RING_SIZE - 1];
    link.param = ringPhys;
    link.status = 0;
    link.control = TRB_TYPE_LINK | TRB_TC | TRB_CYCLE;

    enqueue = 0;
    cycle = true;
    return true;
}

// ---------------------------------------------------------------------------
// Phase 4: Address Device
// ---------------------------------------------------------------------------

// Context entry size: 32 or 64 bytes depending on CSZ bit
static uint32_t CtxEntrySize(XhciController& ctrl) { return ctrl.ctx64 ? 64 : 32; }

// Input Context layout:
//   [0]: Input Control Context (32 or 64 bytes)
//   [1]: Slot Context
//   [2]: EP0 Context (Endpoint Context Index 1 = DCI 1)
//   Total: 3 entries minimum for Address Device
static bool XhciAddressDevice(XhciController& ctrl, XhciDevice& dev)
{
    uint32_t ctxSize = CtxEntrySize(ctrl);

    // Allocate input context — needs at least 33 entries (control + slot + 31 EPs)
    // but we only fill control + slot + EP0
    uint32_t inputPages = (ctxSize * 33 + 4095) / 4096;
    dev.inputCtx = AllocDmaBuffer(inputPages, dev.inputCtxPhys);
    if (!dev.inputCtx) {
        SerialPuts("xhci: failed to allocate input context\n");
        return false;
    }

    // Allocate output (device) context — same size
    uint32_t outputPages = (ctxSize * 32 + 4095) / 4096;
    dev.outputCtx = AllocDmaBuffer(outputPages, dev.outputCtxPhys);
    if (!dev.outputCtx) {
        SerialPuts("xhci: failed to allocate output context\n");
        return false;
    }

    // Install output context pointer in DCBAA
    ctrl.dcbaa[dev.slotId] = dev.outputCtxPhys;
    __asm__ volatile("mfence" ::: "memory");

    // Allocate EP0 transfer ring
    if (!XhciAllocTransferRing(dev.ep0Ring, dev.ep0RingPhys,
                                dev.ep0Enqueue, dev.ep0Cycle)) {
        SerialPuts("xhci: failed to allocate EP0 transfer ring\n");
        return false;
    }

    // Fill Input Control Context
    // Bits 1:0 of Add Context Flags: bit 0 = Slot, bit 1 = EP0
    auto* icc = reinterpret_cast<uint32_t*>(
        static_cast<uint8_t*>(dev.inputCtx));
    icc[1] = 0x3; // Add Slot Context (A0) + EP0 Context (A1)

    // Fill Slot Context (entry 1 in input context)
    auto* slotCtx = reinterpret_cast<uint32_t*>(
        static_cast<uint8_t*>(dev.inputCtx) + ctxSize);

    // Slot Context DW0: Route String (0), Speed, Context Entries (1 = just EP0)
    uint32_t speed = XhciSlotSpeed(dev.portSpeed);
    slotCtx[0] = (1 << 27) | // Context Entries = 1
                 (speed << 20); // Speed

    // Slot Context DW1: Root Hub Port Number (1-based)
    slotCtx[1] = (dev.portNum << 16); // Root Hub Port Number

    // Fill EP0 Context (entry 2 in input context)
    auto* ep0Ctx = reinterpret_cast<uint32_t*>(
        static_cast<uint8_t*>(dev.inputCtx) + ctxSize * 2);

    uint32_t maxPacket = XhciEp0MaxPacket(dev.portSpeed);

    // EP0 DW1: EP Type = Control (4), Max Packet Size, CErr = 3
    ep0Ctx[1] = (3 << 1)  | // CErr = 3
                (4 << 3)  | // EP Type = Control Bidirectional
                (maxPacket << 16);

    // EP0 DW2-3: TR Dequeue Pointer (physical, with DCS=1)
    uint64_t trDqp = dev.ep0RingPhys | 1; // DCS = 1 (cycle state)
    ep0Ctx[2] = static_cast<uint32_t>(trDqp);
    ep0Ctx[3] = static_cast<uint32_t>(trDqp >> 32);

    // EP0 DW4: Average TRB length (8 for control setup packets)
    ep0Ctx[4] = 8;

    __asm__ volatile("mfence" ::: "memory");

    // Submit Address Device command (slot ID in bits 31:24 of control)
    SerialPrintf("xhci: addressing device on slot %u (port %u, speed=%s)\n",
                 dev.slotId, dev.portNum, XhciSpeedString(dev.portSpeed));

    XhciSubmitCommand(ctrl, dev.inputCtxPhys, 0,
                      TRB_TYPE_ADDRESS_DEV | (dev.slotId << 24));

    Trb* evt = XhciWaitForEvent(ctrl, TRB_TYPE_CMD_COMPLETION, 2000);
    if (!evt) {
        SerialPuts("xhci: Address Device timeout\n");
        return false;
    }

    uint32_t cc = (evt->status >> TRB_CC_SHIFT) & 0xFF;
    if (cc != TRB_CC_SUCCESS) {
        SerialPrintf("xhci: Address Device failed (cc=%u)\n", cc);
        return false;
    }

    SerialPrintf("xhci: device addressed on slot %u\n", dev.slotId);
    return true;
}

// ---------------------------------------------------------------------------
// Phase 4-5: Control Transfer (EP0)
// ---------------------------------------------------------------------------

// Submit a Setup+Data+Status control transfer on EP0 and wait for completion.
// dataDir: 0 = no data, 2 = OUT (host→device), 3 = IN (device→host)
static bool XhciControlTransfer(XhciController& ctrl, XhciDevice& dev,
                                 const UsbSetupPacket& setup,
                                 void* data, uint32_t dataLen,
                                 uint64_t dataPhys, uint32_t dataDir)
{
    // 1. Setup TRB (Immediate Data)
    uint32_t idx = dev.ep0Enqueue;

    // Setup stage: 8 bytes of setup packet in param field (IDT)
    Trb& setupTrb = dev.ep0Ring[idx];
    // Copy setup packet into param
    uint64_t setupData;
    __builtin_memcpy(&setupData, &setup, 8);
    setupTrb.param = setupData;
    setupTrb.status = 8; // TRB Transfer Length = 8 bytes
    // TRT (Transfer Type): 0=No Data, 2=OUT Data, 3=IN Data
    setupTrb.control = TRB_TYPE_SETUP | TRB_IDT |
                       (dev.ep0Cycle ? TRB_CYCLE : 0) |
                       (dataDir << 16); // TRT field
    idx++;

    // 2. Data TRB (if there's a data stage)
    if (dataLen > 0 && dataDir != 0) {
        if (idx >= XFER_RING_SIZE - 1) {
            // Wrap with link TRB
            dev.ep0Ring[XFER_RING_SIZE - 1].control =
                TRB_TYPE_LINK | TRB_TC | (dev.ep0Cycle ? TRB_CYCLE : 0);
            dev.ep0Cycle = !dev.ep0Cycle;
            idx = 0;
        }

        Trb& dataTrb = dev.ep0Ring[idx];
        dataTrb.param = dataPhys;
        dataTrb.status = dataLen;
        // DIR bit (bit 16): 0 = OUT, 1 = IN
        uint32_t dirBit = (dataDir == 3) ? (1 << 16) : 0;
        dataTrb.control = TRB_TYPE_DATA | dirBit | TRB_IOC |
                          (dev.ep0Cycle ? TRB_CYCLE : 0);
        idx++;
    }

    // 3. Status TRB
    if (idx >= XFER_RING_SIZE - 1) {
        dev.ep0Ring[XFER_RING_SIZE - 1].control =
            TRB_TYPE_LINK | TRB_TC | (dev.ep0Cycle ? TRB_CYCLE : 0);
        dev.ep0Cycle = !dev.ep0Cycle;
        idx = 0;
    }

    Trb& statusTrb = dev.ep0Ring[idx];
    statusTrb.param = 0;
    statusTrb.status = 0;
    // Direction of status is opposite of data (or IN for no-data)
    uint32_t statusDir = (dataDir == 3) ? 0 : (1 << 16);
    statusTrb.control = TRB_TYPE_STATUS | TRB_IOC | statusDir |
                        (dev.ep0Cycle ? TRB_CYCLE : 0);
    idx++;

    dev.ep0Enqueue = idx;

    __asm__ volatile("mfence" ::: "memory");

    // Ring doorbell for this slot, target = 1 (EP0 = DCI 1)
    XhciRingDoorbell(ctrl, dev.slotId, 1);

    // Wait for transfer event
    Trb* evt = XhciWaitForEvent(ctrl, TRB_TYPE_TRANSFER_EVENT, 2000);
    if (!evt) {
        SerialPuts("xhci: control transfer timeout\n");
        return false;
    }

    uint32_t cc = (evt->status >> TRB_CC_SHIFT) & 0xFF;
    if (cc != TRB_CC_SUCCESS && cc != TRB_CC_SHORT_PACKET) {
        SerialPrintf("xhci: control transfer failed (cc=%u)\n", cc);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Phase 5: GET_DEVICE_DESCRIPTOR
// ---------------------------------------------------------------------------

static bool XhciGetDeviceDescriptor(XhciController& ctrl, XhciDevice& dev)
{
    // Allocate a DMA-accessible buffer for the descriptor
    uint64_t bufPhys;
    auto* buf = static_cast<uint8_t*>(AllocDmaBuffer(1, bufPhys));
    if (!buf) return false;

    UsbSetupPacket setup = {};
    setup.bmRequestType = 0x80; // Device-to-Host, Standard, Device
    setup.bRequest = USB_REQ_GET_DESCRIPTOR;
    setup.wValue = (USB_DESC_DEVICE << 8) | 0; // Descriptor Type + Index
    setup.wIndex = 0;
    setup.wLength = sizeof(UsbDeviceDescriptor);

    if (!XhciControlTransfer(ctrl, dev, setup, buf, sizeof(UsbDeviceDescriptor),
                              bufPhys, 3 /* IN */)) {
        SerialPuts("xhci: GET_DEVICE_DESCRIPTOR failed\n");
        return false;
    }

    auto* desc = reinterpret_cast<UsbDeviceDescriptor*>(buf);
    dev.vendorId = desc->idVendor;
    dev.productId = desc->idProduct;
    dev.deviceClass = desc->bDeviceClass;
    dev.deviceSubClass = desc->bDeviceSubClass;
    dev.deviceProtocol = desc->bDeviceProtocol;
    dev.numConfigurations = desc->bNumConfigurations;

    SerialPrintf("xhci: device descriptor: USB %x.%02x class=%u/%u/%u "
                 "vendor=%04x product=%04x configs=%u maxPkt0=%u\n",
                 desc->bcdUSB >> 8, desc->bcdUSB & 0xFF,
                 desc->bDeviceClass, desc->bDeviceSubClass,
                 desc->bDeviceProtocol,
                 desc->idVendor, desc->idProduct,
                 desc->bNumConfigurations, desc->bMaxPacketSize0);

    return true;
}

// ---------------------------------------------------------------------------
// Phase 5: GET_CONFIG_DESCRIPTOR + SET_CONFIGURATION + parse interfaces
// ---------------------------------------------------------------------------

static bool XhciConfigureDevice(XhciController& ctrl, XhciDevice& dev)
{
    // First read just the config descriptor header to get wTotalLength
    uint64_t bufPhys;
    auto* buf = static_cast<uint8_t*>(AllocDmaBuffer(1, bufPhys));
    if (!buf) return false;

    UsbSetupPacket setup = {};
    setup.bmRequestType = 0x80;
    setup.bRequest = USB_REQ_GET_DESCRIPTOR;
    setup.wValue = (USB_DESC_CONFIG << 8) | 0;
    setup.wIndex = 0;
    setup.wLength = sizeof(UsbConfigDescriptor);

    if (!XhciControlTransfer(ctrl, dev, setup, buf, sizeof(UsbConfigDescriptor),
                              bufPhys, 3)) {
        SerialPuts("xhci: GET_CONFIG_DESCRIPTOR (header) failed\n");
        return false;
    }

    auto* cfgHdr = reinterpret_cast<UsbConfigDescriptor*>(buf);
    uint16_t totalLen = cfgHdr->wTotalLength;
    uint8_t  cfgValue = cfgHdr->bConfigurationValue;

    SerialPrintf("xhci: config descriptor: totalLen=%u numIfaces=%u cfgVal=%u\n",
                 totalLen, cfgHdr->bNumInterfaces, cfgValue);

    if (totalLen > 4096) totalLen = 4096;

    // Read the full configuration descriptor tree
    setup.wLength = totalLen;
    if (!XhciControlTransfer(ctrl, dev, setup, buf, totalLen, bufPhys, 3)) {
        SerialPuts("xhci: GET_CONFIG_DESCRIPTOR (full) failed\n");
        return false;
    }

    // Parse interface and endpoint descriptors
    uint16_t offset = cfgHdr->bLength;
    while (offset + 2 <= totalLen) {
        uint8_t dLen  = buf[offset];
        uint8_t dType = buf[offset + 1];
        if (dLen < 2) break;

        if (dType == USB_DESC_INTERFACE && dLen >= sizeof(UsbInterfaceDescriptor)) {
            auto* iface = reinterpret_cast<UsbInterfaceDescriptor*>(buf + offset);
            dev.ifaceClass = iface->bInterfaceClass;
            dev.ifaceSubClass = iface->bInterfaceSubClass;
            dev.ifaceProtocol = iface->bInterfaceProtocol;

            SerialPrintf("xhci:   interface %u: class=%u/%u/%u endpoints=%u\n",
                         iface->bInterfaceNumber,
                         iface->bInterfaceClass, iface->bInterfaceSubClass,
                         iface->bInterfaceProtocol, iface->bNumEndpoints);

            if (iface->bInterfaceClass == USB_CLASS_HID) {
                if (iface->bInterfaceProtocol == USB_HID_PROTOCOL_KEYBOARD)
                    dev.isKeyboard = true;
                if (iface->bInterfaceProtocol == USB_HID_PROTOCOL_MOUSE)
                    dev.isMouse = true;
            }
        }

        if (dType == USB_DESC_ENDPOINT && dLen >= sizeof(UsbEndpointDescriptor)) {
            auto* ep = reinterpret_cast<UsbEndpointDescriptor*>(buf + offset);

            uint8_t epAddr = ep->bEndpointAddress;
            uint8_t epType = ep->bmAttributes & 0x3;
            bool    isIn   = (epAddr & 0x80) != 0;

            SerialPrintf("xhci:   endpoint 0x%02x: type=%u %s maxPkt=%u interval=%u\n",
                         epAddr, epType, isIn ? "IN" : "OUT",
                         ep->wMaxPacketSize, ep->bInterval);

            // Remember interrupt IN endpoint for HID
            if (epType == 3 && isIn) { // Interrupt IN
                dev.interruptEpAddr = epAddr;
                dev.interruptMaxPacket = ep->wMaxPacketSize;
                dev.interruptInterval = ep->bInterval;
            }
        }

        offset += dLen;
    }

    // SET_CONFIGURATION
    UsbSetupPacket setCfg = {};
    setCfg.bmRequestType = 0x00; // Host-to-Device, Standard, Device
    setCfg.bRequest = USB_REQ_SET_CONFIG;
    setCfg.wValue = cfgValue;
    setCfg.wIndex = 0;
    setCfg.wLength = 0;

    if (!XhciControlTransfer(ctrl, dev, setCfg, nullptr, 0, 0, 0)) {
        SerialPuts("xhci: SET_CONFIGURATION failed\n");
        return false;
    }

    SerialPrintf("xhci: device configured (cfg=%u)\n", cfgValue);
    dev.configured = true;
    return true;
}

// ---------------------------------------------------------------------------
// Phase 6: Configure interrupt endpoint in xHCI (for HID polling)
// ---------------------------------------------------------------------------

static bool XhciConfigureInterruptEndpoint(XhciController& ctrl, XhciDevice& dev)
{
    if (!dev.interruptEpAddr) {
        SerialPuts("xhci: no interrupt endpoint found\n");
        return false;
    }

    // EP address: bit 7 = direction (1=IN), bits 3:0 = EP number
    uint8_t epNum = dev.interruptEpAddr & 0x0F;
    bool isIn = (dev.interruptEpAddr & 0x80) != 0;

    // Device Context Index: DCI = 2*epNum + (isIn ? 1 : 0)
    // EP0 IN/OUT = DCI 1, EP1 OUT = DCI 2, EP1 IN = DCI 3, etc.
    dev.intDci = epNum * 2 + (isIn ? 1 : 0);

    SerialPrintf("xhci: configuring interrupt EP 0x%02x (DCI %u)\n",
                 dev.interruptEpAddr, dev.intDci);

    // Allocate transfer ring for the interrupt endpoint
    if (!XhciAllocTransferRing(dev.intRing, dev.intRingPhys,
                                dev.intEnqueue, dev.intCycle)) {
        SerialPuts("xhci: failed to allocate interrupt transfer ring\n");
        return false;
    }

    uint32_t ctxSize = CtxEntrySize(ctrl);

    // Reuse the input context — zero it first
    auto* input = static_cast<uint8_t*>(dev.inputCtx);
    for (uint32_t i = 0; i < ctxSize * 33; i++) input[i] = 0;

    // Input Control Context: add slot + the interrupt endpoint
    auto* icc = reinterpret_cast<uint32_t*>(input);
    icc[1] = (1 << 0) | (1 << dev.intDci); // Add Slot + EP

    // Slot Context: update Context Entries to include the new EP
    auto* slotCtx = reinterpret_cast<uint32_t*>(input + ctxSize);
    // Read current slot context from output context
    auto* outSlot = reinterpret_cast<uint32_t*>(
        static_cast<uint8_t*>(dev.outputCtx));
    slotCtx[0] = outSlot[0];
    slotCtx[1] = outSlot[1];
    slotCtx[2] = outSlot[2];
    slotCtx[3] = outSlot[3];
    // Update Context Entries to max DCI
    slotCtx[0] = (slotCtx[0] & ~(0x1F << 27)) | (dev.intDci << 27);

    // Endpoint Context for the interrupt IN endpoint
    auto* epCtx = reinterpret_cast<uint32_t*>(input + ctxSize * (dev.intDci + 1));

    // EP DW1: CErr=3, EP Type=7 (Interrupt IN), Max Packet Size
    epCtx[1] = (3 << 1) |      // CErr = 3
               (7 << 3) |      // EP Type = Interrupt IN
               (dev.interruptMaxPacket << 16);

    // EP DW2-3: TR Dequeue Pointer with DCS=1
    uint64_t trDqp = dev.intRingPhys | 1;
    epCtx[2] = static_cast<uint32_t>(trDqp);
    epCtx[3] = static_cast<uint32_t>(trDqp >> 32);

    // EP DW4: Average TRB Length (8 for keyboard), Max ESIT Payload (8)
    epCtx[4] = 8 | (8 << 16);

    __asm__ volatile("mfence" ::: "memory");

    // Submit Configure Endpoint command
    XhciSubmitCommand(ctrl, dev.inputCtxPhys, 0,
                      TRB_TYPE_CONFIG_EP | (dev.slotId << 24));

    Trb* evt = XhciWaitForEvent(ctrl, TRB_TYPE_CMD_COMPLETION, 2000);
    if (!evt) {
        SerialPuts("xhci: Configure Endpoint timeout\n");
        return false;
    }

    uint32_t cc = (evt->status >> TRB_CC_SHIFT) & 0xFF;
    if (cc != TRB_CC_SUCCESS) {
        SerialPrintf("xhci: Configure Endpoint failed (cc=%u)\n", cc);
        return false;
    }

    SerialPrintf("xhci: interrupt endpoint configured (DCI %u)\n", dev.intDci);
    return true;
}

// ---------------------------------------------------------------------------
// Phase 6: Set HID boot protocol
// ---------------------------------------------------------------------------

static bool XhciSetBootProtocol(XhciController& ctrl, XhciDevice& dev)
{
    UsbSetupPacket setup = {};
    setup.bmRequestType = 0x21; // Host-to-Device, Class, Interface
    setup.bRequest = USB_REQ_SET_PROTOCOL;
    setup.wValue = 0; // 0 = Boot Protocol
    setup.wIndex = 0; // Interface 0
    setup.wLength = 0;

    if (!XhciControlTransfer(ctrl, dev, setup, nullptr, 0, 0, 0)) {
        SerialPuts("xhci: SET_PROTOCOL (boot) failed\n");
        return false;
    }

    SerialPuts("xhci: HID boot protocol set\n");
    return true;
}

// ---------------------------------------------------------------------------
// Phase 6: Queue interrupt IN transfer for keyboard polling
// ---------------------------------------------------------------------------

static bool XhciQueueInterruptIn(XhciController& ctrl, XhciDevice& dev,
                                  void* buf, uint64_t bufPhys, uint32_t len)
{
    uint32_t idx = dev.intEnqueue;
    if (idx >= XFER_RING_SIZE - 1) {
        // Wrap
        dev.intRing[XFER_RING_SIZE - 1].control =
            TRB_TYPE_LINK | TRB_TC | (dev.intCycle ? TRB_CYCLE : 0);
        dev.intCycle = !dev.intCycle;
        idx = 0;
    }

    Trb& trb = dev.intRing[idx];
    trb.param = bufPhys;
    trb.status = len;
    trb.control = TRB_TYPE_NORMAL | TRB_IOC | TRB_ISP |
                  (dev.intCycle ? TRB_CYCLE : 0);

    dev.intEnqueue = idx + 1;
    __asm__ volatile("mfence" ::: "memory");

    // Ring doorbell for interrupt endpoint
    XhciRingDoorbell(ctrl, dev.slotId, dev.intDci);
    return true;
}

// ---------------------------------------------------------------------------
// HID boot keyboard report → scancode translation
// ---------------------------------------------------------------------------

// USB HID boot keyboard report: 8 bytes
// Byte 0: modifier keys (Ctrl, Shift, Alt, GUI)
// Byte 1: reserved
// Bytes 2-7: up to 6 simultaneous key codes

static constexpr uint8_t MOD_LEFT_CTRL   = 0x01;
static constexpr uint8_t MOD_LEFT_SHIFT  = 0x02;
static constexpr uint8_t MOD_LEFT_ALT    = 0x04;
static constexpr uint8_t MOD_RIGHT_CTRL  = 0x10;
static constexpr uint8_t MOD_RIGHT_SHIFT = 0x20;
static constexpr uint8_t MOD_RIGHT_ALT   = 0x40;

// USB HID usage ID → Linux input event code (KEY_* from linux/input-event-codes.h)
// Only covers the basic alphanumeric + common keys
// These map to PS/2 scan code set 1 values used by Brook's input subsystem
static const uint8_t g_hidToScancode[128] = {
    0,   0,   0,   0,   30,  48,  46,  32,  // 0x00-0x07: None,None,None,None, A,B,C,D
    18,  33,  34,  35,  23,  36,  37,  38,  // 0x08-0x0F: E,F,G,H, I,J,K,L
    50,  49,  24,  25,  16,  19,  31,  20,  // 0x10-0x17: M,N,O,P, Q,R,S,T
    22,  47,  17,  45,  21,  44,  2,   3,   // 0x18-0x1F: U,V,W,X, Y,Z,1,2
    4,   5,   6,   7,   8,   9,   10,  11,  // 0x20-0x27: 3,4,5,6, 7,8,9,0
    28,  1,   14,  15,  57,  12,  13,  26,  // 0x28-0x2F: Enter,Esc,Bksp,Tab, Space,-,=,[
    27,  43,  43,  39,  40,  41,  51,  52,  // 0x30-0x37: ],\,\,;, ',`,,,. 
    53,  58,  59,  60,  61,  62,  63,  64,  // 0x38-0x3F: /,CapsLk,F1,F2, F3,F4,F5,F6
    65,  66,  67,  68,  87,  88,  99,  70,  // 0x40-0x47: F7,F8,F9,F10, F11,F12,PrtSc,ScrLk
    119, 110, 102, 104, 111, 107, 109, 106, // 0x48-0x4F: Pause,Ins,Home,PgUp, Del,End,PgDn,Right
    105, 108, 103, 69,  98,  55,  74,  78,  // 0x50-0x57: Left,Down,Up,NumLk, KP/,KP*,KP-,KP+
    96,  79,  80,  81,  75,  76,  77,  71,  // 0x58-0x5F: KPEnt,KP1,KP2,KP3, KP4,KP5,KP6,KP7
    72,  73,  82,  83,  0,   0,   0,   0,   // 0x60-0x67: KP8,KP9,KP0,KP., unused...
    0,   0,   0,   0,   0,   0,   0,   0,   // 0x68-0x6F
    0,   0,   0,   0,   0,   0,   0,   0,   // 0x70-0x77
    0,   0,   0,   0,   0,   0,   0,   0,   // 0x78-0x7F
};

// USB keyboard input device
static InputDevice g_usbKbdDev;
static InputDeviceOps g_usbKbdOps = { "usb_kbd", nullptr };

// Track previous report for key up/down detection
static uint8_t g_prevReport[8] = {};

static void XhciPushKey(uint8_t scancode, bool pressed, uint8_t modifiers)
{
    InputEvent ev;
    ev.type = pressed ? InputEventType::KeyPress : InputEventType::KeyRelease;
    ev.scanCode = scancode;
    ev.modifiers = modifiers;
    ev.ascii = 0; // ASCII translation handled by keyboard subsystem
    InputDevicePush(&g_usbKbdDev, ev);
}

static void XhciProcessKeyboardReport(const uint8_t* report)
{
    uint8_t mods = report[0];
    uint8_t prevMods = g_prevReport[0];

    // Build Brook modifier bitmask
    uint8_t brookMods = 0;
    if (mods & MOD_LEFT_SHIFT)  brookMods |= INPUT_MOD_LSHIFT;
    if (mods & MOD_RIGHT_SHIFT) brookMods |= INPUT_MOD_RSHIFT;
    if (mods & (MOD_LEFT_CTRL | MOD_RIGHT_CTRL)) brookMods |= INPUT_MOD_CTRL;
    if (mods & (MOD_LEFT_ALT | MOD_RIGHT_ALT))   brookMods |= INPUT_MOD_ALT;

    // Check each modifier for press/release
    struct { uint8_t mask; uint8_t sc; } modMap[] = {
        { MOD_LEFT_CTRL,   29 },  // KEY_LEFTCTRL
        { MOD_LEFT_SHIFT,  42 },  // KEY_LEFTSHIFT
        { MOD_LEFT_ALT,    56 },  // KEY_LEFTALT
        { MOD_RIGHT_CTRL,  97 },  // KEY_RIGHTCTRL
        { MOD_RIGHT_SHIFT, 54 },  // KEY_RIGHTSHIFT
        { MOD_RIGHT_ALT,   100 }, // KEY_RIGHTALT
    };

    for (auto& m : modMap) {
        bool now  = (mods & m.mask) != 0;
        bool prev = (prevMods & m.mask) != 0;
        if (now != prev)
            XhciPushKey(m.sc, now, brookMods);
    }

    // Regular keys (bytes 2-7): detect releases then presses
    for (int i = 2; i < 8; i++) {
        uint8_t key = g_prevReport[i];
        if (key < 4 || key >= 128) continue;
        bool found = false;
        for (int j = 2; j < 8; j++)
            if (report[j] == key) { found = true; break; }
        if (!found) {
            uint8_t sc = g_hidToScancode[key];
            if (sc) XhciPushKey(sc, false, brookMods);
        }
    }

    for (int i = 2; i < 8; i++) {
        uint8_t key = report[i];
        if (key < 4 || key >= 128) continue;
        bool found = false;
        for (int j = 2; j < 8; j++)
            if (g_prevReport[j] == key) { found = true; break; }
        if (!found) {
            uint8_t sc = g_hidToScancode[key];
            if (sc) XhciPushKey(sc, true, brookMods);
        }
    }

    __builtin_memcpy(g_prevReport, report, 8);
    InputWakeWaiters();
}

// ---------------------------------------------------------------------------
// Keyboard polling state
// ---------------------------------------------------------------------------

static uint64_t g_kbdReportBufPhys = 0;
static uint32_t g_kbdDevIndex = 0;

// Poll for USB keyboard events — called by the input subsystem
static void XhciKeyboardPoll(InputDevice* /*dev*/)
{
    if (g_controllerCount == 0) return;
    XhciController& ctrl = g_controllers[0];
    if (!ctrl.initialized) return;
    if (g_kbdDevIndex >= g_deviceCount) return;

    XhciDevice& kbd = g_devices[g_kbdDevIndex];
    if (!kbd.isKeyboard || !kbd.intRing) return;

    // Check for transfer completion events on the event ring
    Trb* evt = XhciWaitForEvent(ctrl, TRB_TYPE_TRANSFER_EVENT, 0);
    if (!evt) return;

    uint32_t cc = (evt->status >> TRB_CC_SHIFT) & 0xFF;
    if (cc == TRB_CC_SUCCESS || cc == TRB_CC_SHORT_PACKET) {
        // Process the HID boot report
        auto* report = static_cast<uint8_t*>(kbd.priv);
        if (report)
            XhciProcessKeyboardReport(report);
    }

    // Re-queue the interrupt IN transfer for the next report
    if (kbd.priv && g_kbdReportBufPhys) {
        XhciQueueInterruptIn(ctrl, kbd,
                              kbd.priv, g_kbdReportBufPhys, 8);
    }
}

// ---------------------------------------------------------------------------
// Phase 3+4+5+6 combined: enumerate and configure a connected port
// ---------------------------------------------------------------------------

static void XhciEnumeratePort(XhciController& ctrl, uint32_t portNum,
                               uint32_t portSpeed)
{
    if (g_deviceCount >= MAX_DEVICES) {
        SerialPuts("xhci: max devices reached\n");
        return;
    }

    // Enable a slot
    int slotId = XhciEnableSlot(ctrl);
    if (slotId < 0) return;

    XhciDevice& dev = g_devices[g_deviceCount];
    dev = {}; // zero-init
    dev.slotId = static_cast<uint32_t>(slotId);
    dev.portNum = portNum;
    dev.portSpeed = portSpeed;
    dev.ep0Cycle = true;
    dev.intCycle = true;

    // Address device
    if (!XhciAddressDevice(ctrl, dev)) return;

    // Get device descriptor
    if (!XhciGetDeviceDescriptor(ctrl, dev)) return;

    // Get config descriptor, parse interfaces, SET_CONFIGURATION
    if (!XhciConfigureDevice(ctrl, dev)) return;

    g_deviceCount++;

    // If it's a HID keyboard, set boot protocol and start polling
    if (dev.isKeyboard && dev.interruptEpAddr) {
        SerialPrintf("xhci: USB keyboard detected on port %u (slot %u)\n",
                     portNum, dev.slotId);

        XhciSetBootProtocol(ctrl, dev);

        if (XhciConfigureInterruptEndpoint(ctrl, dev)) {
            SerialPuts("xhci: keyboard interrupt endpoint ready\n");
        }
    }

    if (dev.isMouse) {
        SerialPrintf("xhci: USB mouse detected on port %u (slot %u)\n",
                     portNum, dev.slotId);
    }
}

// ---------------------------------------------------------------------------
// Port scanning with device enumeration
// ---------------------------------------------------------------------------

static void XhciScanPorts(XhciController& ctrl)
{
    SerialPrintf("xhci: scanning %u ports...\n", ctrl.maxPorts);

    for (uint32_t port = 0; port < ctrl.maxPorts; port++) {
        uint32_t portsc = xhci_read32(ctrl.opBase,
                                       XHCI_OP_PORTSC_BASE + port * 0x10);

        bool connected = (portsc & PORTSC_CCS) != 0;
        bool enabled   = (portsc & PORTSC_PED) != 0;
        bool powered   = (portsc & PORTSC_PP) != 0;
        uint32_t speed = (portsc >> 10) & 0xF;

        SerialPrintf("xhci: port %u: %s %s %s speed=%s (PORTSC=0x%08x)\n",
                     port + 1,
                     connected ? "CONNECTED" : "disconnected",
                     enabled ? "enabled" : "disabled",
                     powered ? "powered" : "unpowered",
                     XhciSpeedString(speed),
                     portsc);

        if (connected && !enabled) {
            // Reset the port to enable it
            SerialPrintf("xhci: resetting port %u...\n", port + 1);

            // Write PORTSC with PR=1, preserving PP, clearing W1C bits
            uint32_t val = (portsc & ~PORTSC_W1C_MASK) | PORTSC_PR;
            xhci_write32(ctrl.opBase, XHCI_OP_PORTSC_BASE + port * 0x10, val);

            // Wait for reset to complete (PRC bit set)
            for (int i = 0; i < 200; i++) {
                portsc = xhci_read32(ctrl.opBase,
                                      XHCI_OP_PORTSC_BASE + port * 0x10);
                if (portsc & PORTSC_PRC) break;
                for (volatile int d = 0; d < 100000; d++);
            }

            // Clear PRC by writing 1
            portsc = xhci_read32(ctrl.opBase,
                                  XHCI_OP_PORTSC_BASE + port * 0x10);
            xhci_write32(ctrl.opBase, XHCI_OP_PORTSC_BASE + port * 0x10,
                         (portsc & ~PORTSC_W1C_MASK) | PORTSC_PRC);

            portsc = xhci_read32(ctrl.opBase,
                                  XHCI_OP_PORTSC_BASE + port * 0x10);
            speed = (portsc >> 10) & 0xF;
            SerialPrintf("xhci: port %u after reset: %s speed=%s\n",
                         port + 1,
                         (portsc & PORTSC_PED) ? "ENABLED" : "disabled",
                         XhciSpeedString(speed));

            // If enabled, enumerate the device
            if (portsc & PORTSC_PED) {
                XhciEnumeratePort(ctrl, port + 1, speed);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Module init / exit
// ---------------------------------------------------------------------------

static int XhciModuleInit()
{
    SerialPuts("xhci: initializing USB 3.0 host controller driver\n");

    // Detect xHCI controllers on PCI bus
    XhciController& ctrl = g_controllers[0];

    if (!XhciPciDetect(ctrl)) {
        SerialPuts("xhci: no controller detected, module inactive\n");
        return 0; // not an error — just no hardware present
    }

    // Reset the controller
    if (!XhciReset(ctrl)) return -1;

    // Allocate command/event rings and DCBAA
    if (!XhciAllocRings(ctrl)) return -1;

    // Start the controller
    if (!XhciStartController(ctrl)) return -1;

    ctrl.initialized = true;
    g_controllerCount = 1;

    // Small delay for port status to settle
    for (volatile int d = 0; d < 1000000; d++);

    // Scan ports and enumerate connected devices
    XhciScanPorts(ctrl);

    // Process any pending port status change events
    while (true) {
        Trb* evt = XhciWaitForEvent(ctrl, 0, 50);
        if (!evt) break;
    }

    // Register USB keyboard input device if a keyboard was found
    for (uint32_t i = 0; i < g_deviceCount; i++) {
        if (g_devices[i].isKeyboard) {
            g_usbKbdOps.poll = XhciKeyboardPoll;
            g_usbKbdDev.ops = &g_usbKbdOps;
            g_usbKbdDev.head = 0;
            g_usbKbdDev.tail = 0;
            InputRegister(&g_usbKbdDev);

            // Allocate DMA buffer for keyboard reports and queue first transfer
            uint64_t kbdBufPhys;
            auto* kbdBuf = static_cast<uint8_t*>(AllocDmaBuffer(1, kbdBufPhys));
            if (kbdBuf) {
                g_devices[i].priv = kbdBuf;
                g_kbdReportBufPhys = kbdBufPhys;
                g_kbdDevIndex = i;
                XhciQueueInterruptIn(ctrl, g_devices[i], kbdBuf, kbdBufPhys, 8);
            }

            SerialPuts("xhci: USB keyboard registered as input device\n");
            break;
        }
    }

    SerialPrintf("xhci: driver initialized (%u controller%s, %u device%s)\n",
                 g_controllerCount, g_controllerCount > 1 ? "s" : "",
                 g_deviceCount, g_deviceCount > 1 ? "s" : "");
    return 0;
}

static void XhciModuleExit()
{
    for (uint32_t i = 0; i < g_controllerCount; i++) {
        XhciController& ctrl = g_controllers[i];
        if (!ctrl.initialized) continue;

        // Stop the controller
        uint32_t cmd = xhci_read32(ctrl.opBase, XHCI_OP_USBCMD);
        xhci_write32(ctrl.opBase, XHCI_OP_USBCMD, cmd & ~USBCMD_RS);
        SerialPrintf("xhci: controller %u stopped\n", i);
    }
    SerialPuts("xhci: driver unloaded\n");
}

#pragma clang diagnostic pop

DECLARE_MODULE("xhci", XhciModuleInit, XhciModuleExit,
               "xHCI USB 3.0 Host Controller driver");
