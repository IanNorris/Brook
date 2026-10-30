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
//   Phase 8: Interrupt-driven I/O (INTx, event ring ISR)

#include "module_abi.h"
#include "usb_block_bytes.h"
#include "usb_ecm_descriptors.h"
#include "net.h"
#include "xhci_ownership.h"
#include "xhci_root_port.h"
#include "xhci_intel_routing.h"
#include "xhci_msi.h"
#include "xhci_completion_queue.h"
#include "sync/kmutex.h"
#include "vfs.h"
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
#include "keyboard.h"
#include "device.h"
#include "irq_wrapper.h"
#include "memory/heap.h"
#include "string.h"

MODULE_IMPORT_SYMBOL(PciFindDevice);
MODULE_IMPORT_SYMBOL(PciFindNextDevice);
MODULE_IMPORT_SYMBOL(PciEnableMemSpace);
MODULE_IMPORT_SYMBOL(PciEnableBusMaster);
MODULE_IMPORT_SYMBOL(PciConfigRead32);
MODULE_IMPORT_SYMBOL(PciConfigRead16);
MODULE_IMPORT_SYMBOL(PciConfigRead8);
MODULE_IMPORT_SYMBOL(PciConfigWrite16);
MODULE_IMPORT_SYMBOL(PciConfigWrite32);
MODULE_IMPORT_SYMBOL(SerialPrintf);
MODULE_IMPORT_SYMBOL(SerialPuts);
MODULE_IMPORT_SYMBOL(KPrintf);
MODULE_IMPORT_SYMBOL(KPuts);
MODULE_IMPORT_SYMBOL(VmmAllocPages);
MODULE_IMPORT_SYMBOL(VmmVirtToPhys);
MODULE_IMPORT_SYMBOL(VmmMapPage);
MODULE_IMPORT_SYMBOL(PmmAllocPages);
MODULE_IMPORT_SYMBOL(IoApicRegisterHandler);
MODULE_IMPORT_SYMBOL(IoApicUnregisterHandler);
MODULE_IMPORT_SYMBOL(ApicGetId);
MODULE_IMPORT_SYMBOL(IdtInstallHandler);
MODULE_IMPORT_SYMBOL(InputRegister);
MODULE_IMPORT_SYMBOL(InputWakeWaiters);
MODULE_IMPORT_SYMBOL(InputNextSequence);
MODULE_IMPORT_SYMBOL(KbdPushChar);
MODULE_IMPORT_SYMBOL(kmalloc);
MODULE_IMPORT_SYMBOL(kfree);
MODULE_IMPORT_SYMBOL(PmmFreePage);
MODULE_IMPORT_SYMBOL(KMutexLock);
MODULE_IMPORT_SYMBOL(KMutexUnlock);
MODULE_IMPORT_SYMBOL(VfsOpen);
MODULE_IMPORT_SYMBOL(VfsClose);
MODULE_IMPORT_SYMBOL(DeviceRegister);
MODULE_IMPORT_SYMBOL(ApicSendEoi);
MODULE_IMPORT_SYMBOL(ApicTickCount);
MODULE_IMPORT_SYMBOL(NetRegisterIf);
MODULE_IMPORT_SYMBOL(NetReceive);
MODULE_IMPORT_SYMBOL(NetWakePollThread);

using namespace brook;


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
static constexpr uint8_t USB_CLASS_HUB      = 9;

// HID subclasses
static constexpr uint8_t USB_HID_SUBCLASS_BOOT = 1;
static constexpr uint8_t USB_HID_PROTOCOL_KEYBOARD = 1;
static constexpr uint8_t USB_HID_PROTOCOL_MOUSE = 2;

// Mass Storage subclass/protocol
static constexpr uint8_t USB_MSC_SUBCLASS_SCSI    = 0x06; // SCSI transparent command set
static constexpr uint8_t USB_MSC_PROTOCOL_BBB     = 0x50; // Bulk-Only (BBB) transport

// Mass Storage class-specific requests
static constexpr uint8_t USB_MSC_REQ_RESET        = 0xFF;
static constexpr uint8_t USB_MSC_REQ_GET_MAX_LUN  = 0xFE;

// Hub descriptor types
static constexpr uint8_t USB_DESC_HUB_USB2 = 0x29;
static constexpr uint8_t USB_DESC_HUB_USB3 = 0x2A;

// Hub class-specific requests (bmRequestType = 0x20/0xA0 for hub, 0x23/0xA3 for port)
static constexpr uint8_t USB_HUB_REQ_GET_STATUS    = 0;
static constexpr uint8_t USB_HUB_REQ_CLEAR_FEATURE = 1;
static constexpr uint8_t USB_HUB_REQ_SET_FEATURE   = 3;
static constexpr uint8_t USB_HUB_REQ_GET_DESCRIPTOR = 6;

// Hub port features
static constexpr uint16_t USB_PORT_FEAT_CONNECTION  = 0;
static constexpr uint16_t USB_PORT_FEAT_ENABLE      = 1;
static constexpr uint16_t USB_PORT_FEAT_RESET       = 4;
static constexpr uint16_t USB_PORT_FEAT_POWER       = 8;
static constexpr uint16_t USB_PORT_FEAT_C_CONNECTION = 16;
static constexpr uint16_t USB_PORT_FEAT_C_ENABLE    = 17;
static constexpr uint16_t USB_PORT_FEAT_C_RESET     = 20;

// Hub port status bits
static constexpr uint16_t USB_PORT_STAT_CONNECTION  = (1 << 0);
static constexpr uint16_t USB_PORT_STAT_ENABLE      = (1 << 1);
static constexpr uint16_t USB_PORT_STAT_RESET       = (1 << 4);
static constexpr uint16_t USB_PORT_STAT_POWER       = (1 << 8);

// Hub descriptor (USB 2.0 §11.23.2.1)
struct UsbHubDescriptor {
    uint8_t  bDescLength;
    uint8_t  bDescriptorType;
    uint8_t  bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t  bPwrOn2PwrGood;      // Time in 2ms intervals from power-on to power-good
    uint8_t  bHubContrCurrent;
} __attribute__((packed));

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
static constexpr uint8_t  XHCI_IRQ_VECTOR = 49;   // IDT vector for xHCI interrupt

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

    XhciCompletionQueue<Trb> completions;
    bool completionFailed;
    bool     initialized;
};

static XhciController g_controllers[MAX_CONTROLLERS];
static uint32_t       g_controllerCount = 0;
static IrqSpinLock g_eventRingLock;
static IrqSpinLock g_commandRingLock;
static volatile bool  g_irqActive = false;  // true once ISR is registered

// ---------------------------------------------------------------------------
// Utility: allocate physically contiguous, page-aligned DMA buffer
// ---------------------------------------------------------------------------

// Allocates 'pages' physically contiguous pages, returns both virtual and
// physical addresses. Memory is zeroed. Returns nullptr on failure.
static void* AllocDmaBuffer(uint32_t pages, uint64_t& outPhys)
{
    if(!pages || pages>16384) return nullptr;
    auto phys=PmmAllocPages(pages,MemTag::Device,KernelPid);
    if(!phys)return nullptr;
    const uint64_t bytes=uint64_t(pages)*4096;
    if(phys.raw()>=(16ull<<30) || bytes>(16ull<<30)-phys.raw()) {
        for(uint32_t i=0;i<pages;++i)PmmFreePage(phys+uint64_t(i)*4096);
        return nullptr;
    }
    outPhys=phys.raw();
    auto* ptr=reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
    memset(ptr,0,bytes);
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
    ctrl.scratchpadCount=XhciScratchpadCount(hcsParams2);

    uint32_t hccParams1 = xhci_read32(ctrl.capBase, XHCI_CAP_HCCPARAMS1);
    ctrl.ctx64 = (hccParams1 & HCC_CSZ) != 0;
    if(!(hccParams1&HCC_AC64)) {
        SerialPuts("xhci: 32-bit DMA controller unsupported by this probe\n");return false;
    }

    // Operational registers are at capBase + capLength
    ctrl.opBase = ctrl.capBase + ctrl.capLength;

    // Doorbell offset
    uint32_t dboff = xhci_read32(ctrl.capBase, XHCI_CAP_DBOFF) & ~0x3;
    if(ctrl.capLength<0x20 || uint32_t(ctrl.capLength)+XHCI_OP_PORTSC_BASE+ctrl.maxPorts*16>65536 ||
       dboff>65536-(ctrl.maxSlots+1)*4) {
        SerialPuts("xhci: invalid operational/doorbell register bounds\n");return false;
    }
    ctrl.dbBase = ctrl.capBase + dboff;

    // Runtime register offset
    uint32_t rtsoff = xhci_read32(ctrl.capBase, XHCI_CAP_RTSOFF) & ~0x1F;
    if(rtsoff>65536-0x40 || !ctrl.maxSlots || !ctrl.maxPorts || !ctrl.maxIntrs) {
        SerialPuts("xhci: invalid runtime/capability bounds\n");return false;
    }
    ctrl.rtBase = ctrl.capBase + rtsoff;

    // Read page size (PAGESIZE register, bit N means 2^(N+12) bytes)
    uint32_t psReg = xhci_read32(ctrl.opBase, XHCI_OP_PAGESIZE);
    if(!(psReg&1)){SerialPuts("xhci: controller lacks 4KiB page support\n");return false;}
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
    uint32_t cmd=xhci_read32(ctrl.opBase,XHCI_OP_USBCMD);
    xhci_write32(ctrl.opBase,XHCI_OP_USBCMD,cmd&~(USBCMD_RS|USBCMD_INTE));
    bool halted=false;
    for(unsigned i=0;i<100;++i) {
        if(xhci_read32(ctrl.opBase,XHCI_OP_USBSTS)&USBSTS_HCH){halted=true;break;}
        for(volatile int d=0;d<100000;++d){}
    }
    if(!halted){SerialPuts("xhci: halt timed out; reset aborted\n");return false;}

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
            uint64_t scratchPhys=0;
            auto* page=AllocDmaBuffer(1,scratchPhys);
            if (!page) {
                SerialPrintf("xhci: scratchpad page %u alloc failed\n", i);
                return false;
            }
            ctrl.scratchpadArray[i] = scratchPhys;
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

static uint64_t XhciSubmitCommand(XhciController& ctrl, uint64_t param,
                               uint32_t status, uint32_t control)
{
    const auto saved=IrqSpinLockAcquire(&g_commandRingLock);
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
    const auto token=ctrl.cmdRingPhys+idx*sizeof(Trb);
    IrqSpinLockRelease(&g_commandRingLock,saved);
    return token;
}

static void XhciPollDrainEvents();
static Trb* XhciWaitForEvent(XhciController& ctrl,uint32_t type,uint32_t timeoutMs,
                             Trb* output=nullptr,uint64_t token=0,
                             const uint64_t* tokens=nullptr,unsigned tokenCount=0)
{
    static Trb discarded; // wildcard drain is used only during single-threaded init
    if(!output)output=&discarded;
    const auto start=ApicTickCount();
    do {
        XhciPollDrainEvents();
        const auto saved=IrqSpinLockAcquire(&g_eventRingLock);
        const bool failed=ctrl.completionFailed;
        bool found=false;
        if(!failed) {
            if(tokens)found=ctrl.completions.TakeAny(type,tokens,tokenCount,*output);
            else found=ctrl.completions.Take(type,token,*output);
        }
        IrqSpinLockRelease(&g_eventRingLock,saved);
        if(found)return output;
        if(failed)return nullptr;
        __asm__ volatile("pause");
    } while(ApicTickCount()-start<timeoutMs);
    return nullptr;
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

struct UsbEcmState;
struct XhciDevice {
    UsbEcmConfiguration ecm;
    UsbEcmState* network;
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

    // Bulk endpoints (for mass storage)
    uint8_t  bulkInAddr;        // bulk IN endpoint address
    uint8_t  bulkOutAddr;       // bulk OUT endpoint address
    uint16_t bulkInMaxPacket;
    uint16_t bulkOutMaxPacket;

    Trb*     bulkInRing;
    uint64_t bulkInRingPhys;
    uint32_t bulkInEnqueue;
    bool     bulkInCycle;
    uint32_t bulkInDci;

    Trb*     bulkOutRing;
    uint64_t bulkOutRingPhys;
    uint32_t bulkOutEnqueue;
    bool     bulkOutCycle;
    uint32_t bulkOutDci;

    bool     configured;
    bool     isKeyboard;
    bool     isMouse;
    bool     isMassStorage;
    bool     isHub;

    // Mass storage state
    KMutex controlLock;
    bool controlQuarantined;
    bool mscQuarantined;       // retained DMA buffers cannot be reused after transport failure
    uint32_t mscTag;            // CBW tag counter
    void* mscCommandBuffer;     // one persistent CBW/CSW page, serialized below
    uint64_t mscCommandBufferPhys;
    uint64_t mscSectorCount;    // total sectors
    uint32_t mscBlockSize;      // bytes per sector (usually 512)

    // Hub state
    uint8_t  hubNumPorts;       // number of downstream ports
    uint16_t hubCharacteristics;
    uint8_t  hubPwrOnDelay;     // bPwrOn2PwrGood * 2ms units
    uint32_t routeString;       // xHCI route string to this device
    uint32_t parentSlotId;      // parent hub's slot (0 = root hub)
    uint8_t  parentPortNum;     // port on parent hub (1-based)
    uint8_t  hubTier;           // 0 = directly on root hub

    void*    priv;          // driver-private (DMA report buffer for HID)
};

static constexpr unsigned ECM_RX_PACKETS=32;
struct UsbEcmState {
    NetIf interface;
    XhciController* controller;XhciDevice* device;
    uint8_t* rx;uint64_t rxPhysical,rxToken;
    uint8_t* notification;uint64_t notificationPhysical,notificationToken;
    bool linkKnown,linkUp;uint64_t notifications,reportTick;uint32_t notifyHeader[2];
    uint8_t notificationSamples[16][16],notificationLengths[16];uint32_t notificationPrinted;
    uint8_t* tx;uint64_t txPhysical;
    struct Tx {uint64_t first,final;uint32_t bytes;bool active,dataDone;} sends[8];
    uint8_t packets[ECM_RX_PACKETS][1514];uint16_t lengths[ECM_RX_PACKETS];uint32_t head,tail;bool rxArmed;
    uint64_t dropped,received,transmitted;bool failed;
    uint32_t faultStatus,faultDci;uint64_t faultToken;
};
static bool g_networkProbe=false;
static bool XhciEcmCompletion(XhciDevice&,const Trb&);
static bool XhciInitEcm(XhciController&,XhciDevice&);

static XhciDevice g_devices[MAX_DEVICES];
static uint32_t   g_deviceCount = 0;
static bool        g_mousePresent = false; // set when a mouse is successfully configured

// ---------------------------------------------------------------------------
// Phase 4: Enable Slot
// ---------------------------------------------------------------------------

static int XhciEnableSlot(XhciController& ctrl)
{
    SerialPuts("xhci: sending Enable Slot command\n");
    const auto commandToken=XhciSubmitCommand(ctrl, 0, 0, TRB_TYPE_ENABLE_SLOT);

    Trb completion;
    Trb* evt = XhciWaitForEvent(ctrl, TRB_TYPE_CMD_COMPLETION, 1000,&completion,commandToken);
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

    // Slot Context DW0: Route String, Speed, Context Entries (1 = just EP0)
    uint32_t speed = XhciSlotSpeed(dev.portSpeed);
    slotCtx[0] = (1 << 27) |           // Context Entries = 1
                 (speed << 20) |        // Speed
                 (dev.routeString & 0xFFFFF); // Route String (bits 19:0)

    // Slot Context DW1: Root Hub Port Number (1-based)
    slotCtx[1] = (dev.portNum << 16); // Root Hub Port Number

    // For devices behind a hub, set the parent hub slot and port
    if (dev.parentSlotId != 0) {
        // DW2 bits 7:0 = Parent Hub Slot ID, bits 15:8 = Parent Port Number
        slotCtx[2] = (dev.parentSlotId & 0xFF) |
                     ((static_cast<uint32_t>(dev.parentPortNum) & 0xFF) << 8);
    }

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

    const auto commandToken=XhciSubmitCommand(ctrl, dev.inputCtxPhys, 0,
                      TRB_TYPE_ADDRESS_DEV | (dev.slotId << 24));

    Trb completion;
    Trb* evt = XhciWaitForEvent(ctrl, TRB_TYPE_CMD_COMPLETION, 2000,&completion,commandToken);
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
    struct ControlGuard {
        KMutex* mutex;
        explicit ControlGuard(KMutex* m):mutex(m){KMutexLock(mutex);}
        ~ControlGuard(){KMutexUnlock(mutex);}
    } guard(&dev.controlLock);
    if(dev.controlQuarantined || dataLen>65536 || (dataLen && (!data || !dataDir)) ||
       dataLen!=setup.wLength || dataLen>65536-(dataPhys&65535))return false;
    uint64_t dataToken=0;
    // 1. Setup TRB (Immediate Data)
    uint32_t idx = dev.ep0Enqueue;
    if(idx>=XFER_RING_SIZE-1) {
        dev.ep0Ring[XFER_RING_SIZE-1].control=TRB_TYPE_LINK|TRB_TC|(dev.ep0Cycle?TRB_CYCLE:0);
        dev.ep0Cycle=!dev.ep0Cycle;idx=0;
    }

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

        dataToken=dev.ep0RingPhys+idx*sizeof(Trb);
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

    const auto statusToken=dev.ep0RingPhys+idx*sizeof(Trb);
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

    bool exact=true;
    if(dataLen) {
        Trb dataCompletion;
        const auto* event=XhciWaitForEvent(ctrl,TRB_TYPE_TRANSFER_EVENT,2000,&dataCompletion,dataToken);
        if(!event || ((event->status>>24)!=TRB_CC_SUCCESS && (event->status>>24)!=TRB_CC_SHORT_PACKET)) {
            dev.controlQuarantined=true;return false;
        }
        exact=(event->status&0xffffff)==0;
    }
    // Wait for transfer event
    Trb completion;
    Trb* evt = XhciWaitForEvent(ctrl, TRB_TYPE_TRANSFER_EVENT, 2000,&completion,statusToken);
    if (!evt) {
        dev.controlQuarantined=true;
        SerialPuts("xhci: control transfer timeout\n");
        return false;
    }

    uint32_t cc = (evt->status >> TRB_CC_SHIFT) & 0xFF;
    if (cc != TRB_CC_SUCCESS) {
        dev.controlQuarantined=true;
        SerialPrintf("xhci: control transfer failed (cc=%u)\n", cc);
        return false;
    }

    return exact;
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

    // ECM may be an alternate configuration (Realtek config 2; QEMU's
    // config value 1 is also its second descriptor). Never select by index.
    for(unsigned index=0;index<dev.numConfigurations && index<8;++index){
        UsbSetupPacket probe{};probe.bmRequestType=0x80;probe.bRequest=USB_REQ_GET_DESCRIPTOR;
        probe.wValue=(USB_DESC_CONFIG<<8)|index;probe.wLength=9;
        if(!XhciControlTransfer(ctrl,dev,probe,buf,9,bufPhys,3))return false;
        const unsigned bytes=unsigned(buf[2])|(unsigned(buf[3])<<8);
        if(bytes<9 || bytes>4096)return false;
        probe.wLength=bytes;
        if(!XhciControlTransfer(ctrl,dev,probe,buf,bytes,bufPhys,3))return false;
        UsbEcmConfiguration ecm{};
        if(!UsbEcmParse(buf,bytes,dev.portSpeed==PORT_SPEED_SUPER,ecm))continue;
        UsbSetupPacket select{};select.bRequest=USB_REQ_SET_CONFIG;select.wValue=ecm.value;
        if(!XhciControlTransfer(ctrl,dev,select,nullptr,0,0,0))return false;
        select={};select.bmRequestType=1;select.bRequest=11;select.wValue=ecm.alternate;select.wIndex=ecm.data;
        if(!XhciControlTransfer(ctrl,dev,select,nullptr,0,0,0))return false;
        dev.ecm=ecm;dev.bulkInAddr=ecm.in;dev.bulkOutAddr=ecm.out;
        dev.bulkInMaxPacket=ecm.inPacket;dev.bulkOutMaxPacket=ecm.outPacket;
        dev.interruptEpAddr=ecm.notify;dev.interruptMaxPacket=ecm.notifyPacket;dev.interruptInterval=ecm.notifyInterval;
        dev.configured=true;
        SerialPrintf("xhci: CDC-ECM config=%u control=%u data=%u alt=%u burst=%u/%u\n",
            ecm.value,ecm.control,ecm.data,ecm.alternate,ecm.inBurst,ecm.outBurst);
        return true;
    }

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

            if (iface->bInterfaceClass == USB_CLASS_MASS_STORAGE &&
                iface->bInterfaceSubClass == USB_MSC_SUBCLASS_SCSI &&
                iface->bInterfaceProtocol == USB_MSC_PROTOCOL_BBB) {
                dev.isMassStorage = true;
            }

            // Hub devices: class 9 at device level or interface level
            if (iface->bInterfaceClass == USB_CLASS_HUB ||
                dev.deviceClass == USB_CLASS_HUB) {
                dev.isHub = true;
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

            // Remember bulk endpoints for mass storage
            if (epType == 2) { // Bulk
                if (isIn) {
                    dev.bulkInAddr = epAddr;
                    dev.bulkInMaxPacket = ep->wMaxPacketSize;
                } else {
                    dev.bulkOutAddr = epAddr;
                    dev.bulkOutMaxPacket = ep->wMaxPacketSize;
                }
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
// Phase 9: USB Hub support
// ---------------------------------------------------------------------------

// Forward declare for hub downstream enumeration
static void XhciEnumeratePort(XhciController& ctrl, uint32_t portNum,
                               uint32_t portSpeed);
static void XhciEnumerateHubDevice(XhciController& ctrl, XhciDevice& hub,
                                    uint8_t hubPort, uint32_t speed);
static bool XhciConfigureInterruptEndpoint(XhciController& ctrl, XhciDevice& dev);
static bool XhciSetBootProtocol(XhciController& ctrl, XhciDevice& dev);
static bool XhciInitMassStorage(XhciController& ctrl, XhciDevice& dev, uint32_t devIndex);

// Read the hub descriptor and initialize hub state
static bool XhciHubInit(XhciController& ctrl, XhciDevice& dev)
{
    uint64_t bufPhys;
    auto* buf = static_cast<uint8_t*>(AllocDmaBuffer(1, bufPhys));
    if (!buf) return false;

    // GET_DESCRIPTOR(HUB) — class-specific request to device
    UsbSetupPacket setup = {};
    setup.bmRequestType = 0xA0; // Device-to-Host, Class, Device
    setup.bRequest = USB_HUB_REQ_GET_DESCRIPTOR;
    setup.wValue = (USB_DESC_HUB_USB2 << 8) | 0;
    setup.wIndex = 0;
    setup.wLength = sizeof(UsbHubDescriptor);

    if (!XhciControlTransfer(ctrl, dev, setup, buf, sizeof(UsbHubDescriptor),
                              bufPhys, 3)) {
        // Try USB3 hub descriptor type
        setup.wValue = (USB_DESC_HUB_USB3 << 8) | 0;
        if (!XhciControlTransfer(ctrl, dev, setup, buf, sizeof(UsbHubDescriptor),
                                  bufPhys, 3)) {
            SerialPuts("xhci: GET_HUB_DESCRIPTOR failed\n");
            return false;
        }
    }

    auto* hubDesc = reinterpret_cast<UsbHubDescriptor*>(buf);
    dev.hubNumPorts = hubDesc->bNbrPorts;
    dev.hubCharacteristics = hubDesc->wHubCharacteristics;
    dev.hubPwrOnDelay = hubDesc->bPwrOn2PwrGood;

    SerialPrintf("xhci: hub has %u ports, pwrOn=%ums, char=0x%04x\n",
                 dev.hubNumPorts, dev.hubPwrOnDelay * 2, dev.hubCharacteristics);

    if (dev.hubNumPorts > 15) dev.hubNumPorts = 15;

    // Power on each downstream port
    for (uint8_t p = 1; p <= dev.hubNumPorts; p++) {
        UsbSetupPacket pwr = {};
        pwr.bmRequestType = 0x23; // Host-to-Device, Class, Other (port)
        pwr.bRequest = USB_HUB_REQ_SET_FEATURE;
        pwr.wValue = USB_PORT_FEAT_POWER;
        pwr.wIndex = p;
        pwr.wLength = 0;

        XhciControlTransfer(ctrl, dev, pwr, nullptr, 0, 0, 0);
    }

    // Wait for power-good (bPwrOn2PwrGood * 2ms)
    // Simple busy-wait using LAPIC tick count (~1ms per tick)
    uint32_t delayMs = dev.hubPwrOnDelay * 2;
    if (delayMs < 100) delayMs = 100; // minimum 100ms for safety
    uint64_t start = brook::ApicTickCount();
    while ((brook::ApicTickCount() - start) < delayMs) {
        __asm__ volatile("pause");
    }

    SerialPuts("xhci: hub ports powered\n");
    return true;
}

// Get port status from a hub
static bool XhciHubGetPortStatus(XhciController& ctrl, XhciDevice& hub,
                                  uint8_t port, uint16_t* status, uint16_t* change)
{
    uint64_t bufPhys;
    auto* buf = static_cast<uint8_t*>(AllocDmaBuffer(1, bufPhys));
    if (!buf) return false;

    UsbSetupPacket setup = {};
    setup.bmRequestType = 0xA3; // Device-to-Host, Class, Other (port)
    setup.bRequest = USB_HUB_REQ_GET_STATUS;
    setup.wValue = 0;
    setup.wIndex = port;
    setup.wLength = 4;

    if (!XhciControlTransfer(ctrl, hub, setup, buf, 4, bufPhys, 3)) {
        SerialPrintf("xhci: GET_PORT_STATUS(port %u) failed\n", port);
        return false;
    }

    *status = buf[0] | (buf[1] << 8);
    *change = buf[2] | (buf[3] << 8);
    return true;
}

// Clear a port feature on a hub
static bool XhciHubClearPortFeature(XhciController& ctrl, XhciDevice& hub,
                                     uint8_t port, uint16_t feature)
{
    UsbSetupPacket setup = {};
    setup.bmRequestType = 0x23; // Host-to-Device, Class, Other (port)
    setup.bRequest = USB_HUB_REQ_CLEAR_FEATURE;
    setup.wValue = feature;
    setup.wIndex = port;
    setup.wLength = 0;

    return XhciControlTransfer(ctrl, hub, setup, nullptr, 0, 0, 0);
}

// Set a port feature on a hub
static bool XhciHubSetPortFeature(XhciController& ctrl, XhciDevice& hub,
                                    uint8_t port, uint16_t feature)
{
    UsbSetupPacket setup = {};
    setup.bmRequestType = 0x23; // Host-to-Device, Class, Other (port)
    setup.bRequest = USB_HUB_REQ_SET_FEATURE;
    setup.wValue = feature;
    setup.wIndex = port;
    setup.wLength = 0;

    return XhciControlTransfer(ctrl, hub, setup, nullptr, 0, 0, 0);
}

// Poll hub ports for newly connected devices
static void XhciHubPollPorts(XhciController& ctrl, XhciDevice& hub)
{
    for (uint8_t p = 1; p <= hub.hubNumPorts; p++) {
        uint16_t status = 0, change = 0;
        if (!XhciHubGetPortStatus(ctrl, hub, p, &status, &change))
            continue;

        // Check for connection status change
        if (!(change & USB_PORT_STAT_CONNECTION))
            continue;

        // Clear the change bit
        XhciHubClearPortFeature(ctrl, hub, p, USB_PORT_FEAT_C_CONNECTION);

        if (status & USB_PORT_STAT_CONNECTION) {
            // New device connected — reset the port
            SerialPrintf("xhci: hub port %u: device connected, resetting\n", p);
            XhciHubSetPortFeature(ctrl, hub, p, USB_PORT_FEAT_RESET);

            // Wait for reset complete (~50ms)
            uint64_t start = brook::ApicTickCount();
            while ((brook::ApicTickCount() - start) < 50) {
                __asm__ volatile("pause");
            }

            // Clear reset change
            XhciHubClearPortFeature(ctrl, hub, p, USB_PORT_FEAT_C_RESET);

            // Check port is now enabled
            uint16_t newStatus = 0, newChange = 0;
            if (!XhciHubGetPortStatus(ctrl, hub, p, &newStatus, &newChange))
                continue;

            if (!(newStatus & USB_PORT_STAT_ENABLE)) {
                SerialPrintf("xhci: hub port %u: reset failed (not enabled)\n", p);
                continue;
            }

            // Determine speed from port status (bits 9-10 for USB 2.0)
            uint32_t speed = (newStatus >> 9) & 0x3;
            // Map hub port speed encoding to xHCI speed:
            // 0=full, 1=low, 2=high; xHCI: 1=full, 2=low, 3=high, 4=super
            uint32_t xhciSpeed;
            switch (speed) {
                case 0: xhciSpeed = 1; break; // Full Speed
                case 1: xhciSpeed = 2; break; // Low Speed
                case 2: xhciSpeed = 3; break; // High Speed
                default: xhciSpeed = 4; break; // Super Speed
            }

            SerialPrintf("xhci: hub port %u: enabled, speed=%u\n", p, xhciSpeed);

            // Enumerate the downstream device
            XhciEnumerateHubDevice(ctrl, hub, p, xhciSpeed);
        } else {
            // Device disconnected
            SerialPrintf("xhci: hub port %u: device disconnected\n", p);
            // TODO: find and clean up the device slot
        }
    }
}

// Enumerate a device connected through a hub
static void XhciEnumerateHubDevice(XhciController& ctrl, XhciDevice& hub,
                                    uint8_t hubPort, uint32_t speed)
{
    if (g_deviceCount >= MAX_DEVICES) {
        SerialPuts("xhci: max devices reached\n");
        return;
    }

    int slotId = XhciEnableSlot(ctrl);
    if (slotId < 0) return;

    XhciDevice& dev = g_devices[g_deviceCount];
    dev = {};
    dev.slotId = static_cast<uint32_t>(slotId);
    dev.portNum = hub.portNum; // root hub port number (for xHCI slot context)
    dev.portSpeed = speed;
    dev.ep0Cycle = true;
    dev.intCycle = true;
    dev.parentSlotId = hub.slotId;
    dev.parentPortNum = hubPort;
    dev.hubTier = hub.hubTier + 1;

    // Build route string: append hubPort to parent's route string
    // Route string: 4 bits per tier, tier 0 in bits 3:0
    dev.routeString = hub.routeString | (static_cast<uint32_t>(hubPort) << (hub.hubTier * 4));

    SerialPrintf("xhci: enumerating hub device: rootPort=%u, route=0x%05x, tier=%u\n",
                 dev.portNum, dev.routeString, dev.hubTier);

    if (!XhciAddressDevice(ctrl, dev)) return;
    if (!XhciGetDeviceDescriptor(ctrl, dev)) return;
    if (!XhciConfigureDevice(ctrl, dev)) return;

    g_deviceCount++;

    // Handle device type same as root port devices
    if (dev.isKeyboard && dev.interruptEpAddr) {
        SerialPrintf("xhci: USB keyboard via hub (slot %u)\n", dev.slotId);
        XhciSetBootProtocol(ctrl, dev);
        XhciConfigureInterruptEndpoint(ctrl, dev);
    }

    if (dev.isMouse && dev.interruptEpAddr) {
        SerialPrintf("xhci: USB mouse via hub (slot %u)\n", dev.slotId);
        XhciSetBootProtocol(ctrl, dev);
        if (XhciConfigureInterruptEndpoint(ctrl, dev))
            g_mousePresent = true;
    }

    if(dev.ecm.value)XhciInitEcm(ctrl,dev);
    if (dev.isMassStorage && dev.bulkInAddr && dev.bulkOutAddr) {
        SerialPrintf("xhci: USB mass storage via hub (slot %u)\n", dev.slotId);
        XhciInitMassStorage(ctrl, dev, g_deviceCount - 1);
    }

    if (dev.isHub) {
        SerialPrintf("xhci: nested hub detected (slot %u)\n", dev.slotId);
        if (dev.hubTier < 5) {
            XhciHubInit(ctrl, dev);
            XhciHubPollPorts(ctrl, dev);
        } else {
            SerialPuts("xhci: max hub tier depth reached\n");
        }
    }
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
    memset(input, 0, ctxSize * 33);

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
    if(dev.intDci>((slotCtx[0]>>27)&31))slotCtx[0] = (slotCtx[0] & ~(0x1F << 27)) | (dev.intDci << 27);

    // Endpoint Context for the interrupt IN endpoint
    auto* epCtx = reinterpret_cast<uint32_t*>(input + ctxSize * (dev.intDci + 1));

    unsigned interval=dev.interruptInterval;
    if(!interval)return false;
    if(dev.portSpeed==PORT_SPEED_HIGH || dev.portSpeed==PORT_SPEED_SUPER){if(interval>16)return false;--interval;}
    else{unsigned exponent=0;while(interval>1){interval>>=1;++exponent;}interval=exponent+3;}
    epCtx[0]=interval<<16;
    // EP DW1: CErr=3, EP Type=7 (Interrupt IN), Max Packet Size
    epCtx[1] = (3 << 1) |      // CErr = 3
               (7 << 3) |      // EP Type = Interrupt IN
               (dev.interruptMaxPacket << 16);

    // EP DW2-3: TR Dequeue Pointer with DCS=1
    uint64_t trDqp = dev.intRingPhys | 1;
    epCtx[2] = static_cast<uint32_t>(trDqp);
    epCtx[3] = static_cast<uint32_t>(trDqp >> 32);

    // EP DW4: Average TRB Length (8 for keyboard), Max ESIT Payload (8)
    epCtx[4] = dev.interruptMaxPacket | (uint32_t(dev.interruptMaxPacket) << 16);

    __asm__ volatile("mfence" ::: "memory");

    // Submit Configure Endpoint command
    const auto commandToken=XhciSubmitCommand(ctrl, dev.inputCtxPhys, 0,
                      TRB_TYPE_CONFIG_EP | (dev.slotId << 24));

    Trb completion;
    Trb* evt = XhciWaitForEvent(ctrl, TRB_TYPE_CMD_COMPLETION, 2000,&completion,commandToken);
    if (!evt) {
        SerialPuts("xhci: Configure Endpoint timeout\n");
        return false;
    }

    uint32_t cc = (evt->status >> TRB_CC_SHIFT) & 0xFF;
    if (cc != TRB_CC_SUCCESS) {
        SerialPrintf("xhci: Configure Endpoint failed (cc=%u)\n", cc);
        return false;
    }

    SerialPrintf("xhci: interrupt endpoint configured (DCI %u) ring=0x%lx\n",
                 dev.intDci, dev.intRingPhys);
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

// PS/2 scancode set 1 → ASCII (US layout, unshifted)
// Matches keyboard.cpp g_scancodeToAscii_US for the common range.
static const char g_sc1ToAscii[128] = {
    0,0x1B,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',  // 0x00
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,            // 0x10
    'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\',           // 0x1E
    'z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',               // 0x2C
    0,0,0,0,0,0,0,0,0,0,0,0,                                           // 0x3A (F1-F10,NumLk,ScrLk)
    '7','8','9','-','4','5','6','+','1','2','3','0','.',                // 0x47 (KP)
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0            // 0x54-0x7F
};

// PS/2 scancode set 1 → ASCII (US layout, shifted)
static const char g_sc1ToAsciiShift[128] = {
    0,0x1B,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',  // 0x00
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,            // 0x10
    'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',             // 0x1E
    'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' ',               // 0x2C
    0,0,0,0,0,0,0,0,0,0,0,0,                                           // 0x3A
    '7','8','9','-','4','5','6','+','1','2','3','0','.',                // 0x47
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0            // 0x54-0x7F
};

static void XhciPushKey(uint8_t scancode, bool pressed, uint8_t modifiers)
{
    InputEvent ev{};
    ev.type = pressed ? InputEventType::KeyPress : InputEventType::KeyRelease;
    ev.scanCode = scancode;
    ev.modifiers = modifiers;

    // Translate scancode → ASCII so the compositor/terminal can use it
    ev.ascii = 0;
    if (pressed && scancode < 128) {
        bool shifted = (modifiers & (INPUT_MOD_LSHIFT | INPUT_MOD_RSHIFT)) != 0;
        bool capsLock = false; // TODO: track caps lock state
        char c = shifted ? g_sc1ToAsciiShift[scancode] : g_sc1ToAscii[scancode];
        // CapsLock inverts letter case
        if (capsLock && c >= 'A' && c <= 'Z' && !shifted)
            c = g_sc1ToAscii[scancode]; // already lowercase
        ev.ascii = static_cast<uint8_t>(c);
    }

    InputDevicePush(&g_usbKbdDev, ev);

    // Also push to the legacy KbdGetChar ring buffer so the kernel shell
    // (which uses KbdGetChar, not InputPollEvent) receives USB keyboard input.
    if (pressed && ev.ascii != 0)
        KbdPushChar(static_cast<char>(ev.ascii));
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
static uint64_t g_mouseReportBufPhys = 0;
static uint32_t g_mouseDevIndex = 0;

// Forward declarations — defined later in this file
static int XhciMatchTransferEvent(const Trb& evt);
static void XhciProcessMouseReport(const uint8_t* report);

// Poll for USB keyboard events — called by the input subsystem.
// In interrupt-driven mode (g_irqActive), the ISR handles reports
// directly, so this is a no-op. In polling mode, it checks for events.

// Unified event drain — processes all pending events on the event ring
// and routes transfer completions to the correct device. Called from
// both keyboard and mouse poll paths.
static void XhciProcessMouseReport(const uint8_t* report);
static void XhciPollDrainEvents()
{
    if(!g_controllerCount)return;
    auto& ctrl=g_controllers[0];if(!ctrl.initialized)return;
    const auto saved=IrqSpinLockAcquire(&g_eventRingLock);
    unsigned processed=0;bool wakeNetwork=false;
    while(processed<EVT_RING_SIZE) {
        const auto& source=ctrl.evtRing[ctrl.evtDequeue];
        if(bool(source.control&TRB_CYCLE)!=ctrl.evtCycle)break;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        const Trb event=source;
        if(++ctrl.evtDequeue==EVT_RING_SIZE){ctrl.evtDequeue=0;ctrl.evtCycle=!ctrl.evtCycle;}
        ++processed;
        const uint32_t type=event.control&TRB_TYPE_MASK;
        if(type!=TRB_TYPE_TRANSFER_EVENT && type!=TRB_TYPE_CMD_COMPLETION)continue;
        bool handled=false;
        if(type==TRB_TYPE_TRANSFER_EVENT) {
            const int index=XhciMatchTransferEvent(event);
            if(index>=0) {
                auto& dev=g_devices[index];
                const uint32_t dci=(event.control>>16)&31,slot=event.control>>24;
                const uint32_t cc=event.status>>24;
                const bool interrupt=dev.intRingPhys && event.param>=dev.intRingPhys &&
                    event.param<dev.intRingPhys+XFER_RING_SIZE*sizeof(Trb) && dci==dev.intDci && slot==dev.slotId;
                if(dev.network && XhciEcmCompletion(dev,event)){handled=true;wakeNetwork=true;}
                if(interrupt && (dev.isKeyboard || dev.isMouse)) {
                    handled=true;
                    if((cc==TRB_CC_SUCCESS || cc==TRB_CC_SHORT_PACKET) && dev.priv) {
                        const uint32_t residue=event.status&0xffffff;
                        if(dev.isKeyboard && !residue)XhciProcessKeyboardReport(static_cast<uint8_t*>(dev.priv));
                        else if(dev.isMouse && residue<=1) {
                            if(residue)static_cast<uint8_t*>(dev.priv)[3]=0;
                            XhciProcessMouseReport(static_cast<uint8_t*>(dev.priv));
                        }
                        XhciQueueInterruptIn(ctrl,dev,dev.priv,dev.isKeyboard?g_kbdReportBufPhys:g_mouseReportBufPhys,dev.isKeyboard?8:4);
                    }
                }
            }
        }
        if(!handled && !ctrl.completions.Push(event))ctrl.completionFailed=true;
    }
    if(processed) {
        xhci_write64(ctrl.rtBase,XHCI_RT_ERDP,ctrl.evtRingPhys+ctrl.evtDequeue*sizeof(Trb)|(1ull<<3));
        const auto sts=xhci_read32(ctrl.opBase,XHCI_OP_USBSTS);
        if(sts&USBSTS_EINT)xhci_write32(ctrl.opBase,XHCI_OP_USBSTS,USBSTS_EINT);
        const auto iman=xhci_read32(ctrl.rtBase,XHCI_RT_IMAN);
        xhci_write32(ctrl.rtBase,XHCI_RT_IMAN,iman|1); // ack IP, preserve interrupt enable
    }
    IrqSpinLockRelease(&g_eventRingLock,saved);
    if(wakeNetwork)NetWakePollThread();
}

static void XhciKeyboardPoll(InputDevice* /*dev*/)
{
    XhciPollDrainEvents();
}

// ---------------------------------------------------------------------------
// USB mouse support
// ---------------------------------------------------------------------------

// USB HID boot mouse report: 3 bytes minimum
// Byte 0: buttons (bit0=left, bit1=right, bit2=middle)
// Byte 1: X displacement (signed int8)
// Byte 2: Y displacement (signed int8)

MODULE_IMPORT_SYMBOL(MouseGetPosition);
MODULE_IMPORT_SYMBOL(MouseSetPosition);
MODULE_IMPORT_SYMBOL(MouseSetButtons);
MODULE_IMPORT_SYMBOL(MouseSetAvailable);

extern "C" void MouseGetPosition(int32_t* x, int32_t* y);
extern "C" void MouseSetPosition(int32_t x, int32_t y);
extern "C" void MouseSetButtons(uint8_t buttons);
extern "C" void MouseSetAvailable(bool available);

static InputDevice g_usbMouseDev;
static InputDeviceOps g_usbMouseOps = { "usb_mouse", nullptr };

static void XhciProcessMouseReport(const uint8_t* report)
{
    uint8_t buttons = report[0] & 0x07;
    int8_t  dx = static_cast<int8_t>(report[1]);
    int8_t  dy = static_cast<int8_t>(report[2]);

    // Update global mouse position via the kernel mouse API
    if (dx != 0 || dy != 0) {
        int32_t mx = 0, my = 0;
        MouseGetPosition(&mx, &my);
        // USB boot protocol: positive Y = down (screen convention).
        // No inversion needed — unlike PS/2 where positive Y = up.
        MouseSetPosition(mx + dx, my + dy);

        InputEvent ev{};
        ev.type     = InputEventType::MouseRelativeMove;
        ev.deltaX = dx;
        ev.deltaY = dy;
        ev.scanCode = buttons;
        ev.ascii    = 0;
        ev.modifiers = 0;
        InputDevicePush(&g_usbMouseDev, ev);
    }

    // Update buttons through the kernel mouse API
    MouseSetButtons(buttons);

    // Push button change events
    static uint8_t prevButtons = 0;
    for (int btn = 0; btn < 3; btn++) {
        bool now  = (buttons & (1 << btn)) != 0;
        bool prev = (prevButtons & (1 << btn)) != 0;
        if (now != prev) {
            InputEvent ev{};
            ev.type = now ? InputEventType::MouseButtonDown : InputEventType::MouseButtonUp;
            ev.scanCode = static_cast<uint8_t>(btn);
            ev.ascii = 0;
            ev.modifiers = 0;
            InputDevicePush(&g_usbMouseDev, ev);
        }
    }
    prevButtons = buttons;

    InputWakeWaiters();
}

static void XhciMousePoll(InputDevice* /*dev*/)
{
    XhciPollDrainEvents();
}

// ---------------------------------------------------------------------------
// Phase 8: Bulk transfer support for USB Mass Storage
// ---------------------------------------------------------------------------

// Configure a bulk endpoint in the xHCI (IN or OUT)
static bool XhciConfigureBulkEndpoint(XhciController& ctrl, XhciDevice& dev,
                                       uint8_t epAddr, uint16_t maxPacket,
                                       Trb*& ring, uint64_t& ringPhys,
                                       uint32_t& enqueue, bool& cycle,
                                       uint32_t& dci,uint8_t maxBurst=0)
{
    uint8_t epNum = epAddr & 0x0F;
    bool isIn = (epAddr & 0x80) != 0;
    dci = epNum * 2 + (isIn ? 1 : 0);

    SerialPrintf("xhci: configuring bulk EP 0x%02x (DCI %u)\n", epAddr, dci);

    if (!XhciAllocTransferRing(ring, ringPhys, enqueue, cycle)) {
        SerialPuts("xhci: failed to allocate bulk transfer ring\n");
        return false;
    }

    uint32_t ctxSize = CtxEntrySize(ctrl);

    auto* input = static_cast<uint8_t*>(dev.inputCtx);
    memset(input, 0, ctxSize * 33);

    auto* icc = reinterpret_cast<uint32_t*>(input);
    icc[1] = (1 << 0) | (1 << dci); // Add Slot + EP

    // Copy slot context from output
    auto* slotCtx = reinterpret_cast<uint32_t*>(input + ctxSize);
    auto* outSlot = reinterpret_cast<uint32_t*>(
        static_cast<uint8_t*>(dev.outputCtx));
    slotCtx[0] = outSlot[0];
    slotCtx[1] = outSlot[1];
    slotCtx[2] = outSlot[2];
    slotCtx[3] = outSlot[3];

    // Update Context Entries to include this DCI if it's the highest
    uint32_t curEntries = (slotCtx[0] >> 27) & 0x1F;
    if (dci > curEntries)
        slotCtx[0] = (slotCtx[0] & ~(0x1F << 27)) | (dci << 27);

    // Endpoint Context
    auto* epCtx = reinterpret_cast<uint32_t*>(input + ctxSize * (dci + 1));

    // EP Type: 2 = Bulk OUT, 6 = Bulk IN
    uint32_t epType = isIn ? 6 : 2;
    epCtx[1] = (3 << 1) |            // CErr = 3
               (epType << 3) |       // EP Type
               (uint32_t(maxBurst)<<8) |
               (maxPacket << 16);     // Max Packet Size

    // TR Dequeue Pointer with DCS=1
    uint64_t trDqp = ringPhys | 1;
    epCtx[2] = static_cast<uint32_t>(trDqp);
    epCtx[3] = static_cast<uint32_t>(trDqp >> 32);

    // Average TRB Length = max packet, Max ESIT = 0 for bulk
    epCtx[4] = maxPacket;

    __asm__ volatile("mfence" ::: "memory");

    const auto commandToken=XhciSubmitCommand(ctrl, dev.inputCtxPhys, 0,
                      TRB_TYPE_CONFIG_EP | (dev.slotId << 24));

    Trb completion;
    Trb* evt = XhciWaitForEvent(ctrl, TRB_TYPE_CMD_COMPLETION, 2000,&completion,commandToken);
    if (!evt) {
        SerialPuts("xhci: Configure Bulk EP timeout\n");
        return false;
    }

    uint32_t cc = (evt->status >> TRB_CC_SHIFT) & 0xFF;
    if (cc != TRB_CC_SUCCESS) {
        SerialPrintf("xhci: Configure Bulk EP failed (cc=%u)\n", cc);
        return false;
    }

    SerialPrintf("xhci: bulk endpoint 0x%02x configured (DCI %u)\n", epAddr, dci);
    return true;
}

// ECM owns bounded DMA slots for the lifetime of this controller. Idle IN
// requests stay pending; there is no timeout on ordinary absence of traffic.
static uint64_t XhciEcmTrb(XhciDevice& dev,bool input,uint64_t physical,uint32_t bytes,bool chain,bool publish=true){
    auto* ring=input?dev.bulkInRing:dev.bulkOutRing;
    auto& enqueue=input?dev.bulkInEnqueue:dev.bulkOutEnqueue;
    auto& cycle=input?dev.bulkInCycle:dev.bulkOutCycle;
    const auto ringPhysical=input?dev.bulkInRingPhys:dev.bulkOutRingPhys;
    if(enqueue>=XFER_RING_SIZE-1){
        ring[XFER_RING_SIZE-1].control=TRB_TYPE_LINK|TRB_TC|(ring[XFER_RING_SIZE-2].control&(1u<<4))|(cycle?TRB_CYCLE:0);
        cycle=!cycle;enqueue=0;
    }
    auto& trb=ring[enqueue];const auto token=ringPhysical+enqueue*sizeof(Trb);
    trb.param=physical;trb.status=bytes;
    const auto control=TRB_TYPE_NORMAL|(input?(1u<<2):0)|(chain?(1u<<4):TRB_IOC)|(cycle?TRB_CYCLE:0);
    __atomic_store_n(&trb.control,control^(publish?0u:TRB_CYCLE),__ATOMIC_RELEASE);
    ++enqueue;return token;
}
static void XhciEcmReceiveArm(UsbEcmState& net){
    net.rxToken=XhciEcmTrb(*net.device,true,net.rxPhysical,2048,false);net.rxArmed=true;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    XhciRingDoorbell(*net.controller,net.device->slotId,net.device->bulkInDci);
}
static void XhciEcmNotificationArm(UsbEcmState& net){
    auto& dev=*net.device;
    const auto index=dev.intEnqueue>=XFER_RING_SIZE-1?0:dev.intEnqueue;
    net.notificationToken=dev.intRingPhys+index*sizeof(Trb);
    XhciQueueInterruptIn(*net.controller,dev,net.notification,net.notificationPhysical,dev.ecm.notifyPacket);
}
// Called only by the shared event consumer while g_eventRingLock is held.
static bool XhciEcmCompletion(XhciDevice& dev,const Trb& event){
    auto& net=*dev.network;
    const unsigned dci=(event.control>>16)&31,slot=event.control>>24;
    if(slot!=dev.slotId)return false;
    const unsigned cc=event.status>>24,residue=event.status&0xffffff;
    const auto fail=[&]{if(!net.failed){net.faultStatus=event.status;net.faultDci=dci;net.faultToken=event.param;net.failed=true;}};
    if(net.failed && (dci==dev.intDci || dci==dev.bulkInDci || dci==dev.bulkOutDci))return true;
    if(dci==dev.intDci){
        if(net.failed)return true;
        if(event.param!=net.notificationToken || (cc!=TRB_CC_SUCCESS && cc!=TRB_CC_SHORT_PACKET) || residue>dev.ecm.notifyPacket){fail();return true;}
        const auto bytes=dev.ecm.notifyPacket-residue;
        if(net.notifications<16){
            const auto length=bytes<16?bytes:16;
            net.notificationLengths[net.notifications]=length;
            memcpy(net.notificationSamples[net.notifications],net.notification,length);
        }
        ++net.notifications;
        const auto* b=net.notification;
        if(bytes>=8)memcpy(net.notifyHeader,b,8);
        if(bytes>=8 && b[0]==0xa1 && b[1]==0 && (b[4]==dev.ecm.control || b[4]==dev.ecm.data) && !b[5] && !b[6] && !b[7]){
            net.linkKnown=true;net.linkUp=b[2]!=0 || b[3]!=0;
        }
        XhciEcmNotificationArm(net);return true;
    }
    if(dci==dev.bulkInDci){
        if(net.failed)return true;
        if(event.param!=net.rxToken || (cc!=TRB_CC_SUCCESS && cc!=TRB_CC_SHORT_PACKET) || residue>2048){fail();return true;}
        net.rxArmed=false;
        const unsigned bytes=2048-residue;
        if(bytes>=14 && bytes<=1514){
            if(net.tail-net.head<ECM_RX_PACKETS){const auto index=net.tail%ECM_RX_PACKETS;memcpy(net.packets[index],net.rx,bytes);net.lengths[index]=bytes;++net.tail;++net.received;}
            else ++net.dropped;
        }else if(bytes)++net.dropped;
        // A full software queue backpressures USB instead of discarding a
        // completed packet. The poll consumer rearms after making space.
        if(net.tail-net.head<ECM_RX_PACKETS)XhciEcmReceiveArm(net);
        return true;
    }
    if(dci==dev.bulkOutDci){
        for(auto& tx:net.sends)if(tx.active && (event.param==tx.final || event.param==tx.first)){
            if(cc!=TRB_CC_SUCCESS || residue){fail();return true;}
            if(tx.first!=tx.final && event.param==tx.first){tx.dataDone=true;return true;}
            if(tx.first!=tx.final && !tx.dataDone){fail();return true;}
            tx.active=false;++net.transmitted;return true;
        }
        fail();return true;
    }
    return false;
}
static int XhciEcmTransmit(NetIf* nif,const void* frame,uint32_t bytes){
    if(!frame || bytes<14 || bytes>1514)return -22;
    auto& net=*static_cast<UsbEcmState*>(nif->driverPriv);auto& dev=*net.device;
    const auto saved=IrqSpinLockAcquire(&g_eventRingLock);
    if(net.failed || net.controller->completionFailed){IrqSpinLockRelease(&g_eventRingLock,saved);return -5;}
    for(unsigned i=0;i<8;++i){auto& tx=net.sends[i];if(tx.active)continue;
        auto* buffer=net.tx+i*2048;memcpy(buffer,frame,bytes);
        const auto physical=net.txPhysical+i*2048;
        const bool terminate=(bytes%dev.bulkOutMaxPacket)==0;
        // A zero-length TRB chained into the data TD is not a USB ZLP:
        // controllers may merge the TD into one exact-size USB transfer.
        // Termination needs its own zero-length TD and completion ownership.
        tx.first=XhciEcmTrb(dev,false,physical,bytes,false,!terminate);
        tx.final=terminate?XhciEcmTrb(dev,false,physical,0,false):tx.first;
        tx.bytes=bytes;tx.dataDone=false;tx.active=true;
        if(terminate){
            auto& head=dev.bulkOutRing[(tx.first-dev.bulkOutRingPhys)/sizeof(Trb)];
            __atomic_fetch_xor(&head.control,TRB_CYCLE,__ATOMIC_RELEASE);
        }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        XhciRingDoorbell(*net.controller,dev.slotId,dev.bulkOutDci);
        IrqSpinLockRelease(&g_eventRingLock,saved);return 0;
    }
    IrqSpinLockRelease(&g_eventRingLock,saved);return -11;
}
static void XhciEcmPoll(NetIf* nif){
    auto& net=*static_cast<UsbEcmState*>(nif->driverPriv);
    XhciPollDrainEvents();
    const auto now=ApicTickCount();
    if(g_networkProbe && now-net.reportTick>=1000){
        const auto saved=IrqSpinLockAcquire(&g_eventRingLock);
        const auto rx=net.received,tx=net.transmitted,notifications=net.notifications,dropped=net.dropped;
        const auto status=net.faultStatus,dci=net.faultDci;const auto token=net.faultToken;
        const auto notification0=net.notifyHeader[0],notification1=net.notifyHeader[1];
        const bool failed=net.failed,known=net.linkKnown,up=net.linkUp;net.reportTick=now;
        IrqSpinLockRelease(&g_eventRingLock,saved);
        SerialPrintf("xhci: ECM probe link=%s rx=%lu tx=%lu notify=%lu dropped=%lu failed=%u last=%u/%08x/%lx header=%08x/%08x\n",
            known?(up?"up":"down"):"unknown",rx,tx,notifications,dropped,failed,dci,status,token,notification0,notification1);
    }
    if(g_networkProbe){
        for(unsigned n=0;n<16;++n){
            uint8_t sample[16]{};unsigned length=0,index=0;
            const auto saved=IrqSpinLockAcquire(&g_eventRingLock);
            if(net.notificationPrinted<16 && net.notificationPrinted<net.notifications){
                index=net.notificationPrinted++;length=net.notificationLengths[index];
                memcpy(sample,net.notificationSamples[index],length);
            }
            IrqSpinLockRelease(&g_eventRingLock,saved);
            if(!length)break;
            uint32_t words[4]{};memcpy(words,sample,length);
            SerialPrintf("xhci: ECM notification %u bytes=%u data=%08x/%08x/%08x/%08x\n",
                index,length,words[0],words[1],words[2],words[3]);
        }
    }
    for(unsigned n=0;n<8;++n){
        uint8_t frame[1514];unsigned bytes=0;
        const auto saved=IrqSpinLockAcquire(&g_eventRingLock);
        if(net.head!=net.tail){const auto index=net.head%ECM_RX_PACKETS;bytes=net.lengths[index];memcpy(frame,net.packets[index],bytes);++net.head;}
        if(!net.failed && !net.rxArmed && net.tail-net.head<ECM_RX_PACKETS)XhciEcmReceiveArm(net);
        IrqSpinLockRelease(&g_eventRingLock,saved);
        if(!bytes)break;
        // Receive can immediately transmit an ACK. No USB lock is held here.
        NetReceive(nif,frame,bytes);
    }
}
static bool XhciInitEcm(XhciController& ctrl,XhciDevice& dev){
    if(Vnode* marker=VfsOpen("/etc/usb-net-probe",0)){VfsClose(marker);g_networkProbe=true;}
    auto* net=static_cast<UsbEcmState*>(kmalloc(sizeof(UsbEcmState)));if(!net)return false;
    memset(net,0,sizeof(*net));net->controller=&ctrl;net->device=&dev;
    net->rx=static_cast<uint8_t*>(AllocDmaBuffer(1,net->rxPhysical));
    net->tx=static_cast<uint8_t*>(AllocDmaBuffer(4,net->txPhysical));
    net->notification=static_cast<uint8_t*>(AllocDmaBuffer(1,net->notificationPhysical));
    uint64_t physical=0;auto* scratch=static_cast<uint8_t*>(AllocDmaBuffer(1,physical));
    if(!net->rx || !net->tx || !net->notification || !scratch)return false;
    // String 0 gives the supported language. Read exact descriptor lengths,
    // because the control path deliberately rejects unexplained short data.
    UsbSetupPacket request{};request.bmRequestType=0x80;request.bRequest=USB_REQ_GET_DESCRIPTOR;
    request.wValue=3u<<8;request.wLength=2;
    if(!XhciControlTransfer(ctrl,dev,request,scratch,2,physical,3))return false;
    unsigned length=scratch[0];if(length<4 || length>254 || (length&1))return false;
    request.wLength=length;
    if(!XhciControlTransfer(ctrl,dev,request,scratch,length,physical,3))return false;
    unsigned language=unsigned(scratch[2])|(unsigned(scratch[3])<<8);
    request.wValue=(3u<<8)|dev.ecm.macString;request.wIndex=language;request.wLength=26;
    if(!XhciControlTransfer(ctrl,dev,request,scratch,26,physical,3) || !UsbEcmMac(scratch,26,net->interface.mac.b))return false;
    if(!XhciConfigureBulkEndpoint(ctrl,dev,dev.bulkInAddr,dev.bulkInMaxPacket,dev.bulkInRing,dev.bulkInRingPhys,dev.bulkInEnqueue,dev.bulkInCycle,dev.bulkInDci,dev.ecm.inBurst))return false;
    if(!XhciConfigureBulkEndpoint(ctrl,dev,dev.bulkOutAddr,dev.bulkOutMaxPacket,dev.bulkOutRing,dev.bulkOutRingPhys,dev.bulkOutEnqueue,dev.bulkOutCycle,dev.bulkOutDci,dev.ecm.outBurst))return false;
    if(!XhciConfigureInterruptEndpoint(ctrl,dev))return false;
    request={};request.bmRequestType=0x21;request.bRequest=0x43;request.wValue=0x000c;request.wIndex=dev.ecm.control;

    if(!XhciControlTransfer(ctrl,dev,request,nullptr,0,0,0))return false;
    net->interface.driverPriv=net;net->interface.transmit=XhciEcmTransmit;net->interface.poll=XhciEcmPoll;
    const auto saved=IrqSpinLockAcquire(&g_eventRingLock);
    dev.network=net;XhciEcmReceiveArm(*net);XhciEcmNotificationArm(*net);
    IrqSpinLockRelease(&g_eventRingLock,saved);
    NetRegisterIf(&net->interface);
    SerialPrintf("xhci: CDC-ECM network interface ready slot=%u (carrier unverified)\n",dev.slotId);
    return true;
}

// Perform a bulk transfer (IN or OUT)
static bool XhciBulkTransfer(XhciController& ctrl, XhciDevice& dev,
                              bool isIn, void* buf, uint64_t bufPhys,
                              uint32_t len, uint32_t timeoutMs = 5000)
{
    Trb*&     ring    = isIn ? dev.bulkInRing    : dev.bulkOutRing;
    uint32_t& enqueue = isIn ? dev.bulkInEnqueue : dev.bulkOutEnqueue;
    bool&     cycle   = isIn ? dev.bulkInCycle   : dev.bulkOutCycle;
    uint32_t  dci     = isIn ? dev.bulkInDci     : dev.bulkOutDci;

    const uint64_t ringPhys=isIn?dev.bulkInRingPhys:dev.bulkOutRingPhys;
    if (!ring) return false;

    if(!len || len>65536)return false;
    uint64_t physical=bufPhys,token=0,tokens[3]{};unsigned tokenCount=0;uint32_t remaining=len;
    const uint32_t maxPacket=isIn?dev.bulkInMaxPacket:dev.bulkOutMaxPacket;
    if(!maxPacket)return false;
    Trb* first=nullptr;uint32_t firstControl=0;
    while(remaining) {
        if(enqueue>=XFER_RING_SIZE-1) {
            tokens[tokenCount++]=ringPhys+(XFER_RING_SIZE-1)*sizeof(Trb);
            ring[XFER_RING_SIZE-1].control=TRB_TYPE_LINK|TRB_TC|(1u<<4)|(cycle?TRB_CYCLE:0);
            cycle=!cycle;enqueue=0;
        }
        const auto bytes=XhciDmaChunk(physical,remaining);
        auto& trb=ring[enqueue];token=ringPhys+enqueue*sizeof(Trb);
        tokens[tokenCount++]=token;
        trb.param=physical;trb.status=bytes;
        if(ctrl.hciVersion>=0x100)trb.status|=XhciTdPackets(len,len-remaining+bytes,maxPacket)<<17;
        const uint32_t control=TRB_TYPE_NORMAL|(remaining>bytes?(1u<<4):TRB_IOC)|
            (isIn?TRB_ISP:0)|(cycle?TRB_CYCLE:0);
        if(!first){first=&trb;firstControl=control;trb.control=control^TRB_CYCLE;}
        else trb.control=control;
        ++enqueue;physical+=bytes;remaining-=bytes;
    }
    __asm__ volatile("mfence":::"memory");
    __atomic_store_n(&first->control,firstControl,__ATOMIC_RELEASE);
    __asm__ volatile("mfence":::"memory");
    XhciRingDoorbell(ctrl,dev.slotId,dci);
    Trb completion;
    Trb* evt=XhciWaitForEvent(ctrl,TRB_TYPE_TRANSFER_EVENT,timeoutMs,&completion,0,tokens,tokenCount);

    if (!evt) {
        KPrintf("xhci: bulk %s timeout slot=%u dci=%u bytes=%u mps=%u trbs=%u failed=%u dma_low=%x\n",
            isIn?"IN":"OUT",dev.slotId,dci,len,maxPacket,tokenCount,ctrl.completionFailed?1u:0u,unsigned(bufPhys&65535));
        return false;
    }

    uint32_t cc = (evt->status >> TRB_CC_SHIFT) & 0xFF;
    if (cc != TRB_CC_SUCCESS || (evt->status&0xffffff) || evt->param!=token) {
        KPrintf("xhci: bulk %s failed cc=%u residue=%u slot=%u dci=%u bytes=%u mps=%u event=%lx last=%lx\n",
            isIn?"IN":"OUT",cc,evt->status&0xffffff,dev.slotId,dci,len,maxPacket,evt->param,token);
        KPrintf("xhci: bulk DMA low=%x tokens=%u first=%lx\n",unsigned(bufPhys&65535),tokenCount,tokens[0]);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// USB Mass Storage Bulk-Only (BBB) protocol
// ---------------------------------------------------------------------------

// Command Block Wrapper (CBW) — 31 bytes, sent via Bulk OUT
struct UsbMscCbw {
    uint32_t dCBWSignature;        // 0x43425355 = "USBC"
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t  bmCBWFlags;           // bit 7: 0=OUT, 1=IN
    uint8_t  bCBWLUN;
    uint8_t  bCBWCBLength;         // length of CBWCB (1-16)
    uint8_t  CBWCB[16];            // SCSI command block
} __attribute__((packed));
static_assert(sizeof(UsbMscCbw) == 31, "CBW must be 31 bytes");

// Command Status Wrapper (CSW) — 13 bytes, received via Bulk IN
struct UsbMscCsw {
    uint32_t dCSWSignature;        // 0x53425355 = "USBS"
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t  bCSWStatus;           // 0=passed, 1=failed, 2=phase error
} __attribute__((packed));
static_assert(sizeof(UsbMscCsw) == 13, "CSW must be 13 bytes");

static constexpr uint32_t CBW_SIGNATURE = 0x43425355;
static constexpr uint32_t CSW_SIGNATURE = 0x53425355;
static constexpr uint8_t  CBW_FLAG_IN   = 0x80;
static constexpr uint8_t  CBW_FLAG_OUT  = 0x00;

// SCSI command opcodes
static constexpr uint8_t SCSI_TEST_UNIT_READY  = 0x00;
static constexpr uint8_t SCSI_INQUIRY          = 0x12;
static constexpr uint8_t SCSI_READ_CAPACITY_10 = 0x25;
static constexpr uint8_t SCSI_READ_10          = 0x28;
static constexpr uint8_t SCSI_WRITE_10         = 0x2A;
static constexpr uint8_t SCSI_SYNCHRONIZE_CACHE_10 = 0x35;

// Execute a SCSI command via BBB protocol
static KMutex g_mscCommandLock{};
static KMutex g_mscIoLock{};
static bool g_storageReadOnly=false;
static uint32_t g_mscDeviceCount=0;

static bool XhciMscCommand(XhciController& ctrl, XhciDevice& dev,
                            const uint8_t* cdb, uint8_t cdbLen,
                            void* data, uint64_t dataPhys, uint32_t dataLen,
                            bool dataIn)
{
    // A BOT transaction comprises CBW, optional data, and CSW. Keep another
    // read/write/flush from inserting commands between those phases.
    struct CommandGuard {
        CommandGuard() { KMutexLock(&g_mscCommandLock); }
        ~CommandGuard() { KMutexUnlock(&g_mscCommandLock); }
    } guard;
    if(__atomic_load_n(&dev.mscQuarantined,__ATOMIC_ACQUIRE))return false;
    if (!dev.mscCommandBuffer)
        dev.mscCommandBuffer = AllocDmaBuffer(1, dev.mscCommandBufferPhys);
    if (!dev.mscCommandBuffer) return false;
    uint64_t cbwPhys = dev.mscCommandBufferPhys;
    auto* cbw = static_cast<UsbMscCbw*>(dev.mscCommandBuffer);

    // Fill CBW
    memset(cbw, 0, sizeof(UsbMscCbw));
    cbw->dCBWSignature = CBW_SIGNATURE;
    cbw->dCBWTag = ++dev.mscTag;
    cbw->dCBWDataTransferLength = dataLen;
    cbw->bmCBWFlags = dataIn ? CBW_FLAG_IN : CBW_FLAG_OUT;
    cbw->bCBWLUN = 0;
    cbw->bCBWCBLength = cdbLen;
    for (uint8_t i = 0; i < cdbLen && i < 16; i++)
        cbw->CBWCB[i] = cdb[i];

    __asm__ volatile("mfence" ::: "memory");

    // 1. Send CBW via Bulk OUT
    if (!XhciBulkTransfer(ctrl, dev, false, cbw, cbwPhys, sizeof(UsbMscCbw))) {
        __atomic_store_n(&dev.mscQuarantined,true,__ATOMIC_RELEASE);
        KPrintf("xhci: MSC CBW failed opcode=%x bytes=%u; transport quarantined\n",unsigned(cdb[0]),dataLen);
        return false;
    }

    // 2. Data phase (if any)
    if (dataLen > 0 && data) {
        if (!XhciBulkTransfer(ctrl, dev, dataIn, data, dataPhys, dataLen)) {
            __atomic_store_n(&dev.mscQuarantined,true,__ATOMIC_RELEASE);
            const uint32_t lba=cdbLen>=10?(uint32_t(cdb[2])<<24)|(uint32_t(cdb[3])<<16)|(uint32_t(cdb[4])<<8)|cdb[5]:0;
            KPrintf("xhci: MSC data %s failed opcode=%x bytes=%u tag=%u lba=%u; transport quarantined\n",dataIn?"IN":"OUT",unsigned(cdb[0]),dataLen,cbw->dCBWTag,lba);
            return false;
        }
    }

    // 3. Receive CSW via Bulk IN
    uint64_t cswPhys = dev.mscCommandBufferPhys + 64;
    auto* csw = reinterpret_cast<UsbMscCsw*>(
        static_cast<uint8_t*>(dev.mscCommandBuffer) + 64);

    memset(csw, 0, sizeof(UsbMscCsw));

    if (!XhciBulkTransfer(ctrl, dev, true, csw, cswPhys, sizeof(UsbMscCsw))) {
        __atomic_store_n(&dev.mscQuarantined,true,__ATOMIC_RELEASE);
        KPrintf("xhci: MSC CSW failed opcode=%x bytes=%u; transport quarantined\n",unsigned(cdb[0]),dataLen);
        return false;
    }

    if (csw->dCSWSignature != CSW_SIGNATURE) {
        __atomic_store_n(&dev.mscQuarantined,true,__ATOMIC_RELEASE);
        KPrintf("xhci: MSC bad CSW signature 0x%08x\n", csw->dCSWSignature);
        return false;
    }

    if(csw->bCSWStatus>1 || csw->dCSWTag!=cbw->dCBWTag || csw->dCSWDataResidue)__atomic_store_n(&dev.mscQuarantined,true,__ATOMIC_RELEASE);
    if (csw->bCSWStatus != 0 || csw->dCSWTag != cbw->dCBWTag ||
        csw->dCSWDataResidue != 0) {
        KPrintf("xhci: MSC command failed (status=%u residue=%u)\n",
                     csw->bCSWStatus, csw->dCSWDataResidue);
        return false;
    }

    return true;
}

// SCSI INQUIRY — identify the device
static bool XhciMscInquiry(XhciController& ctrl, XhciDevice& dev)
{
    uint64_t dataPhys;
    auto* data = static_cast<uint8_t*>(AllocDmaBuffer(1, dataPhys));
    if (!data) return false;

    memset(data, 0, 4096);

    uint8_t cdb[6] = {};
    cdb[0] = SCSI_INQUIRY;
    cdb[4] = 36; // allocation length

    if (!XhciMscCommand(ctrl, dev, cdb, 6, data, dataPhys, 36, true)) {
        SerialPuts("xhci: SCSI INQUIRY failed\n");
        return false;
    }

    // data[0] bits 4:0 = peripheral device type (0=disk)
    // data[8..15] = vendor, data[16..31] = product
    char vendor[9] = {};
    char product[17] = {};
    for (int i = 0; i < 8; i++) vendor[i] = data[8 + i];
    for (int i = 0; i < 16; i++) product[i] = data[16 + i];

    SerialPrintf("xhci: SCSI device type=%u vendor='%s' product='%s'\n",
                 data[0] & 0x1F, vendor, product);
    return true;
}

// SCSI TEST UNIT READY
static bool XhciMscTestUnitReady(XhciController& ctrl, XhciDevice& dev)
{
    uint8_t cdb[6] = {};
    cdb[0] = SCSI_TEST_UNIT_READY;
    return XhciMscCommand(ctrl, dev, cdb, 6, nullptr, 0, 0, false);
}

// SCSI READ CAPACITY(10) — get disk size
static bool XhciMscReadCapacity(XhciController& ctrl, XhciDevice& dev)
{
    uint64_t dataPhys;
    auto* data = static_cast<uint8_t*>(AllocDmaBuffer(1, dataPhys));
    if (!data) return false;

    memset(data, 0, 4096);

    uint8_t cdb[10] = {};
    cdb[0] = SCSI_READ_CAPACITY_10;

    if (!XhciMscCommand(ctrl, dev, cdb, 10, data, dataPhys, 8, true)) {
        SerialPuts("xhci: SCSI READ CAPACITY failed\n");
        return false;
    }

    // Response is 8 bytes, big-endian:
    // bytes 0-3: last logical block address
    // bytes 4-7: block length in bytes
    uint32_t lastLba = (static_cast<uint32_t>(data[0]) << 24) |
                       (static_cast<uint32_t>(data[1]) << 16) |
                       (static_cast<uint32_t>(data[2]) << 8)  |
                       static_cast<uint32_t>(data[3]);
    uint32_t blockLen = (static_cast<uint32_t>(data[4]) << 24) |
                        (static_cast<uint32_t>(data[5]) << 16) |
                        (static_cast<uint32_t>(data[6]) << 8)  |
                        static_cast<uint32_t>(data[7]);

    if(lastLba==0xffffffffu || blockLen<512 || blockLen>65536 || (blockLen&(blockLen-1))) {
        SerialPuts("xhci: unsupported READ(10) capacity/block size\n");return false;
    }
    dev.mscSectorCount = static_cast<uint64_t>(lastLba) + 1;
    dev.mscBlockSize = blockLen;

    uint32_t sizeMB = static_cast<uint32_t>(
        (dev.mscSectorCount * dev.mscBlockSize) / (1024 * 1024));
    SerialPrintf("xhci: disk capacity: %u sectors × %u bytes = %u MB\n",
                 static_cast<uint32_t>(dev.mscSectorCount), blockLen, sizeMB);
    return true;
}

// ---------------------------------------------------------------------------
// Block device ops for USB mass storage
// ---------------------------------------------------------------------------

// Forward declarations for the DeviceOps callbacks
static int UsbMscRead(Device* dev, uint64_t offset, void* buf, uint64_t len);
static int UsbMscWrite(Device* dev, uint64_t offset, const void* buf, uint64_t len);
static int UsbMscIoctl(Device* dev, uint32_t cmd, void* arg);
static void UsbMscClose(Device* dev);
static uint64_t UsbMscBlockCount(Device* dev);
static uint32_t UsbMscBlockSize(Device* dev);

// Per-device state passed through Device::priv
struct UsbMscState {
    XhciController* ctrl;
    uint32_t devIndex;   // index into g_devices
    uint8_t* bounce;
    uint64_t bouncePhys;
    char name[16];
};

static const BlockDeviceOps g_usbMscOps = {
    .read        = UsbMscRead,
    .write       = UsbMscWrite,
    .ioctl       = UsbMscIoctl,
    .close       = UsbMscClose,
    .block_count = UsbMscBlockCount,
    .block_size  = UsbMscBlockSize,
};

struct UsbMscByteTransport {
    UsbMscState* state;
    uint32_t blockSize() {return g_devices[state->devIndex].mscBlockSize;}
    uint64_t blockCount() {return g_devices[state->devIndex].mscSectorCount;}
    uint32_t bounceBytes() {return 65536;}
    uint8_t* bounce() {return state->bounce;}
    bool usable() {return !__atomic_load_n(&g_devices[state->devIndex].mscQuarantined,__ATOMIC_ACQUIRE);}
    bool transfer(uint64_t lba,uint32_t count,bool read) {
        if(!usable())return false;
        uint8_t cdb[10]{};cdb[0]=read?SCSI_READ_10:SCSI_WRITE_10;
        cdb[2]=lba>>24;cdb[3]=lba>>16;cdb[4]=lba>>8;cdb[5]=lba;
        cdb[7]=count>>8;cdb[8]=count;
        return XhciMscCommand(*state->ctrl,g_devices[state->devIndex],cdb,10,
            state->bounce,state->bouncePhys,count*blockSize(),read);
    }
    bool readBlocks(uint64_t lba,uint32_t count) {return transfer(lba,count,true);}
    bool writeBlocks(uint64_t lba,uint32_t count) {return transfer(lba,count,false);}
};
struct MscIoGuard {
    MscIoGuard(){KMutexLock(&g_mscIoLock);}
    ~MscIoGuard(){KMutexUnlock(&g_mscIoLock);}
};
static int UsbMscRead(Device* dev,uint64_t offset,void* buf,uint64_t len) {
    MscIoGuard guard;
    UsbMscByteTransport io{static_cast<UsbMscState*>(dev->priv)};
    if(!io.usable())return -5;
    return UsbBlockReadBytes(io,offset,buf,len);
}
static int UsbMscWrite(Device* dev,uint64_t offset,const void* buf,uint64_t len) {
    if(g_storageReadOnly)return -30;
    MscIoGuard guard;
    UsbMscByteTransport io{static_cast<UsbMscState*>(dev->priv)};
    if(!io.usable())return -5;
    return UsbBlockWriteBytes(io,offset,buf,len);
}

static int UsbMscIoctl(Device* dev, uint32_t cmd, void* /*arg*/)
{
    if (cmd == DEVICE_IOCTL_READ_ONLY) return g_storageReadOnly ? 1 : 0;
    if (cmd != DEVICE_IOCTL_FLUSH) return -95;
    if(g_storageReadOnly)return 0;
    auto* st = static_cast<UsbMscState*>(dev->priv);
    XhciDevice& udev = g_devices[st->devIndex];
    // IMMED=0, LBA=0 and length=0: wait for the entire device cache.
    uint8_t cdb[10] = {};
    cdb[0] = SCSI_SYNCHRONIZE_CACHE_10;
    return XhciMscCommand(*st->ctrl, udev, cdb, sizeof(cdb),
                         nullptr, 0, 0, false) ? 0 : -5;
}

static void UsbMscClose(Device* /*dev*/) {}

static uint64_t UsbMscBlockCount(Device* dev)
{
    auto* st = static_cast<UsbMscState*>(dev->priv);
    return g_devices[st->devIndex].mscSectorCount;
}

static uint32_t UsbMscBlockSize(Device* dev)
{
    auto* st = static_cast<UsbMscState*>(dev->priv);
    return g_devices[st->devIndex].mscBlockSize;
}

// Initialize a USB mass storage device after enumeration
static bool XhciInitMassStorage(XhciController& ctrl, XhciDevice& dev,
                                 uint32_t devIndex)
{
    SerialPuts("xhci: initializing mass storage device\n");

    // Configure bulk IN endpoint
    if (dev.bulkInAddr) {
        if (!XhciConfigureBulkEndpoint(ctrl, dev, dev.bulkInAddr,
                                        dev.bulkInMaxPacket,
                                        dev.bulkInRing, dev.bulkInRingPhys,
                                        dev.bulkInEnqueue, dev.bulkInCycle,
                                        dev.bulkInDci)) {
            SerialPuts("xhci: failed to configure bulk IN\n");
            return false;
        }
    }

    // Configure bulk OUT endpoint
    if (dev.bulkOutAddr) {
        if (!XhciConfigureBulkEndpoint(ctrl, dev, dev.bulkOutAddr,
                                        dev.bulkOutMaxPacket,
                                        dev.bulkOutRing, dev.bulkOutRingPhys,
                                        dev.bulkOutEnqueue, dev.bulkOutCycle,
                                        dev.bulkOutDci)) {
            SerialPuts("xhci: failed to configure bulk OUT\n");
            return false;
        }
    }

    // SCSI INQUIRY
    XhciMscInquiry(ctrl, dev);

    // TEST UNIT READY (may need retries for slow devices)
    for (int retry = 0; retry < 3; retry++) {
        if (XhciMscTestUnitReady(ctrl, dev)) break;
        // Brief delay between retries
        for (volatile int d = 0; d < 1000000; d++) {}
    }

    // READ CAPACITY
    if (!XhciMscReadCapacity(ctrl, dev)) {
        SerialPuts("xhci: READ CAPACITY failed\n");
        return false;
    }

    // Register as block device
    auto* state = static_cast<UsbMscState*>(kmalloc(sizeof(UsbMscState)));
    if (!state) return false;
    state->ctrl = &ctrl;
    state->devIndex = devIndex;
    state->bounce=static_cast<uint8_t*>(AllocDmaBuffer(16,state->bouncePhys));
    if(!state->bounce){kfree(state);return false;}

    auto* blkDev = static_cast<Device*>(kmalloc(sizeof(Device)));
    if (!blkDev) { kfree(state); return false; }

    blkDev->ops  = reinterpret_cast<const DeviceOps*>(&g_usbMscOps);
    const uint32_t index=g_mscDeviceCount;
    state->name[0]='u';state->name[1]='s';state->name[2]='b';
    uint32_t pos=3;if(index>=10)state->name[pos++]='0'+index/10;
    state->name[pos++]='0'+index%10;state->name[pos]=0;
    blkDev->name = state->name;
    blkDev->type = DeviceType::Block;
    blkDev->priv = state;

    if (!DeviceRegister(blkDev)) {
        SerialPuts("xhci: failed to register USB block device\n");
        kfree(blkDev);
        kfree(state);
        return false;
    }

    ++g_mscDeviceCount;
    SerialPrintf("xhci: USB mass storage registered as '%s'\n", blkDev->name);
    return true;
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

    if (dev.isMouse && dev.interruptEpAddr) {
        SerialPrintf("xhci: USB mouse detected on port %u (slot %u)\n",
                     portNum, dev.slotId);

        XhciSetBootProtocol(ctrl, dev);

        if (XhciConfigureInterruptEndpoint(ctrl, dev)) {
            SerialPuts("xhci: mouse interrupt endpoint ready\n");
            g_mousePresent = true;
        }
    }

    // If it's a mass storage device, initialize SCSI and register block device
    if(dev.ecm.value)XhciInitEcm(ctrl,dev);
    if (dev.isMassStorage && dev.bulkInAddr && dev.bulkOutAddr) {
        SerialPrintf("xhci: USB mass storage detected on port %u (slot %u)\n",
                     portNum, dev.slotId);
        XhciInitMassStorage(ctrl, dev, g_deviceCount - 1);
    }

    // If it's a hub, initialize and enumerate downstream devices
    if (dev.isHub) {
        SerialPrintf("xhci: USB hub detected on port %u (slot %u)\n",
                     portNum, dev.slotId);
        dev.hubTier = 0; // directly on root hub
        if (XhciHubInit(ctrl, dev)) {
            XhciHubPollPorts(ctrl, dev);
        }
    }
}

// ---------------------------------------------------------------------------
// Port scanning with device enumeration
// ---------------------------------------------------------------------------

static void XhciScanPorts(XhciController& ctrl)
{
    KPrintf("xhci: scanning %u root ports (PCI %x:%x)...\n",ctrl.maxPorts,
        ctrl.pciDev.vendorId,ctrl.pciDev.deviceId);
    // Firmware is no longer responsible for powering the root hub. Power all
    // ports first, then allow connection/debounce/link training to settle once.
    for(uint32_t port=0;port<ctrl.maxPorts;++port) {
        const auto offset=XHCI_OP_PORTSC_BASE+port*0x10;
        const auto state=xhci_read32(ctrl.opBase,offset);
        KPrintf("xhci: port %u before power PORTSC=0x%08x\n",port+1,state);
        if(state==0xffffffffu){KPuts("xhci: root port registers inaccessible\n");return;}
        if(!(state&PORTSC_PP)) {
            xhci_write32(ctrl.opBase,offset,XhciPortPersistent(state)|PORTSC_PP);
            (void)xhci_read32(ctrl.opBase,offset); // flush posted MMIO write
        }
    }
    const auto settle=ApicTickCount();
    while(ApicTickCount()-settle<250)__asm__ volatile("pause");

    for (uint32_t port = 0; port < ctrl.maxPorts; port++) {
        uint32_t portsc = xhci_read32(ctrl.opBase,
                                       XHCI_OP_PORTSC_BASE + port * 0x10);

        bool connected = (portsc & PORTSC_CCS) != 0;
        bool enabled   = (portsc & PORTSC_PED) != 0;
        bool powered   = (portsc & PORTSC_PP) != 0;
        uint32_t speed = (portsc >> 10) & 0xF;

        KPrintf("xhci: port %u: %s %s %s speed=%s (PORTSC=0x%08x)\n",
                     port + 1,
                     connected ? "CONNECTED" : "disconnected",
                     enabled ? "enabled" : "disabled",
                     powered ? "powered" : "unpowered",
                     XhciSpeedString(speed),
                     portsc);

        if (connected && enabled) {
            // USB 3.0 (SuperSpeed) ports are auto-enabled — enumerate directly
            SerialPrintf("xhci: port %u already enabled, enumerating\n", port + 1);
            XhciEnumeratePort(ctrl, port + 1, speed);
        }
        else if (connected && !enabled) {
            if(speed>=4) {
                KPrintf("xhci: port %u disabled SuperSpeed link PLS=%u; warm recovery not attempted\n",port+1,(portsc>>5)&15);
                continue;
            }
            // Reset the port to enable it
            SerialPrintf("xhci: resetting port %u...\n", port + 1);

            struct PortIo {
                XhciController& controller;uint32_t offset;
                uint32_t read(){return xhci_read32(controller.opBase,offset);}
                void write(uint32_t value){xhci_write32(controller.opBase,offset,value);}
                uint64_t now(){return ApicTickCount();}
                bool wait(){__asm__ volatile("pause");return true;}
            } io{ctrl,XHCI_OP_PORTSC_BASE+port*0x10};
            const bool resetOk=XhciResetRootPort(io,1000);

            portsc = xhci_read32(ctrl.opBase,
                                  XHCI_OP_PORTSC_BASE + port * 0x10);
            speed = (portsc >> 10) & 0xF;
            KPrintf("xhci: port %u after reset: %s speed=%s PORTSC=0x%08x\n",
                         port + 1,
                         (portsc & PORTSC_PED) ? "ENABLED" : "disabled",
                         XhciSpeedString(speed),portsc);

            // If enabled, enumerate the device
            if (resetOk && (portsc & PORTSC_PED)) {
                XhciEnumeratePort(ctrl, port + 1, speed);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Phase 8: Interrupt-driven event ring processing (ISR)
// ---------------------------------------------------------------------------

// Match a transfer event's TRB pointer to a device. The event ring's param
// field holds the physical address of the completed TRB. We check each
// device's interrupt and bulk transfer ring physical address ranges.
static int XhciMatchTransferEvent(const Trb& evt)
{
    uint64_t trbPhys = evt.param;
    uint32_t ringBytes = XFER_RING_SIZE * sizeof(Trb);

    for (uint32_t i = 0; i < g_deviceCount; i++) {
        XhciDevice& dev = g_devices[i];

        // Check interrupt endpoint ring
        if (dev.intRingPhys &&
            trbPhys >= dev.intRingPhys &&
            trbPhys < dev.intRingPhys + ringBytes)
            return static_cast<int>(i);

        // Check bulk IN ring
        if (dev.bulkInRingPhys &&
            trbPhys >= dev.bulkInRingPhys &&
            trbPhys < dev.bulkInRingPhys + ringBytes)
            return static_cast<int>(i);

        // Check bulk OUT ring
        if (dev.bulkOutRingPhys &&
            trbPhys >= dev.bulkOutRingPhys &&
            trbPhys < dev.bulkOutRingPhys + ringBytes)
            return static_cast<int>(i);

        // Check EP0 ring (control transfers during enumeration)
        if (dev.ep0RingPhys &&
            trbPhys >= dev.ep0RingPhys &&
            trbPhys < dev.ep0RingPhys + ringBytes)
            return static_cast<int>(i);
    }

    return -1; // unknown device
}

// ISR body — called from either the shared IRQ stub (legacy INTx) or
// the MSI-X wrapper below.  Drains all pending event ring entries.
static void XhciIrqHandler()
{
    XhciPollDrainEvents();
}

// MSI-X IDT entry — compiler-generated interrupt stub.
// Inner function called from the naked SWAPGS wrapper below.
extern "C" void XhciMsixIsrInner()
{
    XhciIrqHandler();
    ApicSendEoi();
}

// Naked wrapper with proper SWAPGS for user→kernel transitions.
IRQ_NAKED_HANDLER(XhciMsixIsr, XhciMsixIsrInner)

// ---------------------------------------------------------------------------
// Module init / exit
// ---------------------------------------------------------------------------

static int XhciModuleInit()
{
    SerialPuts("xhci: initializing USB 3.0 host controller driver\n");
    if(auto* marker=VfsOpen("/etc/usb-read-only")) {
        g_storageReadOnly=true;VfsClose(marker);KPuts("xhci: physical storage probe is read-only\n");
    }

    // Detect xHCI controllers on PCI bus
    XhciController& ctrl = g_controllers[0];

    if (!XhciPciDetect(ctrl)) {
        SerialPuts("xhci: no controller detected, module inactive\n");
        return 0; // not an error — just no hardware present
    }

    struct OwnershipIo {
        XhciController& ctrl;uint64_t deadline;
        uint32_t read(uint32_t offset){return xhci_read32(ctrl.capBase,offset);}
        void write(uint32_t offset,uint32_t value){xhci_write32(ctrl.capBase,offset,value);}
        void writeByte(uint32_t offset,uint8_t value){*(ctrl.capBase+offset)=value;}
        bool wait(){__asm__ volatile("pause");return ApicTickCount()<deadline;}
    } ownership{ctrl,ApicTickCount()+1000};
    uint32_t legacy=0;
    const auto extended=(xhci_read32(ctrl.capBase,XHCI_CAP_HCCPARAMS1)>>16)*4;
    if(!XhciAcquireFirmwareOwnership(ownership,extended,65536,legacy)) {
        KPuts("xhci: firmware ownership handoff failed; controller untouched\n");return -1;
    }
    KPrintf("xhci: firmware handoff complete legacy=0x%x\n",legacy);
    // Reset only after firmware releases ownership and halt is observed.
    if (!XhciReset(ctrl)) {KPuts("xhci: controller reset failed; USB storage/network unavailable\n");return -1;}
    PciEnableBusMaster(ctrl.pciDev);

    // Allocate command/event rings and DCBAA
    if (!XhciAllocRings(ctrl)) {KPuts("xhci: ring allocation failed; USB storage/network unavailable\n");return -1;}

    // Start the controller
    if (!XhciStartController(ctrl)) {KPuts("xhci: controller start failed; USB storage/network unavailable\n");return -1;}

    ctrl.initialized = true;
    g_controllerCount = 1;

    struct RoutingIo {
        const PciDevice& device;
        uint32_t read(uint8_t offset){return PciConfigRead32(device.bus,device.dev,device.fn,offset);}
        void write(uint8_t offset,uint32_t value){PciConfigWrite32(device.bus,device.dev,device.fn,offset,value);}
    } routing{ctrl.pciDev};
    XhciIntelRouteState route;
    const uint32_t routeClass=(uint32_t(ctrl.pciDev.classCode)<<16)|(uint32_t(ctrl.pciDev.subclass)<<8)|ctrl.pciDev.progIf;
    const auto routeResult=XhciEnableIntelRouting(ctrl.pciDev.vendorId,ctrl.pciDev.deviceId,routeClass,routing,route);
    if(routeResult!=XhciIntelRouteResult::NotApplicable) {
        KPrintf("xhci: Intel routing result=%u USB2=%08x->%08x mask=%08x SS=%08x->%08x mask=%08x\n",
            static_cast<unsigned>(routeResult),route.usb2Before,route.usb2After,route.usb2Mask,
            route.ssBefore,route.ssAfter,route.ssMask);
        KPrintf("xhci: Intel read-only port-disable overrides USB2=%08x SS=%08x\n",routing.read(0xe4),routing.read(0xe8));
        if(route.usb2Before==route.usb2After && route.ssBefore==route.ssAfter)
            KPuts("xhci: Intel no routing change; connectivity still requires port evidence\n");
    }

    // Small delay for port status to settle
    for (volatile int d = 0; d < 1000000; d++);

    // Scan ports and enumerate connected devices
    XhciScanPorts(ctrl);
    if(g_storageReadOnly)KPrintf("xhci: initial port scan found %u USB devices\n",g_deviceCount);

    // Process any pending port status change events
    while (true) {
        Trb* evt = XhciWaitForEvent(ctrl, 0, 50);
        if (!evt) break;
    }

    // Clear any stale interrupt state from polling phase.  The controller
    // won't generate new MSI-X messages while IMAN.IP or USBSTS.EINT are set.
    {
        uint32_t sts = xhci_read32(ctrl.opBase, XHCI_OP_USBSTS);
        if (sts & USBSTS_EINT)
            xhci_write32(ctrl.opBase, XHCI_OP_USBSTS, USBSTS_EINT); // W1C

        uint32_t iman = xhci_read32(ctrl.rtBase, XHCI_RT_IMAN);
        xhci_write32(ctrl.rtBase, XHCI_RT_IMAN, iman | 0x3); // W1C IP, keep IE

        // Write ERDP with EHB=1 to clear Event Handler Busy
        uint64_t erdpPhys = ctrl.evtRingPhys + ctrl.evtDequeue * sizeof(Trb);
        xhci_write64(ctrl.rtBase, XHCI_RT_ERDP, erdpPhys | (1ULL << 3));
    }

    // --- Phase 8: Register IRQ handler for interrupt-driven event processing ---

    // Set up MSI-X for interrupt delivery.  QEMU's qemu-xhci uses MSI-X and
    // does not support legacy INTx fallback, so we must program the MSI-X
    // table directly.  We configure a single entry (interrupter 0) to deliver
    // our chosen vector to the BSP's LAPIC.
    ctrl.completionFailed=false;
    bool msixConfigured = false;
    {
        uint8_t capPtr = static_cast<uint8_t>(
            PciConfigRead8(ctrl.pciDev.bus, ctrl.pciDev.dev, ctrl.pciDev.fn, 0x34));
        for(unsigned hops=0;capPtr>=0x40 && capPtr<=0xfc && !(capPtr&3) && hops<48;++hops) {
            uint8_t capId = PciConfigRead8(ctrl.pciDev.bus, ctrl.pciDev.dev,
                                           ctrl.pciDev.fn, capPtr);
            if (capId == 0x11) { // MSI-X capability
                // Read Message Control
                uint16_t msgCtrl = static_cast<uint16_t>(
                    PciConfigRead16(ctrl.pciDev.bus, ctrl.pciDev.dev,
                                    ctrl.pciDev.fn, capPtr + 2));
                uint16_t tableSize = (msgCtrl & 0x07FF) + 1;

                // Read Table Offset/BIR
                uint32_t tableOffBir = PciConfigRead32(ctrl.pciDev.bus,
                    ctrl.pciDev.dev, ctrl.pciDev.fn, capPtr + 4);
                uint32_t tableBir    = tableOffBir & 0x7;
                uint32_t tableOffset = tableOffBir & ~0x7u;

                SerialPrintf("xhci: MSI-X cap at 0x%x: %u entries, BAR%u+0x%x\n",
                             capPtr, tableSize, tableBir, tableOffset);

                // Get the BAR physical address for the MSI-X table
                uint64_t barPhys = PciBarMemBase32(ctrl.pciDev.bar[tableBir]);
                if (PciBarIs64(ctrl.pciDev.bar[tableBir]) && tableBir + 1 < 6)
                    barPhys |= static_cast<uint64_t>(ctrl.pciDev.bar[tableBir + 1]) << 32;

                if (barPhys == 0) {
                    SerialPuts("xhci: MSI-X table BAR is zero\n");
                    break;
                }

                // Map the MSI-X table page(s)
                uint64_t tablePhys = barPhys + tableOffset;
                uint32_t tableBytes = tableSize * 16; // 16 bytes per entry
                uint32_t tablePages = (tableBytes + tableOffset % 4096 + 4095) / 4096;
                auto tblVaddr = VmmAllocPages(tablePages, VMM_WRITABLE | VMM_NO_EXEC,
                                              MemTag::Device, KernelPid);
                if (!tblVaddr) {
                    SerialPuts("xhci: failed to alloc pages for MSI-X table\n");
                    break;
                }
                uint64_t pageAlignedPhys = tablePhys & ~0xFFFULL;
                for (uint32_t p = 0; p < tablePages; p++) {
                    VmmMapPage(KernelPageTable,
                               VirtualAddress(tblVaddr.raw() + p * 4096),
                               PhysicalAddress(pageAlignedPhys + p * 4096),
                               VMM_WRITABLE | VMM_NO_EXEC | VMM_CACHE_DISABLE,
                               MemTag::Device, KernelPid);
                }

                volatile uint32_t* table = reinterpret_cast<volatile uint32_t*>(
                    tblVaddr.raw() + (tablePhys & 0xFFF));

                // Program entry 0: target BSP LAPIC (ID 0), edge-triggered,
                // fixed delivery, our chosen vector.
                // Message Address: 0xFEE0_0000 | (destApic << 12)
                // Message Data:    vector (bits 7:0)
                uint8_t destLapic = ApicGetId();
                table[0] = 0xFEE00000u | (static_cast<uint32_t>(destLapic) << 12);
                table[1] = 0; // upper address = 0
                table[2] = XHCI_IRQ_VECTOR; // message data = vector
                table[3] = 0; // vector control: unmask (bit 0 = 0)

                // Mask all other entries
                for (uint16_t i = 1; i < tableSize; i++)
                    table[i * 4 + 3] = 1; // mask

                IdtInstallHandler(XHCI_IRQ_VECTOR,reinterpret_cast<void*>(XhciMsixIsr));
                // Enable MSI-X (set bit 15), clear function mask (bit 14)
                msgCtrl |= 0x8000;   // MSI-X Enable
                msgCtrl &= ~0x4000;  // Clear Function Mask
                PciConfigWrite16(ctrl.pciDev.bus, ctrl.pciDev.dev,
                                 ctrl.pciDev.fn, capPtr + 2, msgCtrl);

                // Install our MSI-X ISR wrapper in the IDT
                IdtInstallHandler(XHCI_IRQ_VECTOR,
                                  reinterpret_cast<void*>(XhciMsixIsr));

                msixConfigured = true;
                SerialPrintf("xhci: MSI-X configured — vector %u → LAPIC %u\n",
                             XHCI_IRQ_VECTOR, destLapic);
                break;
            }
            capPtr = PciConfigRead8(ctrl.pciDev.bus, ctrl.pciDev.dev,
                                    ctrl.pciDev.fn, capPtr + 1);
        }
    }

    bool msiConfigured=false;
    if(!msixConfigured) {
        uint8_t cap=PciConfigRead8(ctrl.pciDev.bus,ctrl.pciDev.dev,ctrl.pciDev.fn,0x34);
        for(unsigned hops=0;cap>=0x40 && cap<=0xfc && !(cap&3) && hops<48;++hops) {
            if(PciConfigRead8(ctrl.pciDev.bus,ctrl.pciDev.dev,ctrl.pciDev.fn,cap)==5) {
                const auto control=PciConfigRead16(ctrl.pciDev.bus,ctrl.pciDev.dev,ctrl.pciDev.fn,cap+2);
                XhciMsiLayout layout;
                if(!XhciMsiRegisters(cap,control,layout))break;
                const auto b=ctrl.pciDev.bus,d=ctrl.pciDev.dev,f=ctrl.pciDev.fn;
                PciConfigWrite16(b,d,f,cap+2,control&~1u);
                IdtInstallHandler(XHCI_IRQ_VECTOR,reinterpret_cast<void*>(XhciMsixIsr));
                PciConfigWrite32(b,d,f,cap+4,0xfee00000u|(uint32_t(ApicGetId())<<12));
                if(layout.upper)PciConfigWrite32(b,d,f,layout.upper,0);
                PciConfigWrite16(b,d,f,layout.data,XHCI_IRQ_VECTOR);
                if(layout.mask)PciConfigWrite32(b,d,f,layout.mask,PciConfigRead32(b,d,f,layout.mask)&~1u);
                PciConfigWrite16(b,d,f,4,PciConfigRead16(b,d,f,4)|(1u<<10)); // INTx disable
                PciConfigWrite16(b,d,f,cap+2,layout.enabled);
                msiConfigured=true;SerialPrintf("xhci: MSI configured vector=%u LAPIC=%u\n",XHCI_IRQ_VECTOR,ApicGetId());
                break;
            }
            cap=PciConfigRead8(ctrl.pciDev.bus,ctrl.pciDev.dev,ctrl.pciDev.fn,cap+1);
        }
    }
    if (!msixConfigured && !msiConfigured) {
        // Fallback: legacy INTx via IOAPIC
        uint32_t intLine = PciConfigRead32(ctrl.pciDev.bus, ctrl.pciDev.dev,
                                            ctrl.pciDev.fn, 0x3C) & 0xFF;
        if(intLine==0 || intLine>=24) {
            SerialPuts("xhci: no usable interrupt route; polling mode retained\n");
            ctrl.irqLine=0;
        } else {
        ctrl.irqLine = static_cast<uint8_t>(intLine);
        ctrl.irqVector = IoApicRegisterHandler(ctrl.irqLine, XHCI_IRQ_VECTOR,
                                                reinterpret_cast<void*>(XhciIrqHandler));
        SerialPrintf("xhci: legacy INTx — IRQ %u, vector %u\n",
                     ctrl.irqLine, ctrl.irqVector);
        }
    }

    g_irqActive = msixConfigured || msiConfigured || ctrl.irqVector;
    SerialPrintf("xhci: interrupt-driven mode active (MSI-X=%s)\n",
                 msixConfigured ? "yes" : "no");

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

    // Register USB mouse input device if a mouse was found
    for (uint32_t i = 0; i < g_deviceCount; i++) {
        if (g_devices[i].isMouse && g_devices[i].intRing) {
            g_usbMouseOps.poll = XhciMousePoll;
            g_usbMouseDev.ops = &g_usbMouseOps;
            g_usbMouseDev.head = 0;
            g_usbMouseDev.tail = 0;
            InputRegister(&g_usbMouseDev);

            uint64_t mouseBufPhys;
            auto* mouseBuf = static_cast<uint8_t*>(AllocDmaBuffer(1, mouseBufPhys));
            if (mouseBuf) {
                g_devices[i].priv = mouseBuf;
                g_mouseReportBufPhys = mouseBufPhys;
                g_mouseDevIndex = i;
                XhciQueueInterruptIn(g_controllers[0], g_devices[i],
                                      mouseBuf, mouseBufPhys, 4);
            }

            SerialPuts("xhci: USB mouse registered as input device\n");
            break;
        }
    }

    KPrintf("xhci: driver initialized (%u controller%s, %u device%s)\n",
                 g_controllerCount, g_controllerCount > 1 ? "s" : "",
                 g_deviceCount, g_deviceCount > 1 ? "s" : "");
    return 0;
}

static void XhciModuleExit()
{
    // Disable IRQ before stopping controller
    g_irqActive = false;
    for (uint32_t i = 0; i < g_controllerCount; i++) {
        XhciController& ctrl = g_controllers[i];
        if (!ctrl.initialized) continue;

        // Unregister IRQ handler
        IoApicUnregisterHandler(ctrl.irqLine,
                                 reinterpret_cast<void*>(XhciIrqHandler));

        // Disable interrupts on the controller
        uint32_t cmd = xhci_read32(ctrl.opBase, XHCI_OP_USBCMD);
        xhci_write32(ctrl.opBase, XHCI_OP_USBCMD, cmd & ~(USBCMD_RS | USBCMD_INTE));
        SerialPrintf("xhci: controller %u stopped\n", i);
    }
    SerialPuts("xhci: driver unloaded\n");
}

#pragma clang diagnostic pop

DECLARE_MODULE("xhci", XhciModuleInit, XhciModuleExit,
               "xHCI USB 3.0 Host Controller driver");
