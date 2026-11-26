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
#include "usb_write_windows.h"
#include "usb_scsi_unmap.h"
#include "usb_uas.h"
#include "usb_system.h"
#include "usb_ecm_descriptors.h"
#include "usb_ecm_rx_credits.h"
#include "usb_ecm_tx_slots.h"
#include "usb_ecm_trb_ring.h"
#include "net.h"
#include "xhci_ownership.h"
#include "xhci_controller_reset.h"
#include "usb_boot_health.h"
#include "xhci_root_port.h"
#include "xhci_intel_routing.h"
#include "xhci_msi.h"
#include "xhci_completion_queue.h"
#include "usb_hub_logic.h"
#include "usb_uac2.h"
#include "audio.h"
#include "boot_status.h"
#include "debug_hardware.h"
#include "scheduler.h"
#include "usb_scsi_cache.h"
#include "usb_bot_recovery.h"
#include "sync/kmutex.h"
#include "vfs.h"
#include "pci.h"
#include "pci_quiesce.h"
#include "platform_power.h"
#include "serial.h"
#include "kprintf.h"
#include "idt.h"
#include "apic.h"
#include "spinlock.h"
#include "xhci_dma_reserve.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "memory/address.h"
#include "mem_tag.h"
#include "input.h"
#include "boot_pointer_usb.h"
#include "compositor.h"
#include "usb_boot_keyboard.h"
#include "usb_fido.h"
#include "fido_hidraw.h"
#include "keyboard.h"
#include "device.h"
#include "storage_io_stats.h"
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
MODULE_IMPORT_SYMBOL(UsbBootReportReset);
MODULE_IMPORT_SYMBOL(KPrintf);
MODULE_IMPORT_SYMBOL(KPuts);
MODULE_IMPORT_SYMBOL(DebugUsbCommandSetHandler);
MODULE_IMPORT_SYMBOL(KernelWorkerStart);
MODULE_IMPORT_SYMBOL(AudioRegister);
MODULE_IMPORT_SYMBOL(AudioClockNowNs);
MODULE_IMPORT_SYMBOL(AudioOutputControlState);
MODULE_IMPORT_SYMBOL(SchedulerSleepMs);
MODULE_IMPORT_SYMBOL(PlatformPowerAddQuiesce);
MODULE_IMPORT_SYMBOL(PciQuiesceFunctionForReset);
MODULE_IMPORT_SYMBOL(VmmAllocPages);
MODULE_IMPORT_SYMBOL(VmmVirtToPhys);
MODULE_IMPORT_SYMBOL(VmmMapPage);
MODULE_IMPORT_SYMBOL(PmmAllocPagesAccounted);
MODULE_IMPORT_SYMBOL(IoApicRegisterHandler);
MODULE_IMPORT_SYMBOL(IoApicUnregisterHandler);
MODULE_IMPORT_SYMBOL(ApicGetId);
MODULE_IMPORT_SYMBOL(IdtInstallHandler);
MODULE_IMPORT_SYMBOL(InputRegister);
MODULE_IMPORT_SYMBOL(HidrawAttach);
MODULE_IMPORT_SYMBOL(HidrawActivate);
MODULE_IMPORT_SYMBOL(HidrawDetach);
MODULE_IMPORT_SYMBOL(HidrawReceive);
MODULE_IMPORT_SYMBOL(HidrawOutputComplete);
MODULE_IMPORT_SYMBOL(HidrawTransportFailed);
MODULE_IMPORT_SYMBOL(InputMarkPointerIngress);
extern "C" void BootTimingUsbPointer(bool, uint32_t, const char*, uint16_t, uint16_t);
MODULE_IMPORT_SYMBOL(BootTimingUsbPointer);
MODULE_IMPORT_SYMBOL(BootTimingUsbPointerTransfer);
MODULE_IMPORT_SYMBOL(InputWakeWaiters);
MODULE_IMPORT_SYMBOL(CompositorWake);
MODULE_IMPORT_SYMBOL(InputNextSequence);
MODULE_IMPORT_SYMBOL(KbdPushCodepoint);
MODULE_IMPORT_SYMBOL(KbdTranslateKey);
MODULE_IMPORT_SYMBOL(KbdLayout);
MODULE_IMPORT_SYMBOL(kmalloc);
MODULE_IMPORT_SYMBOL(kfree);
MODULE_IMPORT_SYMBOL(PmmFreePage);
MODULE_IMPORT_SYMBOL(KMutexLock);
MODULE_IMPORT_SYMBOL(KMutexUnlock);
MODULE_IMPORT_SYMBOL(KMutexTryLock);
MODULE_IMPORT_SYMBOL(NetEarlyBootPolling);
MODULE_IMPORT_SYMBOL(VfsOpen);
MODULE_IMPORT_SYMBOL(VfsClose);
MODULE_IMPORT_SYMBOL(DeviceRegister);
MODULE_IMPORT_SYMBOL(StorageIoAccount);
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
static constexpr uint32_t MAX_DEVICES     = 48;   // max USB devices tracked (all controllers)
static constexpr uint32_t MAX_CONTROLLERS = 4;    // max xHCI controllers
static constexpr uint32_t MAX_SLOTS_PER_CONTROLLER = 32; // MaxSlotsEn
static constexpr uint32_t XHCI_MAX_ROOT_PORTS = 64;
static constexpr uint32_t XHCI_HUB_MAX_PORTS = 15;
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
    uint32_t mapBytes;             // bytes of BAR0 mapped at capBase
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

    // Bring-up record for the status block.
    uint32_t index;
    bool dma32;                 // DMA must stay below 4 GiB
    const char* handoffText;
    char resetText[72];
    const char* runText;
    const char* irqText;
    const char* failure;        // why the controller is not driven
    uint8_t portMajor[XHCI_MAX_ROOT_PORTS]; // Supported Protocol major revision
    struct PortState {
        uint8_t device;         // device index + 1 (0 = none)
        uint8_t tries, waits, cc;
        const char* error;
        uint32_t portsc;
        uint32_t logged;        // last PORTSC logged (state bits only)
        uint64_t lastTryMs;     // last enumeration attempt (retry back-off)
    } ports[XHCI_MAX_ROOT_PORTS];
};

static XhciController g_controllers[MAX_CONTROLLERS];

// Bounded text builder for the status block and bring-up records.
struct XhciText {
    char* out; uint32_t cap, len;
    void c(char ch) { if (len + 1 < cap) out[len++] = ch; }
    void s(const char* text) { while (text && *text) c(*text++); }
    void u(uint64_t v) { char d[24]; uint32_t n = 0; do { d[n++] = char('0' + v % 10); v /= 10; } while (v); while (n) c(d[--n]); }
    void x(uint64_t v, uint32_t width) {
        static const char hex[] = "0123456789abcdef";
        for (uint32_t i = width; i-- > 0;) c(hex[(v >> (i * 4)) & 15u]);
    }
    void nl() { c('\n'); }
};

static uint32_t       g_controllerCount = 0;
static IrqSpinLock g_eventRingLock;
static IrqSpinLock g_commandRingLock;
static volatile bool  g_irqActive = false;  // true once ISR is registered

// ---------------------------------------------------------------------------
// Utility: allocate physically contiguous, page-aligned DMA buffer
// ---------------------------------------------------------------------------

// Allocates 'pages' physically contiguous pages, returns both virtual and
// physical addresses. Memory is zeroed. Returns nullptr on failure.
// Set once any driven controller needs 32-bit DMA (no AC64, or ASMedia
// ASM1142/2142/3242, which Linux limits to 32-bit DMA). Every later xHCI
// allocation then comes from the first-fit run allocator below 4 GiB.
static bool g_dma32=false;
// Low-memory reserve for allocations after boot (hot-plug enumeration).
// Once a large RAM image (the live store) is resident, nothing below the
// 4 GiB DMA32 limit may be free, and every late enumeration failed at its
// first allocation ("scratch allocation failed"). Taken once at module
// init; bump-allocated, not recycled (gone devices keep their buffers).
static uint8_t* g_dmaReserve=nullptr;
static uint64_t g_dmaReservePhys=0;
static uint32_t g_dmaReservePages=0;
static XhciDmaReserve<2048> g_dmaReserveMap;
static SpinLock g_dmaReserveLock;
static void* AllocDmaReserve(uint32_t pages, uint64_t& outPhys)
{
    SpinLockAcquire(&g_dmaReserveLock);
    void* ptr=nullptr;
    const int first=g_dmaReserve?g_dmaReserveMap.alloc(pages):-1;
    if(first>=0){
        outPhys=g_dmaReservePhys+uint64_t(first)*4096;
        ptr=g_dmaReserve+uint64_t(first)*4096;
    }
    const uint32_t used=g_dmaReserveMap.inUse,total=g_dmaReservePages;
    SpinLockRelease(&g_dmaReserveLock);
    if(ptr){memset(ptr,0,uint64_t(pages)*4096);
        if(used%64<pages||total-used<64)KPrintf("xhci: DMA reserve %u/%u pages in use\n",used,total);}
    else KPrintf("xhci: DMA reserve exhausted (%u/%u pages, need %u)\n",used,total,pages);
    return ptr;
}
// Return a DMA buffer: reserve pages go back to the reserve map, others to
// the PMM. Only for buffers the controller no longer references.
static void FreeDmaBuffer(uint64_t phys, uint32_t pages)
{
    if(!phys||!pages)return;
    if(g_dmaReserve&&phys>=g_dmaReservePhys&&phys<g_dmaReservePhys+uint64_t(g_dmaReservePages)*4096){
        SpinLockAcquire(&g_dmaReserveLock);
        const bool ok=g_dmaReserveMap.release(uint32_t((phys-g_dmaReservePhys)/4096),pages);
        SpinLockRelease(&g_dmaReserveLock);
        if(!ok)KPrintf("xhci: DMA reserve release refused at %lx (%u pages)\n",static_cast<unsigned long>(phys),pages);
        return;
    }
    for(uint32_t i=0;i<pages;++i)PmmFreePage(PhysicalAddress(phys+uint64_t(i)*4096));
}
static void* AllocDmaBufferDirect(uint32_t pages, uint64_t& outPhys, PmmOwner owner);
static void* AllocDmaBuffer(uint32_t pages, uint64_t& outPhys, PmmOwner owner = PmmOwner::XhciDma)
{
    if(void* ptr=AllocDmaBufferDirect(pages,outPhys,owner))return ptr;
    if(!g_dmaReserve || !pages || pages>16384)return nullptr;
    return AllocDmaReserve(pages,outPhys);
}
static void* AllocDmaBufferDirect(uint32_t pages, uint64_t& outPhys, PmmOwner owner)
{
    if(!pages || pages>16384) return nullptr;
    // Multi-page requests scan from physical zero; a single page uses the
    // next-fit hint and may land high, so ask for two and return the spare.
    const uint64_t bytes=uint64_t(pages)*4096;
    const uint64_t limit=g_dma32?(4ull<<30):(16ull<<30);
    // A single page uses the next-fit hint and can land above the DMA limit
    // once a large RAM image (the live-stick store) has been allocated; a
    // two-page request scans from physical zero, so retry low once.
    PhysicalAddress phys{};
    for(uint32_t attempt=0;attempt<2;++attempt) {
        const uint32_t request=(g_dma32 || attempt) && pages==1?2:pages;
        phys=PmmAllocPagesAccounted(request,MemTag::Device,KernelPid,owner);
        if(!phys){KPrintf("xhci: DMA allocation of %u page(s) failed (attempt %u)\n",request,attempt);return nullptr;}
        if(request!=pages)PmmFreePage(phys+uint64_t(pages)*4096);
        if(phys.raw()<limit && bytes<=limit-phys.raw())break;
        KPrintf("xhci: DMA page(s) at %lx above the %lu GiB limit (attempt %u)\n",
                static_cast<unsigned long>(phys.raw()),static_cast<unsigned long>(limit>>30),attempt);
        for(uint32_t i=0;i<pages;++i)PmmFreePage(phys+uint64_t(i)*4096);
        if(pages!=1 || attempt)return nullptr;
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

// Every xHCI function (class 0c/03/30) in bus order, bounded.
static uint32_t XhciFindControllers(PciDevice* out, uint32_t capacity)
{
    uint32_t count = 0;
    for (uint16_t bus = 0; bus < 256 && count < capacity; bus++) {
        for (uint8_t d = 0; d < 32 && count < capacity; d++) {
            for (uint8_t f = 0; f < 8 && count < capacity; f++) {
                uint32_t reg0 = PciConfigRead32(bus, d, f, 0);
                if ((reg0 & 0xFFFF) == 0xFFFF) { if (f == 0) break; continue; }
                uint32_t reg8 = PciConfigRead32(bus, d, f, 8);
                uint8_t cls = (reg8 >> 24) & 0xFF;
                uint8_t sub = (reg8 >> 16) & 0xFF;
                uint8_t pi  = (reg8 >> 8) & 0xFF;
                if (cls == PCI_CLASS_SERIAL_BUS && sub == PCI_SUBCLASS_USB && pi == PCI_PROGIF_XHCI) {
                    PciDevice dev{};
                    dev.bus = static_cast<uint8_t>(bus);
                    dev.dev = d;
                    dev.fn = f;
                    dev.vendorId = reg0 & 0xFFFF;
                    dev.deviceId = (reg0 >> 16) & 0xFFFF;
                    dev.classCode = cls;
                    dev.subclass = sub;
                    dev.progIf = pi;
                    for (int i = 0; i < 6; i++)
                        dev.bar[i] = PciConfigRead32(bus, d, f, 0x10 + i * 4);
                    out[count++] = dev;
                }
                if (f == 0 && !(PciConfigRead8(bus, d, 0, 0x0E) & 0x80)) break; // single-function
            }
        }
    }
    return count;
}

static bool XhciPciDetect(XhciController& ctrl, const PciDevice& dev)
{
    ctrl.pciDev = dev;
    KPrintf("xhci: c%u controller %04x:%04x at PCI %02x:%02x.%u\n", ctrl.index,
                 dev.vendorId, dev.deviceId, dev.bus, dev.dev, dev.fn);
    if (!(dev.bar[0] & ~0xFu) && !(PciBarIs64(dev.bar[0]) && dev.bar[1])) {
        ctrl.failure = "BAR0 unassigned"; return false;
    }

    // Enable PCI bus master and memory space
    PciEnableMemSpace(dev);

    // Map BAR0 (64-bit MMIO) — map a generous region to cover all register spaces
    ctrl.mapBytes = 64 * 1024;
    ctrl.capBase = MapBar64(dev, 0, ctrl.mapBytes);
    if (!ctrl.capBase) {
        SerialPuts("xhci: failed to map BAR0\n");
        ctrl.failure = "BAR0 map failed";
        return false;
    }

    // Read capability registers
    ctrl.capLength  = xhci_read8(ctrl.capBase, XHCI_CAP_CAPLENGTH);
    // Dword read: some controllers (QEMU) do not decode 16-bit capability reads.
    ctrl.hciVersion = static_cast<uint16_t>(xhci_read32(ctrl.capBase, 0) >> 16);

    uint32_t hcsParams1 = xhci_read32(ctrl.capBase, XHCI_CAP_HCSPARAMS1);
    ctrl.maxSlots = hcsParams1 & 0xFF;
    ctrl.maxIntrs = (hcsParams1 >> 8) & 0x7FF;
    ctrl.maxPorts = (hcsParams1 >> 24) & 0xFF;

    uint32_t hcsParams2 = xhci_read32(ctrl.capBase, XHCI_CAP_HCSPARAMS2);
    ctrl.scratchpadCount=XhciScratchpadCount(hcsParams2);

    uint32_t hccParams1 = xhci_read32(ctrl.capBase, XHCI_CAP_HCCPARAMS1);
    ctrl.ctx64 = (hccParams1 & HCC_CSZ) != 0;
    if(!(hccParams1&HCC_AC64) || (dev.vendorId==0x1b21 && (dev.deviceId==0x1142 || dev.deviceId==0x2142 || dev.deviceId==0x3242))) {
        // Keep every later xHCI DMA buffer below 4 GiB for this controller.
        ctrl.dma32=true;g_dma32=true;
        KPrintf("xhci: c%u uses 32-bit DMA (%s)\n",ctrl.index,(hccParams1&HCC_AC64)?"ASMedia quirk":"no AC64");
    }

    // Doorbell offset
    uint32_t dboff = xhci_read32(ctrl.capBase, XHCI_CAP_DBOFF) & ~0x3;
    uint32_t rtsoff = xhci_read32(ctrl.capBase, XHCI_CAP_RTSOFF) & ~0x1F;
    // The register spaces need not fit in 64 KiB (the RTX 20xx USB-C xHCI,
    // 10de:1ad8, places them higher in its BAR). The capability registers
    // define the extent; map that much (bounded) instead of assuming 64 KiB.
    {
        uint64_t needed = uint64_t(ctrl.capLength) + XHCI_OP_PORTSC_BASE + uint64_t(ctrl.maxPorts) * 16;
        const uint64_t doorbells = uint64_t(dboff) + (uint64_t(ctrl.maxSlots) + 1) * 4;
        const uint64_t runtime = uint64_t(rtsoff) + 0x20 + uint64_t(ctrl.maxIntrs ? ctrl.maxIntrs : 1) * 32;
        const uint64_t extended = uint64_t(hccParams1 >> 16) * 4 + 16;
        if (doorbells > needed) needed = doorbells;
        if (runtime > needed) needed = runtime;
        if (extended > needed) needed = extended;
        needed = (needed + 4095) & ~uint64_t(4095);
        if (needed > ctrl.mapBytes && needed <= 4u * 1024 * 1024) {
            auto* wider = MapBar64(dev, 0, static_cast<uint32_t>(needed));
            if (wider) {
                // The first window stays mapped (a few pages of VA); harmless.
                ctrl.capBase = wider;
                ctrl.mapBytes = static_cast<uint32_t>(needed);
                KPrintf("xhci: c%u register window %u KiB (dboff=0x%x rtsoff=0x%x)\n", ctrl.index,
                        ctrl.mapBytes / 1024, dboff, rtsoff);
            }
        }
    }

    // Operational registers are at capBase + capLength
    ctrl.opBase = ctrl.capBase + ctrl.capLength;
    const uint32_t window = ctrl.mapBytes;
    if(ctrl.capLength<0x20 || uint32_t(ctrl.capLength)+XHCI_OP_PORTSC_BASE+ctrl.maxPorts*16>window ||
       dboff>window-(ctrl.maxSlots+1)*4) {
        SerialPuts("xhci: invalid operational/doorbell register bounds\n");ctrl.failure="register bounds";return false;
    }
    ctrl.dbBase = ctrl.capBase + dboff;

    // Runtime register offset
    if(rtsoff>window-0x40 || !ctrl.maxSlots || !ctrl.maxPorts || !ctrl.maxIntrs) {
        SerialPuts("xhci: invalid runtime/capability bounds\n");ctrl.failure="capability bounds";return false;
    }
    ctrl.rtBase = ctrl.capBase + rtsoff;

    SerialPrintf("xhci: version %x.%02x, capLen=%u\n",
                 ctrl.hciVersion >> 8, ctrl.hciVersion & 0xFF, ctrl.capLength);
    SerialPrintf("xhci: maxSlots=%u maxPorts=%u maxIntrs=%u ctx=%uB scratchpad=%u\n",
                 ctrl.maxSlots, ctrl.maxPorts, ctrl.maxIntrs,
                 ctrl.ctx64 ? 64 : 32, ctrl.scratchpadCount);

    return true;
}

// ---------------------------------------------------------------------------
// Phase 2: Controller reset and ring allocation
// ---------------------------------------------------------------------------

static bool XhciReset(XhciController& ctrl)
{
    struct ResetIo {
        XhciController& ctrl;
        uint32_t command() { return xhci_read32(ctrl.opBase,XHCI_OP_USBCMD); }
        uint32_t status() { return xhci_read32(ctrl.opBase,XHCI_OP_USBSTS); }
        void writeCommand(uint32_t value) { xhci_write32(ctrl.opBase,XHCI_OP_USBCMD,value); }
        uint64_t now() { return ApicTickCount(); }
        void pause() { __asm__ volatile("pause"); }
    } io{ctrl};
    const uint64_t started=ApicTickCount();
    const auto result = XhciResetController(io, ctrl.pciDev.vendorId==0x8086, 10000, 500000000);
    const bool sampleStatus=result.phase==XhciResetPhase::ExistingReset &&
                            result.error==XhciResetError::Timeout;
    const uint32_t statusNow=sampleStatus?io.status():0;
    UsbBootReportReset(ctrl.pciDev.vendorId,ctrl.pciDev.deviceId,
        static_cast<uint32_t>(result.phase),static_cast<uint32_t>(result.error),
        result.command,result.status,statusNow,sampleStatus && statusNow!=0xffffffffu,ApicTickCount()-started);
    {
        XhciText t{ctrl.resetText,sizeof(ctrl.resetText),0};
        if(result.error==XhciResetError::None) t.s("ok");
        else {
            static const char* const phases[]={"initial","halt","intel-quiet","reset","ready","complete",
                "initial-quiet","initial-ready","existing-reset","existing-ready"};
            static const char* const errors[]={"none","inaccessible","timeout","clock-stopped","not-ready"};
            const auto phase=static_cast<unsigned>(result.phase),error=static_cast<unsigned>(result.error);
            t.s("FAIL("); t.s(phase<10?phases[phase]:"?"); t.c(' '); t.s(error<5?errors[error]:"?");
            t.s(" cmd="); t.x(result.command,8); t.s(" sts="); t.x(result.status,8); t.c(')');
        }
        ctrl.resetText[t.len]=0;
    }
    if (result.error != XhciResetError::None) {
        // The wait may have sampled only USBCMD; label its last status and
        // take a fresh status sample after the bounded wait for diagnostics.
        KPrintf("xhci: reset failed phase=%u error=%u USBCMD=0x%x USBSTS_last=0x%x USBSTS_now=0x%x valid=%u elapsed=%lu ms\n",
                static_cast<unsigned>(result.phase),static_cast<unsigned>(result.error),
                result.command,result.status,statusNow,(sampleStatus && statusNow!=0xffffffffu)?1u:0u,ApicTickCount()-started);
        return false;
    }
    // PAGESIZE is operational state: inspect it only after reset and CNR clear.
    const uint32_t psReg=xhci_read32(ctrl.opBase,XHCI_OP_PAGESIZE);
    if(psReg==0xffffffffu || !(psReg&1)){SerialPuts("xhci: page size register unavailable or lacks 4KiB support\n");return false;}
    ctrl.pageSize=4096;
    SerialPrintf("xhci: page=%u\n",ctrl.pageSize);
    SerialPuts("xhci: controller reset complete\n");
    return true;
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
    XhciInitializeLink(link,ctrl.cmdRingPhys);

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
    if (maxSlots > MAX_SLOTS_PER_CONTROLLER) maxSlots = MAX_SLOTS_PER_CONTROLLER;
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
            ctrl.runText = (sts & USBSTS_HSE) ? "running+HSE" : "ok";
            return true;
        }
        for (volatile int d = 0; d < 100000; d++);
    }

    SerialPuts("xhci: controller failed to start!\n");
    ctrl.runText = "FAIL(HCH stuck)";
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
struct XhciUasRing { Trb* ring;uint64_t phys;uint32_t enqueue;bool cycle; };
struct XhciUas {
    UsbUasConfig config;
    XhciUasRing rings[UasPipeCount][UsbUasDepth];
    uint64_t contexts[UasPipeCount];
    uint8_t* buffers;
    uint64_t bufferPhys;
};
static bool g_storageUas=false;
static bool g_storageUasFixture=false;
// One boot-protocol HID function (keyboard or mouse) with its own ring.
struct XhciHidFn {
    uint8_t iface, ep, interval;
    uint16_t packet;            // wMaxPacketSize
    Trb*     ring;
    uint64_t ringPhys;
    uint32_t enqueue;
    bool     cycle;
    uint32_t dci;
    uint8_t* report;
    uint64_t reportPhys;
    uint32_t length;            // bytes requested per report
    uint8_t  previous[8];       // per-interface keys or buttons
    bool     active;
    uint8_t  errorCc;
    uint8_t  pointerStages;
    UsbPointerEndpoint pointerEndpoint;
};
struct XhciDevice {
    XhciController* ctrl;
    UsbTopology topo;
    uint8_t parent;             // parent hub device index + 1 (0 = root port)
    uint8_t parentPort;         // port on the parent hub (1-based)
    uint16_t ep0Packet;
    uint16_t bcdUsb;
    uint8_t lastCc;             // last failing completion code (255 = timeout)
    bool gone;
    const char* note;           // function setup problem for the status block
    uint8_t* scratch;           // one DMA page for descriptors and hub requests
    uint64_t scratchPhys;
    XhciHidFn kbd, mouse;
    XhciHidFn fidoIn, fidoOut;
    uint32_t fidoIdentity;
    uint64_t fidoTxStarted;
    bool fidoTxPending;
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
    XhciUas* uas;
    uint32_t mscDiscardDcis;
    KMutex controlLock;
    bool controlQuarantined;
    uint8_t mscInterface, mscAlternate;
    bool mscQuarantined;       // retained DMA buffers cannot be reused after transport failure
    UsbScsiCache mscCache;      // reset to Unknown with each enumerated device
    bool mscUnsupportedSync;    // only after current sense + explicit WCE=0
    uint32_t mscTag;            // CBW tag counter
    void* mscCommandBuffer;     // one persistent CBW/CSW page, serialized below
    uint64_t mscCommandBufferPhys;
    UsbStorageTiming mscTiming; // g_mscCommandLock
    uint64_t mscSectorCount;    // total sectors
    uint32_t mscBlockSize;      // bytes per sector (usually 512)

    // Hub state
    bool     hubReady, hubSuper, hubMulti;
    uint8_t  hubPortCount, hubThink;
    uint8_t  hubChild[XHCI_HUB_MAX_PORTS];    // device index + 1
    uint8_t  hubTries[XHCI_HUB_MAX_PORTS], hubCc[XHCI_HUB_MAX_PORTS];
    uint16_t hubPortStatus[XHCI_HUB_MAX_PORTS];
    const char* hubError[XHCI_HUB_MAX_PORTS];

    void*    priv;          // driver-private (DMA report buffer for HID)

    // Raw descriptors kept for read-only diagnostics (bridge hardware usb).
    uint8_t  rawDevice[18];
    uint8_t* rawConfig;     // kmalloc'd copy of the selected configuration
    uint16_t rawConfigLen;
};

// The QEMU fixture has no BOT alternate, so firmware boots a separate
// read-only carrier. Physical dual-alt disks are promoted after admission.
static bool XhciUasFixture(const XhciDevice& dev) {
    return g_storageUasFixture && dev.vendorId==0x46f4 && dev.productId==3;
}
static constexpr unsigned ECM_RX_PACKETS=UsbEcmRxCredits::Capacity;
static constexpr unsigned ECM_RX_SLOTS=UsbEcmRxCredits::Slots;
static constexpr unsigned ECM_RX_BYTES=UsbEcmRxCredits::BufferBytes;
static_assert(ECM_RX_SLOTS < XFER_RING_SIZE-1 && XFER_RING_SIZE==UsbEcmRxCredits::RingEntries);
static_assert(sizeof(Trb)==16);
static constexpr unsigned ECM_TX_SLOTS=UsbEcmTxSlots::Capacity;
static_assert(ECM_TX_SLOTS*2 < XFER_RING_SIZE-1, "ECM frames and ZLPs must fit the transfer ring");
struct UsbEcmState {
    NetIf interface;
    XhciController* controller;XhciDevice* device;
    uint8_t* rx;uint64_t rxPhysical;
    UsbEcmRxCredits rxCredits;
    unsigned rxDepth,txDepth;KMutex dispatchLock;bool earlyDispatch;
    uint8_t* notification;uint64_t notificationPhysical,notificationToken;
    bool linkKnown,linkUp;uint64_t notifications,reportTick;uint32_t notifyHeader[2];
    uint8_t notificationSamples[16][16],notificationLengths[16];uint32_t notificationPrinted;
    uint8_t* tx;uint64_t txPhysical;
    UsbEcmTxSlots sends;
    uint8_t packets[ECM_RX_PACKETS][1514];uint16_t lengths[ECM_RX_PACKETS];uint32_t head,tail;
    uint64_t dropped,received,transmitted,txBusy;uint32_t queueHighWater;bool failed;
    UsbEcmRxPressure rxPressure;uint64_t rxDispatchBusy;
    uint32_t faultStatus,faultDci;uint64_t faultToken;
    uint64_t rxPosted,rxCompletions,rxErrors,txSubmitted,txErrors;
};
static bool g_networkProbe=false;
static bool XhciEcmCompletion(XhciDevice&,const Trb&);
static bool XhciInitEcm(XhciController&,XhciDevice&);

static XhciDevice g_devices[MAX_DEVICES];
static uint32_t   g_deviceCount = 0;

// ---------------------------------------------------------------------------
// Phase 4: Enable / Disable Slot
// ---------------------------------------------------------------------------

static constexpr uint32_t TRB_TYPE_STOP_EP    = (15 << TRB_TYPE_SHIFT);
static constexpr uint32_t TRB_TYPE_SET_TR_DEQ = (16 << TRB_TYPE_SHIFT);
static constexpr uint32_t TRB_CC_STALL = 6;
static constexpr uint32_t TRB_CC_CONTEXT_STATE = 19;

static int XhciEnableSlot(XhciController& ctrl)
{
    const auto commandToken=XhciSubmitCommand(ctrl, 0, 0, TRB_TYPE_ENABLE_SLOT);

    Trb completion;
    Trb* evt = XhciWaitForEvent(ctrl, TRB_TYPE_CMD_COMPLETION, 1000,&completion,commandToken);
    if (!evt) {
        SerialPuts("xhci: Enable Slot timeout\n");
        return -1;
    }

    uint32_t cc = (evt->status >> TRB_CC_SHIFT) & 0xFF;
    uint32_t slotId = (evt->control >> 24) & 0xFF;

    if (cc != TRB_CC_SUCCESS || !slotId || slotId > MAX_SLOTS_PER_CONTROLLER) {
        SerialPrintf("xhci: Enable Slot failed (cc=%u slot=%u)\n", cc, slotId);
        return -1;
    }

    SerialPrintf("xhci: slot %u enabled\n", slotId);
    return static_cast<int>(slotId);
}

static bool XhciDisableSlot(XhciController& ctrl, uint32_t slotId)
{
    if (!slotId) return false;
    const auto token = XhciSubmitCommand(ctrl, 0, 0, TRB_TYPE_DISABLE_SLOT | (slotId << 24));
    Trb completion;
    if (!XhciWaitForEvent(ctrl, TRB_TYPE_CMD_COMPLETION, 1000, &completion, token)) return false;
    ctrl.dcbaa[slotId] = 0;
    return true;
}

// Generic command with slot/endpoint fields; returns the completion code.
static uint32_t XhciSlotCommand(XhciController& ctrl, uint64_t param, uint32_t control, uint32_t timeoutMs = 1000)
{
    const auto token = XhciSubmitCommand(ctrl, param, 0, control);
    Trb completion;
    Trb* evt = XhciWaitForEvent(ctrl, TRB_TYPE_CMD_COMPLETION, timeoutMs, &completion, token);
    return evt ? (evt->status >> TRB_CC_SHIFT) & 0xFF : 0;
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
    // Producer and endpoint DCS start at one. Keep the Link unowned until
    // software actually wraps, so the consumer cannot cross a future submission
    // gate before the producer has prepared the next lap.
    XhciInitializeLink(link,ringPhys);

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
static bool XhciAddressDevice(XhciController& ctrl, XhciDevice& dev)
{
    uint32_t ctxSize = CtxEntrySize(ctrl);

    // Input context: control + slot + 31 endpoint contexts.
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

    auto* icc = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(dev.inputCtx));
    icc[1] = 0x3; // Add Slot Context (A0) + EP0 Context (A1)

    // Slot context: route string, speed, root port and, for LS/FS devices
    // behind a high-speed hub, the TT hub slot/port (xHCI 1.2 §4.3.3).
    auto* slotCtx = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(dev.inputCtx) + ctxSize);
    const auto words = UsbSlotContext(dev.topo, 1, false, 0, 0, false);
    slotCtx[0] = words.dw0;
    slotCtx[1] = words.dw1;
    slotCtx[2] = words.dw2;

    auto* ep0Ctx = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(dev.inputCtx) + ctxSize * 2);
    dev.ep0Packet = static_cast<uint16_t>(UsbDefaultEp0Packet(dev.portSpeed));
    ep0Ctx[1] = (3 << 1)  | // CErr = 3
                (4 << 3)  | // EP Type = Control Bidirectional
                (uint32_t(dev.ep0Packet) << 16);
    uint64_t trDqp = dev.ep0RingPhys | 1; // DCS = 1
    ep0Ctx[2] = static_cast<uint32_t>(trDqp);
    ep0Ctx[3] = static_cast<uint32_t>(trDqp >> 32);
    ep0Ctx[4] = 8; // average TRB length for control

    __asm__ volatile("mfence" ::: "memory");

    SerialPrintf("xhci: addressing slot %u root=%u route=0x%05x depth=%u speed=%s tt=%u/%u\n",
                 dev.slotId, dev.topo.rootPort, dev.topo.route, dev.topo.depth,
                 UsbSpeedName(dev.portSpeed), dev.topo.ttSlot, dev.topo.ttPort);

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
        dev.lastCc = static_cast<uint8_t>(cc);
        return false;
    }

    SerialPrintf("xhci: device addressed on slot %u\n", dev.slotId);
    return true;
}

// Update EP0 max packet once bMaxPacketSize0 is known (xHCI 1.2 §4.6.7).
static bool XhciEvaluateEp0(XhciController& ctrl, XhciDevice& dev, uint32_t packet)
{
    const uint32_t ctxSize = CtxEntrySize(ctrl);
    auto* input = static_cast<uint8_t*>(dev.inputCtx);
    memset(input, 0, ctxSize * 33);
    reinterpret_cast<uint32_t*>(input)[1] = 1u << 1; // A1: EP0
    auto* ep0 = reinterpret_cast<uint32_t*>(input + ctxSize * 2);
    const auto* out = reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(dev.outputCtx) + ctxSize);
    for (unsigned i = 0; i < 5; ++i) ep0[i] = out[i];
    ep0[0] &= ~7u; // EP state is xHC-owned
    ep0[1] = (ep0[1] & 0xffffu) | (packet << 16);
    __asm__ volatile("mfence" ::: "memory");
    const uint32_t cc = XhciSlotCommand(ctrl, dev.inputCtxPhys, TRB_TYPE_EVAL_CTX | (dev.slotId << 24));
    if (cc != TRB_CC_SUCCESS) {
        SerialPrintf("xhci: Evaluate Context (EP0 mps=%u) failed cc=%u\n", packet, cc);
        return false;
    }
    dev.ep0Packet = static_cast<uint16_t>(packet);
    return true;
}

// ---------------------------------------------------------------------------
// Phase 4-5: Control Transfer (EP0)
// ---------------------------------------------------------------------------

// A halted or timed-out control endpoint is reset and its dequeue pointer
// moved past the abandoned TD, so one stalled request (an unsupported
// descriptor, a hub class probe) does not disable the device.
static bool XhciRecoverEp0(XhciController& ctrl, XhciDevice& dev, bool halted,
                           const uint64_t* stale, unsigned staleCount)
{
    const uint32_t target = (dev.slotId << 24) | (1u << 16);
    uint32_t cc = XhciSlotCommand(ctrl, 0, (halted ? TRB_TYPE_RESET_EP : TRB_TYPE_STOP_EP) | target);
    if (!halted && cc == TRB_CC_CONTEXT_STATE) {
        // Stop on a Halted EP0 fails; the output context says which state.
        const auto* ep0 = reinterpret_cast<const volatile uint32_t*>(
            static_cast<uint8_t*>(dev.outputCtx) + CtxEntrySize(ctrl));
        if ((ep0[0] & 7u) == 2) { halted = true; cc = XhciSlotCommand(ctrl, 0, TRB_TYPE_RESET_EP | target); }
    }
    if (cc != TRB_CC_SUCCESS && cc != TRB_CC_CONTEXT_STATE) {
        SerialPrintf("xhci: EP0 %s failed slot=%u cc=%u\n", halted ? "reset" : "stop", dev.slotId, cc);
        return false;
    }
    const uint64_t dequeue = (dev.ep0RingPhys + uint64_t(dev.ep0Enqueue) * sizeof(Trb)) | (dev.ep0Cycle ? 1u : 0u);
    cc = XhciSlotCommand(ctrl, dequeue, TRB_TYPE_SET_TR_DEQ | target);
    if (cc != TRB_CC_SUCCESS) {
        SerialPrintf("xhci: EP0 set dequeue failed slot=%u cc=%u\n", dev.slotId, cc);
        return false;
    }
    // Drop completions that still name the abandoned TD.
    const auto saved = IrqSpinLockAcquire(&g_eventRingLock);
    Trb discarded;
    for (unsigned i = 0; i < staleCount; ++i)
        while (stale[i] && ctrl.completions.Take(TRB_TYPE_TRANSFER_EVENT, stale[i], discarded)) {}
    IrqSpinLockRelease(&g_eventRingLock, saved);
    return true;
}

// Submit a Setup+Data+Status control transfer on EP0 and wait for completion.
// dataDir: 0 = no data, 2 = OUT (host→device), 3 = IN (device→host)
// With 'actual', a short IN data stage succeeds and reports its length;
// without it, only an exact-length transfer succeeds.
static bool XhciControlTransfer(XhciController& ctrl, XhciDevice& dev,
                                 const UsbSetupPacket& setup,
                                 void* data, uint32_t dataLen,
                                 uint64_t dataPhys, uint32_t dataDir,
                                 uint32_t* actual = nullptr)
{
    struct ControlGuard {
        KMutex* mutex;
        explicit ControlGuard(KMutex* m):mutex(m){KMutexLock(mutex);}
        ~ControlGuard(){KMutexUnlock(mutex);}
    } guard(&dev.controlLock);
    if(dev.controlQuarantined || dev.gone || dataLen>65536 || (dataLen && (!data || !dataDir)) ||
       dataLen!=setup.wLength || dataLen>65536-(dataPhys&65535))return false;
    uint64_t dataToken=0;
    // 1. Setup TRB (Immediate Data)
    uint32_t idx = dev.ep0Enqueue;
    if(idx>=XFER_RING_SIZE-1) {
        dev.ep0Ring[XFER_RING_SIZE-1].control=TRB_TYPE_LINK|TRB_TC|(dev.ep0Cycle?TRB_CYCLE:0);
        dev.ep0Cycle=!dev.ep0Cycle;idx=0;
    }

    const auto setupToken=dev.ep0RingPhys+idx*sizeof(Trb);
    Trb& setupTrb = dev.ep0Ring[idx];
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
            dev.ep0Ring[XFER_RING_SIZE - 1].control =
                TRB_TYPE_LINK | TRB_TC | (dev.ep0Cycle ? TRB_CYCLE : 0);
            dev.ep0Cycle = !dev.ep0Cycle;
            idx = 0;
        }

        dataToken=dev.ep0RingPhys+idx*sizeof(Trb);
        Trb& dataTrb = dev.ep0Ring[idx];
        dataTrb.param = dataPhys;
        dataTrb.status = dataLen;
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
    uint32_t statusDir = (dataDir == 3) ? 0 : (1 << 16);
    statusTrb.control = TRB_TYPE_STATUS | TRB_IOC | statusDir |
                        (dev.ep0Cycle ? TRB_CYCLE : 0);
    idx++;

    dev.ep0Enqueue = idx;

    __asm__ volatile("mfence" ::: "memory");

    XhciRingDoorbell(ctrl, dev.slotId, 1);

    uint32_t received=dataLen;bool halted=false,timedOut=false;uint32_t failCc=0;
    if(dataLen) {
        Trb dataCompletion;
        // An error on the Setup stage names the Setup TRB (it has no IOC).
        const uint64_t tokens[2]={setupToken,dataToken};
        const auto* event=XhciWaitForEvent(ctrl,TRB_TYPE_TRANSFER_EVENT,2000,&dataCompletion,0,tokens,2);
        if(!event)timedOut=true;
        else {
            const uint32_t cc=event->status>>24;
            if(event->param==setupToken){halted=true;failCc=cc;}
            else if(cc==TRB_CC_SUCCESS || cc==TRB_CC_SHORT_PACKET) {
                const uint32_t residue=event->status&0xffffff;
                received=residue<=dataLen?dataLen-residue:0;
            } else {halted=true;failCc=cc;}
        }
    }
    if(!timedOut && !halted) {
        Trb completion;
        const uint64_t tokens[2]={setupToken,statusToken};
        Trb* evt = XhciWaitForEvent(ctrl, TRB_TYPE_TRANSFER_EVENT, 2000,&completion,0,tokens,2);
        if (!evt) timedOut=true;
        else {
            const uint32_t cc = (evt->status >> TRB_CC_SHIFT) & 0xFF;
            if (cc != TRB_CC_SUCCESS || evt->param==setupToken) {halted=true;failCc=cc;}
        }
    }
    if(timedOut || halted) {
        dev.lastCc=static_cast<uint8_t>(timedOut?255u:failCc);
        SerialPrintf("xhci: control req=%02x/%02x slot=%u %s cc=%u\n",setup.bmRequestType,setup.bRequest,
                     dev.slotId,timedOut?"timeout":"halted",failCc);
        const uint64_t stale[3]={setupToken,dataToken,statusToken};
        if(!XhciRecoverEp0(ctrl,dev,halted,stale,3))dev.controlQuarantined=true;
        return false;
    }
    if(actual){*actual=received;return true;}
    return received==dataLen;
}

// ---------------------------------------------------------------------------
// Phase 5: GET_DEVICE_DESCRIPTOR
// ---------------------------------------------------------------------------

static bool XhciGetDeviceDescriptor(XhciController& ctrl, XhciDevice& dev)
{
    auto* buf = dev.scratch;
    const uint64_t bufPhys = dev.scratchPhys;

    UsbSetupPacket setup = {};
    setup.bmRequestType = 0x80; // Device-to-Host, Standard, Device
    setup.bRequest = USB_REQ_GET_DESCRIPTOR;
    setup.wValue = (USB_DESC_DEVICE << 8) | 0;
    setup.wLength = 8;
    // The first eight bytes carry bMaxPacketSize0; full-speed devices may
    // use 8..64, so fix EP0 before reading the whole descriptor.
    uint32_t got = 0;
    if (!XhciControlTransfer(ctrl, dev, setup, buf, 8, bufPhys, 3, &got) || got < 8) {
        SerialPuts("xhci: GET_DESCRIPTOR(device, 8) failed\n");
        return false;
    }
    const uint32_t packet = UsbEp0PacketFromDescriptor(dev.portSpeed, buf[7]);
    if (!packet) {
        SerialPrintf("xhci: invalid bMaxPacketSize0=%u\n", buf[7]);
        return false;
    }
    if (packet != dev.ep0Packet && !XhciEvaluateEp0(ctrl, dev, packet)) return false;

    setup.wLength = sizeof(UsbDeviceDescriptor);
    if (!XhciControlTransfer(ctrl, dev, setup, buf, sizeof(UsbDeviceDescriptor),
                              bufPhys, 3 /* IN */)) {
        SerialPuts("xhci: GET_DEVICE_DESCRIPTOR failed\n");
        return false;
    }

    auto* desc = reinterpret_cast<UsbDeviceDescriptor*>(buf);
    memcpy(dev.rawDevice, buf, sizeof(dev.rawDevice));
    dev.vendorId = desc->idVendor;
    dev.productId = desc->idProduct;
    dev.bcdUsb = desc->bcdUSB;
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
    auto* buf = dev.scratch;
    const uint64_t bufPhys = dev.scratchPhys;

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
        if(!UsbEcmParse(buf,bytes,UsbSpeedIsSuper(dev.portSpeed),ecm))continue;
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

    if (totalLen < sizeof(UsbConfigDescriptor)) return false;
    if (totalLen > 4096) totalLen = 4096;

    setup.wLength = totalLen;
    if (!XhciControlTransfer(ctrl, dev, setup, buf, totalLen, bufPhys, 3)) {
        SerialPuts("xhci: GET_CONFIG_DESCRIPTOR (full) failed\n");
        return false;
    }

    // Parse interface and endpoint descriptors. Endpoints belong to the
    // interface (alternate setting 0) that precedes them, so a composite
    // keyboard keeps its boot keyboard endpoint and a separate mouse
    // interface keeps its own.
    enum class Owner { None, Keyboard, Mouse, Storage, Hub };
    Owner owner = Owner::None;
    uint16_t offset = cfgHdr->bLength;
    while (offset + 2 <= totalLen) {
        uint8_t dLen  = buf[offset];
        uint8_t dType = buf[offset + 1];
        if (dLen < 2 || offset + dLen > totalLen) break;

        if (dType == USB_DESC_INTERFACE && dLen >= sizeof(UsbInterfaceDescriptor)) {
            auto* iface = reinterpret_cast<UsbInterfaceDescriptor*>(buf + offset);
            owner = Owner::None;
            SerialPrintf("xhci:   interface %u alt %u: class=%u/%u/%u endpoints=%u\n",
                         iface->bInterfaceNumber, iface->bAlternateSetting,
                         iface->bInterfaceClass, iface->bInterfaceSubClass,
                         iface->bInterfaceProtocol, iface->bNumEndpoints);
            if (iface->bInterfaceClass == USB_CLASS_MASS_STORAGE &&
                iface->bInterfaceSubClass == USB_MSC_SUBCLASS_SCSI &&
                iface->bInterfaceProtocol == USB_MSC_PROTOCOL_BBB &&
                (!dev.isMassStorage || (dev.mscAlternate && !iface->bAlternateSetting))) {
                dev.isMassStorage = true; owner = Owner::Storage;
                dev.mscInterface = iface->bInterfaceNumber;
                dev.mscAlternate = iface->bAlternateSetting;
                dev.bulkInAddr = dev.bulkOutAddr = 0;
            }
            if (iface->bAlternateSetting == 0) {
                if (!dev.ifaceClass) {
                    dev.ifaceClass = iface->bInterfaceClass;
                    dev.ifaceSubClass = iface->bInterfaceSubClass;
                    dev.ifaceProtocol = iface->bInterfaceProtocol;
                }
                if (iface->bInterfaceClass == USB_CLASS_HID &&
                    iface->bInterfaceSubClass == USB_HID_SUBCLASS_BOOT) {
                    if (iface->bInterfaceProtocol == USB_HID_PROTOCOL_KEYBOARD && !dev.kbd.ep) {
                        owner = Owner::Keyboard; dev.kbd.iface = iface->bInterfaceNumber;
                    } else if (iface->bInterfaceProtocol == USB_HID_PROTOCOL_MOUSE && !dev.mouse.ep) {
                        owner = Owner::Mouse; dev.mouse.iface = iface->bInterfaceNumber;
                    }
                }
                if (iface->bInterfaceClass == USB_CLASS_HUB || dev.deviceClass == USB_CLASS_HUB) {
                    dev.isHub = true; owner = Owner::Hub;
                }
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

            XhciHidFn* fn = owner == Owner::Keyboard ? &dev.kbd : owner == Owner::Mouse ? &dev.mouse : nullptr;
            if (fn && epType == 3 && isIn && !fn->ep) {
                fn->ep = epAddr; fn->packet = ep->wMaxPacketSize; fn->interval = ep->bInterval;
            }
            if (owner == Owner::Storage && epType == 2) {
                if (isIn && !dev.bulkInAddr) {
                    dev.bulkInAddr = epAddr; dev.bulkInMaxPacket = ep->wMaxPacketSize;
                } else if (!isIn && !dev.bulkOutAddr) {
                    dev.bulkOutAddr = epAddr; dev.bulkOutMaxPacket = ep->wMaxPacketSize;
                }
            }
        }

        offset += dLen;
    }
    dev.isKeyboard = dev.kbd.ep != 0;
    dev.isMouse = dev.mouse.ep != 0;
    if (!dev.rawConfig) dev.rawConfig = static_cast<uint8_t*>(kmalloc(totalLen));
    if (dev.rawConfig) { memcpy(dev.rawConfig, buf, totalLen); dev.rawConfigLen = totalLen; }

    UsbSetupPacket setCfg = {};
    setCfg.bmRequestType = 0x00; // Host-to-Device, Standard, Device
    setCfg.bRequest = USB_REQ_SET_CONFIG;
    setCfg.wValue = cfgValue;

    if (!XhciControlTransfer(ctrl, dev, setCfg, nullptr, 0, 0, 0)) {
        SerialPuts("xhci: SET_CONFIGURATION failed\n");
        return false;
    }

    if(dev.isMassStorage && dev.mscAlternate && dev.bulkInAddr && dev.bulkOutAddr) {
        UsbSetupPacket setInterface{};
        setInterface.bmRequestType=0x01;
        setInterface.bRequest=0x0b; // SET_INTERFACE
        setInterface.wValue=dev.mscAlternate;
        setInterface.wIndex=dev.mscInterface;
        if(!XhciControlTransfer(ctrl,dev,setInterface,nullptr,0,0,0)) {
            dev.isMassStorage=false;
            KPuts("xhci: USB storage could not select Bulk-Only alternate setting\n");
        }
    }
    if(XhciUasFixture(dev) && !dev.isMassStorage && UsbSpeedIsSuper(dev.portSpeed)) {
        UsbUasConfig config{};
        dev.isMassStorage=UsbUasParse(dev.rawConfig,dev.rawConfigLen,-1,config);
    }
    SerialPrintf("xhci: device configured (cfg=%u)\n", cfgValue);
    dev.configured = true;
    return true;
}

// ---------------------------------------------------------------------------
// Phase 6: Configure an interrupt IN endpoint (HID reports, ECM notify)
// ---------------------------------------------------------------------------

static bool XhciConfigureInterrupt(XhciController& ctrl, XhciDevice& dev, uint8_t epAddr,
                               uint16_t wMaxPacket, uint8_t bInterval,
                               Trb*& ring, uint64_t& ringPhys, uint32_t& enqueue,
                               bool& cycle, uint32_t& dci)
{
    if (!(epAddr & 0x0f)) return false;
    const bool inputEndpoint = epAddr & 0x80;
    dci = (epAddr & 0x0f) * 2 + inputEndpoint;
    uint32_t interval = 0;
    if (!UsbXhciInterval(dev.portSpeed, bInterval, interval)) {
        SerialPrintf("xhci: interrupt EP 0x%02x invalid interval %u\n", epAddr, bInterval);
        return false;
    }
    const uint32_t packet = wMaxPacket & 0x7ffu;
    const uint32_t burst = dev.portSpeed == UsbSpeedHigh ? (wMaxPacket >> 11) & 3u : 0u;
    if (!packet) return false;

    SerialPrintf("xhci: configuring interrupt EP 0x%02x (DCI %u) mps=%u interval=%u\n",
                 epAddr, dci, packet, interval);

    if (!XhciAllocTransferRing(ring, ringPhys, enqueue, cycle)) {
        SerialPuts("xhci: failed to allocate interrupt transfer ring\n");
        return false;
    }

    uint32_t ctxSize = CtxEntrySize(ctrl);
    auto* input = static_cast<uint8_t*>(dev.inputCtx);
    memset(input, 0, ctxSize * 33);

    auto* icc = reinterpret_cast<uint32_t*>(input);
    icc[1] = (1 << 0) | (1 << dci); // Add Slot + EP

    auto* slotCtx = reinterpret_cast<uint32_t*>(input + ctxSize);
    auto* outSlot = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(dev.outputCtx));
    slotCtx[0] = outSlot[0];
    slotCtx[1] = outSlot[1];
    slotCtx[2] = outSlot[2];
    slotCtx[3] = outSlot[3];
    if(dci>((slotCtx[0]>>27)&31))slotCtx[0] = (slotCtx[0] & ~(0x1Fu << 27)) | (dci << 27);

    auto* epCtx = reinterpret_cast<uint32_t*>(input + ctxSize * (dci + 1));
    const uint32_t esit = packet * (burst + 1);
    epCtx[0] = interval << 16;
    epCtx[1] = (3 << 1) |      // CErr = 3
               ((inputEndpoint ? 7 : 3) << 3) | // Interrupt IN or OUT
               (burst << 8) |
               (packet << 16);
    uint64_t trDqp = ringPhys | 1;
    epCtx[2] = static_cast<uint32_t>(trDqp);
    epCtx[3] = static_cast<uint32_t>(trDqp >> 32);
    epCtx[4] = esit | (esit << 16); // average TRB length, max ESIT payload

    __asm__ volatile("mfence" ::: "memory");

    const uint32_t cc = XhciSlotCommand(ctrl, dev.inputCtxPhys, TRB_TYPE_CONFIG_EP | (dev.slotId << 24), 2000);
    if (cc != TRB_CC_SUCCESS) {
        SerialPrintf("xhci: Configure Endpoint (interrupt) failed (cc=%u)\n", cc);
        return false;
    }
    SerialPrintf("xhci: interrupt endpoint configured (DCI %u) ring=0x%lx\n", dci, ringPhys);
    return true;
}

// ECM notification endpoint (generic per-device interrupt ring).
static bool XhciConfigureInterruptEndpoint(XhciController& ctrl, XhciDevice& dev)
{
    if (!dev.interruptEpAddr) {
        SerialPuts("xhci: no interrupt endpoint found\n");
        return false;
    }
    return XhciConfigureInterrupt(ctrl, dev, dev.interruptEpAddr, dev.interruptMaxPacket,
                              dev.interruptInterval, dev.intRing, dev.intRingPhys,
                              dev.intEnqueue, dev.intCycle, dev.intDci);
}

// ---------------------------------------------------------------------------
// Phase 6: Set HID boot protocol
// ---------------------------------------------------------------------------

static bool XhciSetBootProtocol(XhciController& ctrl, XhciDevice& dev, uint8_t iface)
{
    UsbSetupPacket setup = {};
    setup.bmRequestType = 0x21; // Host-to-Device, Class, Interface
    setup.bRequest = USB_REQ_SET_PROTOCOL;
    setup.wValue = 0; // 0 = Boot Protocol
    setup.wIndex = iface;
    setup.wLength = 0;

    if (!XhciControlTransfer(ctrl, dev, setup, nullptr, 0, 0, 0)) {
        SerialPuts("xhci: SET_PROTOCOL (boot) failed\n");
        return false;
    }
    SerialPrintf("xhci: HID boot protocol set (interface %u)\n", iface);
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

// USB keyboard input device
static InputDevice g_usbKbdDev;
static InputDeviceOps g_usbKbdOps = { "usb_kbd", nullptr };

// Track previous report for key up/down detection

// Key presses seen (any USB keyboard) and the latest scancodes, reported
// from the hotplug worker so typing is visible on the console.
static volatile uint32_t g_keyPresses = 0;
static volatile uint8_t g_keyRecent[16];
static void XhciPushKey(uint8_t scancode, bool pressed, uint8_t modifiers)
{
    if (pressed) {
        const uint32_t n = __atomic_fetch_add(&g_keyPresses, 1, __ATOMIC_RELAXED);
        g_keyRecent[n % 16] = scancode;
    }
    InputEvent ev{};
    ev.type = pressed ? InputEventType::KeyPress : InputEventType::KeyRelease;
    ev.scanCode = scancode;
    ev.modifiers = modifiers;

    if (pressed) {
        ev.textCodepoint = KbdTranslateKey(scancode, modifiers);
        if (ev.textCodepoint < 128) ev.ascii = char(ev.textCodepoint);
        KbdPushCodepoint(ev.textCodepoint);
    }
    InputDevicePush(&g_usbKbdDev, ev);
}

static UsbBootKeyboard g_bootKeyboard;
static uint32_t g_activeUsbMice;
static volatile uint32_t g_mouseReports;

static UsbPointerIgnore XhciProcessMouseReport(const uint8_t* report, XhciHidFn& fn);
static void XhciEndHid(XhciHidFn& fn, bool keyboard);
static int XhciMatchTransferEvent(const XhciController& ctrl, const Trb& evt);
static bool XhciUacEventHit(const XhciController& ctrl, const Trb& event);
static void XhciUacEvent(const Trb& event);

// Drain the shared event ring for IRQs and both HID input queues.
static bool XhciHidRingHit(const XhciHidFn& fn, const Trb& event, uint32_t slot, uint32_t slotId)
{
    const uint32_t dci=(event.control>>16)&31;
    return fn.ringPhys && event.param>=fn.ringPhys && event.param<fn.ringPhys+XFER_RING_SIZE*sizeof(Trb) &&
           dci==fn.dci && slot==slotId;
}

static void XhciQueueHidTd(XhciController& ctrl, XhciDevice& dev, XhciHidFn& fn);

// Call with the controller event lock held. One TD per endpoint owns its
// buffer until completion or slot disable, including after a transport error.
static bool XhciFidoCompletion(XhciDevice& dev, const Trb& event)
{
    const uint32_t slot = event.control >> 24;
    const bool input = XhciHidRingHit(dev.fidoIn, event, slot, dev.slotId);
    const bool output = XhciHidRingHit(dev.fidoOut, event, slot, dev.slotId);
    if (!input && !output) return false;
    if (dev.gone || !dev.fidoIn.active) return true;
    const bool success = (event.status >> 24) == TRB_CC_SUCCESS && !(event.status & 0xffffff);
    if (output) {
        if (!dev.fidoTxPending) return true;
        dev.fidoTxPending = false;
        HidrawOutputComplete(dev.fidoIdentity, success);
        if (!success) dev.fidoIn.active = false;
    } else if (success) {
        HidrawReceive(dev.fidoIdentity, dev.fidoIn.report);
        XhciQueueHidTd(*dev.ctrl, dev, dev.fidoIn);
    } else {
        dev.fidoIn.active = false;
        HidrawTransportFailed(dev.fidoIdentity);
    }
    return true;
}

static void XhciMarkPointer(XhciHidFn& fn, UsbPointerStage stage, uint8_t cc = 0,
                            uint32_t residue = 0, UsbPointerIgnore reason = UsbPointerIgnore::None)
{
    const unsigned bit = 1u << static_cast<unsigned>(stage);
    if (fn.pointerStages & bit) return;
    fn.pointerStages |= bit;
    BootTimingUsbPointerTransfer(fn.pointerEndpoint, stage, cc, residue, reason);
}

static void XhciDrainController(XhciController& ctrl,bool& wakeNetwork)
{
    const auto saved=IrqSpinLockAcquire(&g_eventRingLock);
    unsigned processed=0;
    bool wakeInput=false;
    while(processed<EVT_RING_SIZE) {
        const auto& source=ctrl.evtRing[ctrl.evtDequeue];
        if(bool(source.control&TRB_CYCLE)!=ctrl.evtCycle)break;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        const Trb event=source;
        if(++ctrl.evtDequeue==EVT_RING_SIZE){ctrl.evtDequeue=0;ctrl.evtCycle=!ctrl.evtCycle;}
        ++processed;
        // A deep receive flight keeps the device producing completions as
        // we drain. Return event entries before it can fill the whole ring.
        if((processed&31u)==0)
            xhci_write64(ctrl.rtBase,XHCI_RT_ERDP,ctrl.evtRingPhys+ctrl.evtDequeue*sizeof(Trb));
        const uint32_t type=event.control&TRB_TYPE_MASK;
        if(type!=TRB_TYPE_TRANSFER_EVENT && type!=TRB_TYPE_CMD_COMPLETION)continue;
        bool handled=false;
        if(type==TRB_TYPE_TRANSFER_EVENT && XhciUacEventHit(ctrl,event)){XhciUacEvent(event);continue;}
        if(type==TRB_TYPE_TRANSFER_EVENT) {
            const int index=XhciMatchTransferEvent(ctrl,event);
            if(index>=0) {
                auto& dev=g_devices[index];
                const uint32_t slot=event.control>>24;
                const uint32_t cc=event.status>>24;
                if(dev.network && !dev.gone && XhciEcmCompletion(dev,event)){handled=true;wakeNetwork=true;}
                if (!handled) handled = XhciFidoCompletion(dev, event);
                for(int which=0;which<2 && !handled;++which) {
                    auto& fn=which?dev.mouse:dev.kbd;
                    if(!XhciHidRingHit(fn,event,slot,dev.slotId))continue;
                    handled=true;
                    const uint32_t residue = event.status & 0xffffff;
                    if (which) XhciMarkPointer(fn, UsbPointerStage::Complete, cc, residue);
                    if(!fn.active || dev.gone) {
                        if (which) XhciMarkPointer(fn, UsbPointerStage::Ignored, cc, residue,
                                                    UsbPointerIgnore::Inactive);
                        break;
                    }
                    if(cc!=TRB_CC_SUCCESS && cc!=TRB_CC_SHORT_PACKET) {
                        if (which) XhciMarkPointer(fn, UsbPointerStage::Ignored, cc, residue,
                                                    UsbPointerIgnore::TransferError);
                        fn.errorCc=static_cast<uint8_t>(cc);
                        XhciEndHid(fn,!which);
                        wakeInput=true;
                        break;
                    }
                    const uint32_t bytes=residue<=fn.length?fn.length-residue:0;
                    if(!which && bytes>=3) {
                        for(uint32_t i=bytes;i<8;++i)fn.report[i]=0;
                        g_bootKeyboard.Update(fn.report,fn.previous,XhciPushKey,KbdLayout());
                        wakeInput=true;
                    } else if(which && bytes>=3) {
                        if(bytes<4)fn.report[3]=0;
                        const uint8_t buttons = fn.report[0] & 7;
                        const bool motion = fn.report[1] || fn.report[2] || fn.report[3];
                        const bool changed = buttons != fn.previous[0];
                        if (motion || buttons || changed)
                            XhciMarkPointer(fn, UsbPointerStage::Report, cc, residue);
                        const auto reason = XhciProcessMouseReport(fn.report,fn);
                        if (reason != UsbPointerIgnore::None)
                            XhciMarkPointer(fn, UsbPointerStage::Ignored, cc, residue, reason);
                        __atomic_fetch_add(&g_mouseReports,1,__ATOMIC_RELAXED);
                        wakeInput=true;
                    } else if (which) XhciMarkPointer(fn, UsbPointerStage::Ignored, cc, residue,
                                                      UsbPointerIgnore::Length);
                    XhciQueueHidTd(ctrl,dev,fn);
                    if (which) XhciMarkPointer(fn, UsbPointerStage::Rearm);
                }
                // Completions for a disconnected device have no waiter.
                const auto discard=__atomic_load_n(&dev.mscDiscardDcis,__ATOMIC_ACQUIRE);
                if(!handled && (dev.gone || (discard&(1u<<((event.control>>16)&31)))))handled=true;
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
    if(wakeInput) {
        CompositorWake();
        InputWakeWaiters();
    }
}

static void XhciPollDrainEvents()
{
    bool wakeNetwork=false;
    const uint32_t count=__atomic_load_n(&g_controllerCount,__ATOMIC_ACQUIRE);
    for(uint32_t c=0;c<count;++c) {
        auto& ctrl=g_controllers[c];
        if(__atomic_load_n(&ctrl.initialized,__ATOMIC_ACQUIRE))XhciDrainController(ctrl,wakeNetwork);
    }
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

MODULE_IMPORT_SYMBOL(MouseMoveRelative);
MODULE_IMPORT_SYMBOL(MouseSetUsbButtons);
MODULE_IMPORT_SYMBOL(MouseSetUsbAvailable);

extern "C" void MouseMoveRelative(int32_t dx, int32_t dy);
extern "C" void MouseSetUsbButtons(uint8_t buttons);
extern "C" void MouseSetUsbAvailable(bool available);

static InputDevice g_usbMouseDev;
static InputDeviceOps g_usbMouseOps = { "usb_mouse", nullptr };

static uint8_t g_usbButtons;
static uint8_t g_usbButtonHolders[3];

static UsbPointerIgnore XhciProcessMouseReport(const uint8_t* report, XhciHidFn& fn)
{
    const uint8_t buttons = report[0] & 0x07;
    const uint8_t previous = g_usbButtons;
    const bool changed = buttons != fn.previous[0];
    for (unsigned bit = 0; bit < 3; ++bit) {
        const uint8_t mask = 1u << bit;
        if (!((buttons ^ fn.previous[0]) & mask)) continue;
        if (buttons & mask) ++g_usbButtonHolders[bit];
        else --g_usbButtonHolders[bit];
        if (g_usbButtonHolders[bit]) g_usbButtons |= mask;
        else g_usbButtons &= ~mask;
    }
    fn.previous[0] = buttons;
    MouseSetUsbButtons(g_usbButtons);

    const int8_t dx = static_cast<int8_t>(report[1]);
    const int8_t dy = static_cast<int8_t>(report[2]);
    const int8_t wheel = static_cast<int8_t>(report[3]);
    bool dropped = false;
    if (dx || dy) {
        MouseMoveRelative(dx, dy);
        InputEvent ev{};
        ev.type = InputEventType::MouseRelativeMove;
        ev.scanCode = g_usbButtons;
        ev.deltaX = dx;
        ev.deltaY = dy;
        dropped |= !InputDevicePush(&g_usbMouseDev, ev);
    }
    if (wheel) {
        InputEvent ev{};
        ev.type = InputEventType::MouseScroll;
        ev.scanCode = static_cast<uint8_t>(wheel);
        ev.modifiers = g_usbButtons;
        ev.deltaY = int32_t(wheel) * 120;
        dropped |= !InputDevicePush(&g_usbMouseDev, ev);
    }
    for (unsigned bit = 0; bit < 3; ++bit) {
        const uint8_t mask = 1u << bit;
        if (!((previous ^ g_usbButtons) & mask)) continue;
        InputEvent ev{};
        ev.type = (g_usbButtons & mask) ? InputEventType::MouseButtonDown : InputEventType::MouseButtonUp;
        ev.scanCode = bit;
        ev.modifiers = g_usbButtons;
        dropped |= !InputDevicePush(&g_usbMouseDev, ev);
    }
    if (dropped) return UsbPointerIgnore::RingFull;
    if (dx || dy || wheel || previous != g_usbButtons) return UsbPointerIgnore::None;
    return changed ? UsbPointerIgnore::SharedButton : UsbPointerIgnore::Idle;
}

static void XhciEndHid(XhciHidFn& fn, bool keyboard)
{
    if (!fn.active) return;
    fn.active = false;
    const uint8_t empty[8]{};
    if (keyboard) g_bootKeyboard.Update(empty,fn.previous,XhciPushKey,KbdLayout());
    else {
        XhciProcessMouseReport(empty,fn);
        MouseSetUsbAvailable(--g_activeUsbMice != 0);
    }
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
    return UsbEcmPrepareTrb(ring,XFER_RING_SIZE,ringPhysical,enqueue,cycle,
                            input,physical,bytes,chain,publish,[]{});
}
// Called under g_eventRingLock. A queued frame or an outstanding TD each
// reserves one software-queue slot, so simultaneous completions cannot
// overrun the consumer. Publish each TD only after recording its ownership:
// the controller may already be running from an earlier doorbell.
static bool XhciEcmArmSlot(UsbEcmState& net,unsigned slot)
{
    if(!net.rxCredits.CanArm(slot,net.rxDepth,net.tail-net.head))return false;
    const auto token=XhciEcmTrb(*net.device,true,net.rxPhysical+slot*ECM_RX_BYTES,ECM_RX_BYTES,false,false);
    net.rxCredits.Arm(slot,token);++net.rxPosted;
    auto& head=net.device->bulkInRing[(token-net.device->bulkInRingPhys)/sizeof(Trb)];
    __atomic_fetch_xor(&head.control,TRB_CYCLE,__ATOMIC_RELEASE);
    return true;
}
static void XhciEcmRingReceive(UsbEcmState& net,bool armed)
{
    if(armed){
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        XhciRingDoorbell(*net.controller,net.device->slotId,net.device->bulkInDci);
    }
    net.rxPressure.Observe(net.rxDepth,net.rxCredits.outstanding,net.tail-net.head);
}
static void XhciEcmReceiveArm(UsbEcmState& net)
{
    if(net.failed || net.controller->completionFailed)return;
    bool armed=false;
    for(unsigned i=0;i<net.rxDepth && net.rxCredits.outstanding<net.rxDepth &&
                     net.tail-net.head+net.rxCredits.outstanding<ECM_RX_PACKETS;++i)
        armed|=XhciEcmArmSlot(net,i);
    XhciEcmRingReceive(net,armed);
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
        if(event.param!=net.notificationToken || (cc!=TRB_CC_SUCCESS && cc!=TRB_CC_SHORT_PACKET) || residue>dev.ecm.notifyPacket){++net.rxErrors;fail();return true;}
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
        ++net.rxCompletions;
        const int rxSlot=net.rxCredits.CompletedSlot(event.param,cc,residue);
        if(rxSlot<0){++net.rxErrors;fail();return true;}
        net.rxCredits.Complete(static_cast<unsigned>(rxSlot));
        const unsigned bytes=ECM_RX_BYTES-residue;
        if(bytes>=14 && bytes<=1514){
            if(net.tail-net.head<ECM_RX_PACKETS){const auto index=net.tail%ECM_RX_PACKETS;memcpy(net.packets[index],net.rx+rxSlot*ECM_RX_BYTES,bytes);net.lengths[index]=bytes;++net.tail;++net.received;
                const auto queued=net.tail-net.head;if(queued>net.queueHighWater)net.queueHighWater=queued;}
            else ++net.dropped;
        }else if(bytes)++net.dropped;
        // Immediately rearm the returned DMA buffer when another queue
        // credit is available. The consumer refills after queue pressure.
        XhciEcmRingReceive(net,!net.controller->completionFailed &&
                          XhciEcmArmSlot(net,static_cast<unsigned>(rxSlot)));
        return true;
    }
    if(dci==dev.bulkOutDci){
        auto result=net.sends.Complete(event.param,cc,residue);
        if(result==UsbEcmTxSlots::Result::Finished)++net.transmitted;
        else if(result!=UsbEcmTxSlots::Result::Intermediate){++net.txErrors;fail();}
        return true;
    }
    return false;
}
static int XhciEcmTransmitImpl(NetIf* nif,const void* frame,uint32_t bytes,bool tryOnly){
    if(!frame || bytes<14 || bytes>1514)return -22;
    auto& net=*static_cast<UsbEcmState*>(nif->driverPriv);auto& dev=*net.device;
    uint64_t saved;
    if(tryOnly){if(!IrqSpinLockTryAcquire(&g_eventRingLock,&saved))return -11;}
    else saved=IrqSpinLockAcquire(&g_eventRingLock);
    if(net.failed || net.controller->completionFailed){IrqSpinLockRelease(&g_eventRingLock,saved);return -5;}
    const int freeSlot=net.sends.Free(net.txDepth);
    if(freeSlot>=0){const auto i=static_cast<unsigned>(freeSlot);
        auto* buffer=net.tx+i*2048;memcpy(buffer,frame,bytes);
        const auto physical=net.txPhysical+i*2048;
        const bool terminate=(bytes%dev.bulkOutMaxPacket)==0;
        // A zero-length TRB chained into the data TD is not a USB ZLP:
        // controllers may merge the TD into one exact-size USB transfer.
        // Termination needs its own zero-length TD and completion ownership.
        const auto first=XhciEcmTrb(dev,false,physical,bytes,false,!terminate);
        const auto final=terminate?XhciEcmTrb(dev,false,physical,0,false):first;
        net.sends.Reserve(i,first,final);++net.txSubmitted;
        if(terminate){
            auto& head=dev.bulkOutRing[(first-dev.bulkOutRingPhys)/sizeof(Trb)];
            __atomic_fetch_xor(&head.control,TRB_CYCLE,__ATOMIC_RELEASE);
        }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        XhciRingDoorbell(*net.controller,dev.slotId,dev.bulkOutDci);
        IrqSpinLockRelease(&g_eventRingLock,saved);return 0;
    }
    ++net.txBusy;IrqSpinLockRelease(&g_eventRingLock,saved);return -11;
}
static int XhciEcmTransmit(NetIf* nif,const void* frame,uint32_t bytes){
    return XhciEcmTransmitImpl(nif,frame,bytes,false);
}
static int XhciEcmTryTransmit(NetIf* nif,const void* frame,uint32_t bytes){
    return XhciEcmTransmitImpl(nif,frame,bytes,true);
}
static bool XhciEcmSnapshot(NetIf* nif,NetDriverSnapshot* out){
    if(!nif || !out || !nif->driverPriv)return false;
    auto& net=*static_cast<UsbEcmState*>(nif->driverPriv);
    const auto saved=IrqSpinLockAcquire(&g_eventRingLock);
    *out={net.received,net.transmitted,net.dropped,net.txBusy,
          net.rxPressure.entries,net.rxPressure.zeroEntries,net.rxDispatchBusy,net.rxDepth,
          net.rxCredits.outstanding,net.tail-net.head,net.queueHighWater,net.txDepth,
          net.failed || net.controller->completionFailed,
          net.rxPosted,net.rxCompletions,net.rxErrors,net.txSubmitted,net.txErrors,net.notifications,
          net.faultStatus,net.faultDci,net.linkKnown,net.linkUp};
    IrqSpinLockRelease(&g_eventRingLock,saved);return true;
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
    // Only one poller delivers the FIFO, since receive processing can
    // recurse into networking, transmit and polling. USB completion and
    // rearming always proceed independently under the controller lock.
    const bool early=NetEarlyBootPolling();
    {
        const auto saved=IrqSpinLockAcquire(&g_eventRingLock);
        if(net.head==net.tail || (early && net.earlyDispatch)){
            IrqSpinLockRelease(&g_eventRingLock,saved);
            return;
        }
        if(early)net.earlyDispatch=true;
        IrqSpinLockRelease(&g_eventRingLock,saved);
    }
    if(!early && !KMutexTryLock(&net.dispatchLock)){
        const auto saved=IrqSpinLockAcquire(&g_eventRingLock);
        if(net.head!=net.tail)++net.rxDispatchBusy;
        IrqSpinLockRelease(&g_eventRingLock,saved);
        return;
    }
    // Return one batch of credits before entering the network stack. Yield
    // dispatch after the batch so the next poll retires USB OUT completions
    // before this FIFO can fill all transmit slots with TCP acknowledgments.
    constexpr unsigned Batch=8;
    struct Frame {uint8_t data[1514];uint16_t length;};
    Frame frames[Batch];
    unsigned count=0;
    {
        const auto saved=IrqSpinLockAcquire(&g_eventRingLock);
        while(count<Batch && net.head!=net.tail){
            const auto index=net.head%ECM_RX_PACKETS;
            auto& frame=frames[count++];
            frame.length=net.lengths[index];
            memcpy(frame.data,net.packets[index],frame.length);
            ++net.head;
        }
        if(count)XhciEcmReceiveArm(net);
        IrqSpinLockRelease(&g_eventRingLock,saved);
    }
    for(unsigned i=0;i<count;++i)NetReceive(nif,frames[i].data,frames[i].length);
    if(early){
        const auto saved=IrqSpinLockAcquire(&g_eventRingLock);
        net.earlyDispatch=false;
        IrqSpinLockRelease(&g_eventRingLock,saved);
    }else{
        KMutexUnlock(&net.dispatchLock);
        // Recheck after releasing ownership. An IRQ arriving after this
        // observation already wakes net_poll itself.
        const auto saved=IrqSpinLockAcquire(&g_eventRingLock);
        const bool queued=net.head!=net.tail;
        IrqSpinLockRelease(&g_eventRingLock,saved);
        if(queued)NetWakePollThread();
    }
}
static bool XhciInitEcm(XhciController& ctrl,XhciDevice& dev){
    if(Vnode* marker=VfsOpen("/etc/usb-net-probe",0)){VfsClose(marker);g_networkProbe=true;}
    auto* net=static_cast<UsbEcmState*>(kmalloc(sizeof(UsbEcmState)));if(!net)return false;
    memset(net,0,sizeof(*net));net->controller=&ctrl;net->device=&dev;
    net->rxDepth=1;
    // Keep the on-disk opt-in used by the Dell image and older installations.
    if(Vnode* marker=VfsOpen("/etc/usb-net-rx8",0)){VfsClose(marker);net->rxDepth=ECM_RX_SLOTS;}
    net->txDepth=8;
    if(Vnode* marker=VfsOpen("/etc/usb-net-tx16",0)){VfsClose(marker);net->txDepth=ECM_TX_SLOTS;}
    net->rx=static_cast<uint8_t*>(AllocDmaBuffer((net->rxDepth*ECM_RX_BYTES+4095)/4096,net->rxPhysical,PmmOwner::NetworkDma));
    net->tx=static_cast<uint8_t*>(AllocDmaBuffer((net->txDepth+1)/2,net->txPhysical,PmmOwner::NetworkDma));
    net->notification=static_cast<uint8_t*>(AllocDmaBuffer(1,net->notificationPhysical,PmmOwner::NetworkDma));
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
    net->interface.driverPriv=net;net->interface.transmit=XhciEcmTransmit;net->interface.tryTransmit=XhciEcmTryTransmit;net->interface.poll=XhciEcmPoll;net->interface.snapshot=XhciEcmSnapshot;
    const auto saved=IrqSpinLockAcquire(&g_eventRingLock);
    net->rxCredits.ringBase=dev.bulkInRingPhys;
    dev.network=net;XhciEcmReceiveArm(*net);XhciEcmNotificationArm(*net);
    IrqSpinLockRelease(&g_eventRingLock,saved);
    NetRegisterIf(&net->interface);
    SerialPrintf("xhci: CDC-ECM network interface ready slot=%u rx_depth=%u tx_depth=%u (carrier unverified)\n",dev.slotId,net->rxDepth,net->txDepth);
    return true;
}

// Each 64 KiB DMA boundary consumes a TRB. An unaligned start and a
// wrap across the link TRB can each add one more.
static constexpr uint32_t USB_MSC_MAX_BULK_BYTES=1u<<20;
struct MscBulkTd {
    Trb* ring;
    uint64_t ringPhys,last;
    uint64_t tokens[USB_MSC_MAX_BULK_BYTES/65536+2];
    unsigned tokenCount=0;
    uint32_t dci,maxPacket;
};

static bool XhciPrepareBulkRing(XhciController& ctrl,bool isIn,Trb* ring,uint64_t ringPhys,
    uint32_t& enqueue,bool& cycle,uint32_t dci,uint32_t maxPacket,
    uint64_t bufPhys,uint32_t len,XhciTdPublication& publication,MscBulkTd& td)
{
    if(!ring || !maxPacket || !len || len>USB_MSC_MAX_BULK_BYTES)return false;
    uint64_t physical=bufPhys;
    uint32_t remaining=len;
    td.ring=ring;
    td.ringPhys=ringPhys;
    td.dci=dci;
    td.maxPacket=maxPacket;
    while(remaining) {
        if(enqueue>=XFER_RING_SIZE-1) {
            td.tokens[td.tokenCount++]=ringPhys+(XFER_RING_SIZE-1)*sizeof(Trb);
            publication.Stage(ring[XFER_RING_SIZE-1].control,
                XhciLinkControl(cycle,(ring[XFER_RING_SIZE-2].control&(1u<<4))!=0));
            cycle=!cycle;enqueue=0;
        }
        const auto bytes=XhciDmaChunk(physical,remaining);
        auto& trb=ring[enqueue];td.last=ringPhys+enqueue*sizeof(Trb);
        td.tokens[td.tokenCount++]=td.last;
        trb.param=physical;trb.status=bytes;
        if(ctrl.hciVersion>=0x100)trb.status|=XhciTdPackets(len,len-remaining+bytes,maxPacket)<<17;
        const uint32_t control=TRB_TYPE_NORMAL|(remaining>bytes?(1u<<4):TRB_IOC)|
            (isIn?TRB_ISP:0)|(cycle?TRB_CYCLE:0);
        publication.Stage(trb.control,control);
        ++enqueue;physical+=bytes;remaining-=bytes;
    }
    return true;
}

static bool XhciPrepareMscBulk(XhciController& ctrl,XhciDevice& dev,bool isIn,
    uint64_t bufPhys,uint32_t len,XhciTdPublication& publication,MscBulkTd& td)
{
    return XhciPrepareBulkRing(ctrl,isIn,isIn?dev.bulkInRing:dev.bulkOutRing,
        isIn?dev.bulkInRingPhys:dev.bulkOutRingPhys,isIn?dev.bulkInEnqueue:dev.bulkOutEnqueue,
        isIn?dev.bulkInCycle:dev.bulkOutCycle,isIn?dev.bulkInDci:dev.bulkOutDci,
        isIn?dev.bulkInMaxPacket:dev.bulkOutMaxPacket,bufPhys,len,publication,td);
}

static bool XhciPublishMscBulk(XhciController& ctrl,XhciDevice& dev,
    XhciTdPublication& publication,uint32_t dci)
{
    __asm__ volatile("mfence":::"memory");
    // Removal publishes gone under the same lock before disabling the slot.
    const auto saved=IrqSpinLockAcquire(&g_eventRingLock);
    const bool live=!__atomic_load_n(&dev.gone,__ATOMIC_ACQUIRE);
    if(live) {
        publication.Publish();
        __asm__ volatile("mfence":::"memory");
        XhciRingDoorbell(ctrl,dev.slotId,dci);
    }
    IrqSpinLockRelease(&g_eventRingLock,saved);
    return live;
}

static bool XhciWaitMscBulk(XhciController& ctrl,XhciDevice& dev,const MscBulkTd& td,
    bool isIn,uint64_t bufPhys,uint32_t len,uint32_t timeoutMs,uint32_t* shortInActual,
    UsbBotReceive* status)
{
    if(status)*status=UsbBotReceive::Failure;
    if(shortInActual)*shortInActual=0;
    const auto& [ring,ringPhys,token,tokens,tokenCount,dci,maxPacket]=td;
    Trb completion;
    Trb* evt=XhciWaitForEvent(ctrl,TRB_TYPE_TRANSFER_EVENT,timeoutMs,&completion,0,tokens,tokenCount);

    if (!evt) {
        KPrintf("xhci: bulk %s timeout slot=%u dci=%u bytes=%u mps=%u trbs=%u failed=%u dma_low=%x\n",
            isIn?"IN":"OUT",dev.slotId,dci,len,maxPacket,tokenCount,ctrl.completionFailed?1u:0u,unsigned(bufPhys&65535));
        auto* endpoint=reinterpret_cast<volatile uint32_t*>(static_cast<uint8_t*>(dev.outputCtx)+dci*(ctrl.ctx64?64:32));
        KPrintf("xhci: timeout USBSTS=%08x endpoint state=%u dequeue=%08x:%08x\n",
            xhci_read32(ctrl.opBase,XHCI_OP_USBSTS),endpoint[0]&7,endpoint[3],endpoint[2]);
        for(unsigned i=0;i<tokenCount;++i) {
            const auto* entry=ring+(tokens[i]-ringPhys)/sizeof(Trb);
            KPrintf("xhci: submitted token=%lx buffer=%lx status=%08x control=%08x\n",tokens[i],entry->param,entry->status,entry->control);
        }
        return false;
    }

    uint32_t cc = (evt->status >> TRB_CC_SHIFT) & 0xFF;
    const bool identity=evt->param==token&&((evt->control>>16)&31)==dci&&(evt->control>>24)==dev.slotId;
    if(status&&identity&&cc==6&&(evt->status&0xffffff)<=len) {
        *status=UsbBotReceive::Stall;return false;
    }
    if (!UsbScsiShortInCompletion(isIn,XhciDmaChunk(bufPhys,len)==len,
            shortInActual!=nullptr,len,cc,evt->status&0xffffff) || !identity) {
        KPrintf("xhci: bulk %s failed cc=%u residue=%u slot=%u dci=%u bytes=%u mps=%u event=%lx last=%lx\n",
            isIn?"IN":"OUT",cc,evt->status&0xffffff,dev.slotId,dci,len,maxPacket,evt->param,token);
        KPrintf("xhci: bulk DMA low=%x tokens=%u first=%lx\n",unsigned(bufPhys&65535),tokenCount,tokens[0]);
        return false;
    }

    if(status)*status=UsbBotReceive::Complete;
    if(shortInActual)*shortInActual=len-(evt->status&0xffffff);
    return true;
}

static bool XhciBulkTransfer(XhciController& ctrl,XhciDevice& dev,
    bool isIn,void*,uint64_t bufPhys,uint32_t len,uint32_t timeoutMs=5000,
    uint32_t* shortInActual=nullptr,UsbBotReceive* status=nullptr)
{
    if(status)*status=UsbBotReceive::Failure;
    if(shortInActual && (!isIn || len>192 || XhciDmaChunk(bufPhys,len)!=len))return false;
    XhciTdPublication publication;
    MscBulkTd td{};
    return XhciPrepareMscBulk(ctrl,dev,isIn,bufPhys,len,publication,td) &&
        XhciPublishMscBulk(ctrl,dev,publication,td.dci) &&
        XhciWaitMscBulk(ctrl,dev,td,isIn,bufPhys,len,timeoutMs,shortInActual,status);
}

// Restore only the halted bulk-IN endpoint after an observed CSW STALL.
// The MSC command lock guarantees no other TD is queued on these bulk rings.
struct UsbMscCswReceive {
    XhciController& controller;XhciDevice& device;void* buffer;uint64_t physical;
    UsbBotDequeue position{};
    const MscBulkTd* queued=nullptr;
    uint32_t timeoutMs=5000;
    UsbBotReceive receive() {
        UsbBotReceive status;
        if(queued) {
            XhciWaitMscBulk(controller,device,*queued,true,physical,13,timeoutMs,nullptr,&status);
            queued=nullptr;
        } else {
            XhciBulkTransfer(controller,device,true,buffer,physical,13,timeoutMs,nullptr,&status);
        }
        return status;
    }
    bool command(uint64_t param,uint32_t control) {
        const auto token=XhciSubmitCommand(controller,param,0,control|(device.slotId<<24)|(device.bulkInDci<<16));
        Trb event;auto* done=XhciWaitForEvent(controller,TRB_TYPE_CMD_COMPLETION,2000,&event,token);
        return done&&(done->status>>24)==TRB_CC_SUCCESS&&(done->control>>24)==device.slotId;
    }
    bool clearHalt() {
        UsbSetupPacket setup{};setup.bmRequestType=2;setup.bRequest=1; // standard endpoint CLEAR_FEATURE
        setup.wIndex=device.bulkInAddr; // ENDPOINT_HALT feature selector is zero
        return XhciControlTransfer(controller,device,setup,nullptr,0,0,0);
    }
    bool resetEndpoint(){return command(0,TRB_TYPE_RESET_EP);}
    bool setDequeue() {
        position=UsbBotNextDequeue(device.bulkInRingPhys,device.bulkInEnqueue,device.bulkInCycle,XFER_RING_SIZE);
        if(!position.valid)return false;
        if(position.skipLink) {
            // Hardware skips this Link explicitly. Leave it unowned for the new
            // lap, just as initial Links must be unowned before their first use.
            __atomic_store_n(&device.bulkInRing[XFER_RING_SIZE-1].control,
                XhciLinkControl(!position.cycle,false),__ATOMIC_RELEASE);
        }
        if(!command(position.pointer,16u<<TRB_TYPE_SHIFT))return false; // Set TR Dequeue Pointer
        device.bulkInEnqueue=position.index;device.bulkInCycle=position.cycle;
        KPrintf("xhci: MSC CSW stall recovered slot=%u dci=%u next=%u cycle=%u\n",
            device.slotId,device.bulkInDci,position.index,position.cycle?1u:0u);
        return true;
    }
};

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
static constexpr uint8_t SCSI_UNMAP = 0x42;

static uint32_t MscTimeout(uint8_t opcode) {
    return opcode==SCSI_UNMAP || opcode==SCSI_SYNCHRONIZE_CACHE_10 ? 30000 : 5000;
}

// Execute a SCSI command via BBB protocol
static KMutex g_mscCommandLock{};
static KMutex g_mscIoLock{};
static bool g_storageReadOnly=false;
static bool g_storagePageChunks=false;
static bool g_storageLegacyBurst=false;
static bool g_storagePipeline=false;
static uint32_t g_mscDeviceCount=0;

struct XhciMscResult {
    bool transport=false;uint8_t status=0;uint32_t actual=0,residue=0;UsbScsiSense sense{};
    bool success()const{return transport&&status==0;}
};
static uint64_t MscNowNs() {
    const auto precise=AudioClockNowNs();
    return precise?precise:ApicTickCount()*1000000;
}

struct MscCommandTiming {
    UsbStorageTiming& stats;
    uint32_t bytes;
    uint64_t start=MscNowNs(), cbw=0, data=0, csw=0;
    bool completed=false;
    unsigned commands=1;
    static void Record(UsbPhaseTiming& row,uint64_t ns) {
        row.lastNs=ns;row.totalNs+=ns;
        if(ns>row.maxNs)row.maxNs=ns;
    }
    uint64_t Lap() {
        const auto now=MscNowNs();
        const auto elapsed=now>=start?now-start:0;
        start=now;
        return elapsed;
    }
    ~MscCommandTiming() {
        stats.commands+=commands;
        if(!completed)stats.errors+=commands;
        else stats.dataBytes+=bytes;
        stats.lastBytes=bytes;
        Record(stats.command,cbw);Record(stats.data,data);Record(stats.status,csw);
    }
};

// Caller holds g_mscCommandLock through the original command and REQUEST SENSE.
static uint32_t XhciUasDci(uint8_t address) {
    return 2u*(address&15)+((address>>7)&1);
}

static void XhciFreeUas(XhciUas* uas) {
    if(!uas)return;
    for(unsigned pipe=0;pipe<UasPipeCount;++pipe) {
        FreeDmaBuffer(uas->contexts[pipe],1);
        for(const auto& ring:uas->rings[pipe])FreeDmaBuffer(ring.phys,1);
    }
    FreeDmaBuffer(uas->bufferPhys,1);
    kfree(uas);
}

static XhciUas* XhciAllocateUas(const UsbUasConfig& config) {
    auto* uas=static_cast<XhciUas*>(kmalloc(sizeof(XhciUas)));
    if(!uas)return nullptr;
    *uas={};
    uas->config=config;
    uas->buffers=static_cast<uint8_t*>(AllocDmaBuffer(1,uas->bufferPhys));
    if(!uas->buffers){XhciFreeUas(uas);return nullptr;}
    for(unsigned pipe=0;pipe<UasPipeCount;++pipe) {
        uint64_t* contexts=nullptr;
        if(pipe!=UasCommand) {
            contexts=static_cast<uint64_t*>(AllocDmaBuffer(1,uas->contexts[pipe]));
            if(!contexts){XhciFreeUas(uas);return nullptr;}
            memset(contexts,0,4096);
        }
        const unsigned count=contexts?UsbUasDepth:1;
        for(unsigned i=0;i<count;++i) {
            auto& ring=uas->rings[pipe][i];
            if(!XhciAllocTransferRing(ring.ring,ring.phys,ring.enqueue,ring.cycle)) {
                XhciFreeUas(uas);
                return nullptr;
            }
            if(contexts)contexts[2*(i+1)]=ring.phys|3u; // primary transfer ring, DCS 1
        }
    }
    return uas;
}

static bool XhciUasCanClaim(XhciController& ctrl,XhciDevice& dev,
    const UsbUasConfig& config,uint32_t replace=0)
{
    for(const auto& ep:config.pipes) {
        const uint32_t dci=XhciUasDci(ep.address);
        const auto* live=reinterpret_cast<const volatile uint32_t*>(
            static_cast<const uint8_t*>(dev.outputCtx)+CtxEntrySize(ctrl)*dci);
        if((live[0]&7u) && !(replace&(1u<<dci)))return false;
    }
    return true;
}

static uint32_t XhciDropStorageEndpoints(XhciController& ctrl,XhciDevice& dev,uint32_t mask) {
    const uint32_t size=CtxEntrySize(ctrl);
    auto* input=static_cast<uint8_t*>(dev.inputCtx);
    memset(input,0,size*33);
    auto* icc=reinterpret_cast<uint32_t*>(input);
    auto* slot=reinterpret_cast<uint32_t*>(input+size);
    memcpy(slot,dev.outputCtx,16);
    uint32_t entries=1;
    for(uint32_t dci=2;dci<32;++dci) {
        const auto* live=reinterpret_cast<const volatile uint32_t*>(
            static_cast<const uint8_t*>(dev.outputCtx)+size*dci);
        if(!(live[0]&7u))continue;
        if(mask&(1u<<dci))icc[0]|=1u<<dci;
        else entries=dci;
    }
    if(!icc[0])return TRB_CC_SUCCESS;
    icc[1]=1;
    slot[0]=(slot[0]&~(31u<<27))|(entries<<27);
    return XhciSlotCommand(ctrl,dev.inputCtxPhys,TRB_TYPE_CONFIG_EP|(dev.slotId<<24),2000);
}

static uint32_t XhciConfigureUas(XhciController& ctrl,XhciDevice& dev,XhciUas& uas) {
    const uint32_t size=CtxEntrySize(ctrl);
    auto* input=static_cast<uint8_t*>(dev.inputCtx);
    memset(input,0,size*33);
    auto* icc=reinterpret_cast<uint32_t*>(input);
    auto* slot=reinterpret_cast<uint32_t*>(input+size);
    memcpy(slot,dev.outputCtx,16);
    uint32_t entries=slot[0]>>27;
    icc[1]=1u;
    for(unsigned pipe=0;pipe<UasPipeCount;++pipe) {
        const auto& ep=uas.config.pipes[pipe];
        const uint32_t dci=XhciUasDci(ep.address);
        if(dci>entries)entries=dci;
        icc[1]|=1u<<dci;
        auto* ctx=reinterpret_cast<uint32_t*>(input+size*(dci+1));
        const uint32_t type=(ep.address&0x80)?6:2;
        const bool streams=pipe!=UasCommand;
        ctx[0]=streams?((2u<<10)|(1u<<15)):0; // eight linear primary stream contexts
        ctx[1]=(3u<<1)|(type<<3)|(uint32_t(ep.burst)<<8)|(uint32_t(ep.packet)<<16);
        const uint64_t dequeue=streams?uas.contexts[pipe]:(uas.rings[pipe][0].phys|1u);
        ctx[2]=dequeue;
        ctx[3]=dequeue>>32;
        ctx[4]=ep.packet;
    }
    slot[0]=(slot[0]&~(31u<<27))|(entries<<27);
    return XhciSlotCommand(ctrl,dev.inputCtxPhys,TRB_TYPE_CONFIG_EP|(dev.slotId<<24),2000);
}

static bool XhciSelectStorageInterface(XhciController& ctrl,XhciDevice& dev,
    uint8_t interface,uint8_t alternate)
{
    UsbSetupPacket setup{};
    setup.bmRequestType=1;
    setup.bRequest=0x0b;
    setup.wIndex=interface;
    setup.wValue=alternate;
    return XhciControlTransfer(ctrl,dev,setup,nullptr,0,0,0);
}

struct XhciUasRequest {
    const uint8_t* cdb;
    uint64_t physical;
    uint32_t size;
    uint8_t cdbSize;
};

static bool XhciUasSubmit(XhciController& ctrl,XhciDevice& dev,unsigned pipe,unsigned index,
    uint64_t physical,uint32_t size,MscBulkTd& td)
{
    auto& uas=*dev.uas;
    const auto& ep=uas.config.pipes[pipe];
    const uint32_t stream=pipe==UasCommand?0:index+1;
    auto& ring=uas.rings[pipe][stream?index:0];
    XhciTdPublication publication;
    return XhciPrepareBulkRing(ctrl,ep.address&0x80,ring.ring,ring.phys,ring.enqueue,ring.cycle,
        XhciUasDci(ep.address),ep.packet,physical,size,publication,td) &&
        XhciPublishMscBulk(ctrl,dev,publication,td.dci|(stream<<16));
}

static void XhciDiscardStorageEvents(XhciController& ctrl,XhciDevice& dev,uint32_t dcis) {
    XhciPollDrainEvents();
    const auto saved=IrqSpinLockAcquire(&g_eventRingLock);
    ctrl.completions.Discard([&](const Trb& event) {
        return (event.control&TRB_TYPE_MASK)==TRB_TYPE_TRANSFER_EVENT &&
            (event.control>>24)==dev.slotId && (dcis&(1u<<((event.control>>16)&31)));
    });
    IrqSpinLockRelease(&g_eventRingLock,saved);
}

static bool XhciUasRetireData(XhciController& ctrl,XhciDevice& dev,unsigned pipe,unsigned tags) {
    auto& uas=*dev.uas;
    const uint32_t dci=XhciUasDci(uas.config.pipes[pipe].address);
    const uint32_t target=(dev.slotId<<24)|(dci<<16);
    const auto* ep=reinterpret_cast<const volatile uint32_t*>(
        static_cast<const uint8_t*>(dev.outputCtx)+CtxEntrySize(ctrl)*dci);
    auto cc=XhciSlotCommand(ctrl,0,TRB_TYPE_STOP_EP|target);
    if(cc==TRB_CC_CONTEXT_STATE && (ep[0]&7u)==2) {
        // Preserve the other streams' transfer state on a halted endpoint.
        cc=XhciSlotCommand(ctrl,0,TRB_TYPE_RESET_EP|(1u<<9)|target);
    }
    if(cc!=TRB_CC_SUCCESS && !(cc==TRB_CC_CONTEXT_STATE && (ep[0]&7u)==3))return false;
    for(unsigned i=0;i<UsbUasDepth;++i)if(tags&(1u<<i)) {
        auto& ring=uas.rings[pipe][i];
        const auto position=UsbBotNextDequeue(ring.phys,ring.enqueue,ring.cycle,XFER_RING_SIZE);
        if(!position.valid)return false;
        if(position.skipLink)
            __atomic_store_n(&ring.ring[XFER_RING_SIZE-1].control,
                XhciLinkControl(!position.cycle,false),__ATOMIC_RELEASE);
        const uint64_t dequeue=position.pointer|2u; // primary transfer ring, DCS preserved
        const auto token=XhciSubmitCommand(ctrl,dequeue,(i+1)<<16,TRB_TYPE_SET_TR_DEQ|target);
        Trb completion;
        if(!XhciWaitForEvent(ctrl,TRB_TYPE_CMD_COMPLETION,2000,&completion,token) ||
           (completion.status>>24)!=TRB_CC_SUCCESS || (completion.control>>24)!=dev.slotId)return false;
        ring.enqueue=position.index;
        ring.cycle=position.cycle;
    }
    XhciDiscardStorageEvents(ctrl,dev,1u<<dci);
    return true;
}

// The caller holds the command lock for the whole flight. Every referenced
// DMA page is transport-owned and survives even if a command is unconfirmed.
static XhciMscResult XhciUasExecute(XhciController& ctrl,XhciDevice& dev,
    const XhciUasRequest* requests,unsigned count,bool in,bool allowShort=false)
{
    if(__atomic_load_n(&dev.mscQuarantined,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&dev.gone,__ATOMIC_ACQUIRE))return {};
    constexpr unsigned slotBytes=256,statusOffset=64,statusBytes=112;
    auto& uas=*dev.uas;
    struct Flight {
        MscBulkTd command{},data{},status{};
        uint32_t actual=0;
    } flight[UsbUasDepth]{};
    auto run=[&]() -> XhciMscResult {
        if(!count || count>UsbUasDepth)return {};
        uint32_t total=0;
        for(unsigned i=0;i<count;++i) {
            const auto& req=requests[i];
            uint8_t* buffer=uas.buffers+i*slotBytes;
            const uint64_t physical=uas.bufferPhys+i*slotBytes;
            if(!req.cdbSize || req.cdbSize>16)return {};
            memset(buffer,0,slotBytes);
            buffer[0]=1; // Command IU, SIMPLE task attribute, LUN 0.
            buffer[3]=i+1;
            memcpy(buffer+16,req.cdb,req.cdbSize);
            if(!XhciUasSubmit(ctrl,dev,UasStatus,i,physical+statusOffset,statusBytes,flight[i].status) ||
               (req.size && !XhciUasSubmit(ctrl,dev,in?UasDataIn:UasDataOut,i,
                   req.physical,req.size,flight[i].data)))return {};
            total+=req.size;
        }
        MscCommandTiming timing{dev.mscTiming,total};
        timing.commands=count;
        ++dev.mscTiming.uasFlights;
        if(count>dev.mscTiming.uasPeakDepth)dev.mscTiming.uasPeakDepth=count;
        for(unsigned i=0;i<count;++i)
            if(!XhciUasSubmit(ctrl,dev,UasCommand,i,uas.bufferPhys+i*slotBytes,32,flight[i].command))return {};
        for(unsigned i=0;i<count;++i)
            if(!XhciWaitMscBulk(ctrl,dev,flight[i].command,false,uas.bufferPhys+i*slotBytes,
                32,5000,nullptr,nullptr))return {};
        timing.cbw=timing.Lap();
        UsbUasFlightState pending{uint8_t((1u<<count)-1),0};
        uint32_t budget=5000;
        for(unsigned i=0;i<count;++i) {
            if(requests[i].size)pending.awaitingData|=1u<<i;
            const auto timeout=MscTimeout(requests[i].cdb[0]);
            if(timeout>budget)budget=timeout;
        }
        const auto start=ApicTickCount();
        uint8_t status[UsbUasDepth]{};
        UsbScsiSense sense[UsbUasDepth]{};
        bool badData[UsbUasDepth]{};
        const unsigned dataPipe=in?UasDataIn:UasDataOut;
        bool queueFailed=false;
        auto take=[&](const MscBulkTd& td,Trb& out) {
            const auto saved=IrqSpinLockAcquire(&g_eventRingLock);
            queueFailed=ctrl.completionFailed;
            const bool found=!queueFailed && ctrl.completions.TakeAny(TRB_TYPE_TRANSFER_EVENT,td.tokens,td.tokenCount,out);
            IrqSpinLockRelease(&g_eventRingLock,saved);
            return found;
        };
        auto identity=[&](const Trb& event,const MscBulkTd& td) {
            return ((event.control>>16)&31)==td.dci && (event.control>>24)==dev.slotId;
        };
        auto poll=[&]() -> bool {
            XhciPollDrainEvents();
            if(__atomic_load_n(&dev.gone,__ATOMIC_ACQUIRE))return false;
            for(unsigned i=0;i<count;++i) {
                Trb event;
                const unsigned bit=1u<<i;
                if((pending.awaitingStatus&bit) && take(flight[i].status,event)) {
                    const uint32_t residue=event.status&0xffffff;
                    const auto* buffer=uas.buffers+i*slotBytes+statusOffset;
                    uint32_t senseBytes=0;
                    if(!identity(event,flight[i].status) || event.param!=flight[i].status.last ||
                       !UsbScsiShortInCompletion(true,true,true,statusBytes,event.status>>24,residue) ||
                       !UsbUasStatus(buffer,statusBytes-residue,i+1,status[i],senseBytes))return false;
                    sense[i]=UsbScsiParseSense(buffer+16,senseBytes);
                    pending.Status(i,status[i]);
                }
                if((pending.awaitingData&bit) && !badData[i] && take(flight[i].data,event)) {
                    const auto& req=requests[i];
                    const uint32_t cc=event.status>>24,residue=event.status&0xffffff;
                    if(!identity(event,flight[i].data))return false;
                    if(event.param==flight[i].data.last &&
                       UsbScsiShortInCompletion(in,XhciDmaChunk(req.physical,req.size)==req.size,
                           allowShort || (pending.rejected&bit),req.size,cc,residue)) {
                        flight[i].actual=req.size-residue;
                        pending.Data(i);
                    } else {
                        // A short or halted data TD can precede the final Sense IU.
                        // With GOOD status this remains a transfer error.
                        if(cc!=TRB_CC_SHORT_PACKET && cc!=TRB_CC_STALL)return false;
                        badData[i]=true;
                    }
                }
                if(badData[i] && !(pending.awaitingStatus&bit) && !status[i])return false;
            }
            __asm__ volatile("pause");
            return !queueFailed;
        };
        if(!UsbUasWaitFlight(pending,poll,[&] {return ApicTickCount()-start>=budget;},
            [&](unsigned tags) {return XhciUasRetireData(ctrl,dev,dataPipe,tags);}))return {};
        timing.data=timing.Lap();
        XhciMscResult result{true,0,0,0,{}};
        for(unsigned i=0;i<count;++i) {
            // Prefer permanent errors over a retryable result elsewhere in the flight.
            if(status[i] && (!result.status ||
               UsbUasRetryRead(result.status,result.sense.current,result.sense.key))) {
                result.status=status[i];
                result.sense=sense[i];
            }
            result.actual+=flight[i].actual;
            result.residue+=requests[i].size-flight[i].actual;
        }
        timing.csw=timing.Lap();
        timing.completed=result.success();
        timing.bytes=result.actual;
        return result;
    };
    auto result=run();
    if(!result.transport) {
        uint32_t dcis=0;
        for(const auto& pipe:uas.config.pipes)dcis|=1u<<XhciUasDci(pipe.address);
        __atomic_store_n(&dev.mscDiscardDcis,dcis,__ATOMIC_RELEASE);
        XhciDiscardStorageEvents(ctrl,dev,dcis);
        __atomic_store_n(&dev.mscQuarantined,true,__ATOMIC_RELEASE);
        KPrintf("xhci: UAS incomplete flight depth=%u; transport quarantined\n",count);
    }
    return result;
}

static XhciMscResult XhciMscCommandLocked(XhciController& ctrl,XhciDevice& dev,
    const uint8_t* cdb,uint8_t cdbLen,void* data,uint64_t dataPhys,uint32_t dataLen,
    bool dataIn,bool allowShort=false)
{
    if(__atomic_load_n(&dev.gone,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&dev.mscQuarantined,__ATOMIC_ACQUIRE))return {};
    if (!dev.mscCommandBuffer)
        dev.mscCommandBuffer = AllocDmaBuffer(1, dev.mscCommandBufferPhys);
    if (!dev.mscCommandBuffer) return {};
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

    const uint32_t timeout=MscTimeout(cdb[0]);
    MscCommandTiming timing{dev.mscTiming,dataLen};
    // 1. Send CBW via Bulk OUT
    const bool sent=XhciBulkTransfer(ctrl, dev, false, cbw, cbwPhys, sizeof(UsbMscCbw));
    timing.cbw=timing.Lap();
    if (!sent) {
        __atomic_store_n(&dev.mscQuarantined,true,__ATOMIC_RELEASE);
        KPrintf("xhci: MSC CBW failed opcode=%x bytes=%u; transport quarantined\n",unsigned(cdb[0]),dataLen);
        return {};
    }

    uint64_t cswPhys=dev.mscCommandBufferPhys+64;
    auto* csw=reinterpret_cast<UsbMscCsw*>(static_cast<uint8_t*>(dev.mscCommandBuffer)+64);
    memset(csw,0,sizeof(*csw));

    // Both TDs use the same IN ring, so the status cannot overtake the
    // data. Keep short/variable transfers and OUT data on separate phases.
    const bool queueStatus=g_storagePipeline && dataIn && dataLen && data && !allowShort &&
        cdb[0]==SCSI_READ_10;
    MscBulkTd statusTd{};
    // 2. Data phase (if any)
    uint32_t actual=dataLen;
    if (dataLen > 0 && data) {
        bool transferred;
        if(queueStatus) {
            XhciTdPublication publication;
            MscBulkTd dataTd{};
            transferred=XhciPrepareMscBulk(ctrl,dev,true,dataPhys,dataLen,publication,dataTd) &&
                XhciPrepareMscBulk(ctrl,dev,true,cswPhys,sizeof(*csw),publication,statusTd) &&
                XhciPublishMscBulk(ctrl,dev,publication,dataTd.dci) &&
                XhciWaitMscBulk(ctrl,dev,dataTd,true,dataPhys,dataLen,timeout,nullptr,nullptr);
        } else {
            transferred=XhciBulkTransfer(ctrl,dev,dataIn,data,dataPhys,dataLen,timeout,
                allowShort?&actual:nullptr);
        }
        timing.data=timing.Lap();
        if (!transferred) {
            __atomic_store_n(&dev.mscQuarantined,true,__ATOMIC_RELEASE);
            const uint32_t lba=cdbLen>=10?(uint32_t(cdb[2])<<24)|(uint32_t(cdb[3])<<16)|(uint32_t(cdb[4])<<8)|cdb[5]:0;
            KPrintf("xhci: MSC data %s failed opcode=%x bytes=%u tag=%u lba=%u; transport quarantined\n",dataIn?"IN":"OUT",unsigned(cdb[0]),dataLen,cbw->dCBWTag,lba);
            return {};
        }
    }

    // 3. Receive CSW via Bulk IN
    UsbMscCswReceive receive{ctrl,dev,csw,cswPhys,{},queueStatus?&statusTd:nullptr,timeout};
    const bool received=UsbBotReceiveCsw(receive);
    timing.csw=timing.Lap();
    if (!received) {
        __atomic_store_n(&dev.mscQuarantined,true,__ATOMIC_RELEASE);
        KPrintf("xhci: MSC CSW failed opcode=%x bytes=%u; transport quarantined\n",unsigned(cdb[0]),dataLen);
        return {};
    }

    if (csw->dCSWSignature != CSW_SIGNATURE) {
        __atomic_store_n(&dev.mscQuarantined,true,__ATOMIC_RELEASE);
        KPrintf("xhci: MSC bad CSW signature 0x%08x\n", csw->dCSWSignature);
        return {};
    }

    if(!UsbBotStatusValid(cbw->dCBWTag,csw->dCSWTag,csw->bCSWStatus,
                          allowShort,dataLen,actual,csw->dCSWDataResidue)) {
        __atomic_store_n(&dev.mscQuarantined,true,__ATOMIC_RELEASE);
        KPrintf("xhci: MSC invalid CSW opcode=%x status=%u residue=%u actual=%u requested=%u; transport quarantined\n",
            unsigned(cdb[0]),csw->bCSWStatus,csw->dCSWDataResidue,actual,dataLen);
        return {};
    }
    timing.completed=csw->bCSWStatus==0;
    timing.bytes=dataLen-csw->dCSWDataResidue;
    return {true,csw->bCSWStatus,timing.bytes,csw->dCSWDataResidue,{}};
}

static XhciMscResult XhciMscExecuteLocked(XhciController& ctrl,XhciDevice& dev,
    const uint8_t* cdb,uint8_t cdbLen,void* data,uint64_t dataPhys,uint32_t dataLen,
    bool dataIn,bool allowShort=false)
{
    if(dev.uas) {
        const XhciUasRequest request{cdb,dataPhys,dataLen,cdbLen};
        return XhciUasExecute(ctrl,dev,&request,1,dataIn,allowShort);
    }
    auto result=XhciMscCommandLocked(ctrl,dev,cdb,cdbLen,data,dataPhys,dataLen,dataIn,allowShort);
    if(!result.transport||result.status==0)return result;
    const uint8_t opcode=cdb[0];
    KPrintf("xhci: MSC command failed opcode=%x status=%u residue=%u actual=%u\n",
        unsigned(opcode),result.status,result.residue,result.actual);
    // Sense uses separate bytes in the persistent command page. Do not recurse
    // through this wrapper, reuse a live data buffer, or take the mutex again.
    if(opcode!=3) {
        auto* sense=static_cast<uint8_t*>(dev.mscCommandBuffer)+128;
        memset(sense,0,18);uint8_t request[6]{};request[0]=3;request[4]=18;
        const auto response=XhciMscCommandLocked(ctrl,dev,request,6,sense,
            dev.mscCommandBufferPhys+128,18,true,true);
        if(response.success())result.sense=UsbScsiParseSense(sense,response.actual);
        KPrintf("xhci: MSC sense original=%x valid=%u current=%u key=%x asc=%x ascq=%x bytes=%u\n",
            unsigned(opcode),result.sense.valid?1u:0u,result.sense.current?1u:0u,
            unsigned(result.sense.key),unsigned(result.sense.asc),unsigned(result.sense.ascq),response.actual);
    }
    return result;
}
struct MscCommandGuard {
    MscCommandGuard(){KMutexLock(&g_mscCommandLock);}
    ~MscCommandGuard(){KMutexUnlock(&g_mscCommandLock);}
};
static bool XhciMscCommand(XhciController& ctrl,XhciDevice& dev,
    const uint8_t* cdb,uint8_t cdbLen,void* data,uint64_t dataPhys,uint32_t dataLen,bool dataIn)
{
    MscCommandGuard guard;
    return XhciMscExecuteLocked(ctrl,dev,cdb,cdbLen,data,dataPhys,dataLen,dataIn).success();
}
static UsbScsiCache XhciMscCacheLocked(XhciController& ctrl,XhciDevice& dev) {
    auto* data=static_cast<uint8_t*>(dev.mscCommandBuffer)+128;memset(data,0,192);
    uint8_t request[6]{};request[0]=0x1a;request[2]=0x3f;request[4]=192; // current all pages, DBD=0 for USB compatibility
    auto result=XhciMscExecuteLocked(ctrl,dev,request,6,data,dev.mscCommandBufferPhys+128,192,true,true);
    auto cache=result.success()?UsbScsiModeCache6(data,result.actual,true):UsbScsiCache::Unknown;
    KPrintf("xhci: MSC current cache mode=%u bytes=%u complete=%u (0=unknown 1=reported-through 2=write-back 3=assumed-through)\n",
        unsigned(cache),result.actual,result.success()?1u:0u);
    return cache;
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
    uint32_t bounceCapacity;
    uint32_t botMaxBytes;
    UsbUnmapLimits unmapLimits;
    char name[16];
    UsbWriteWindows windows;
    bool systemFailed;
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
    uint32_t bounceBytes() {
        const uint32_t preferred=g_storagePageChunks?4096:state->bounceCapacity;
        return !g_devices[state->devIndex].uas && state->botMaxBytes && state->botMaxBytes<preferred
            ?state->botMaxBytes:preferred;
    }
    uint8_t* bounce() {return state->bounce;}
    bool usable() {
        const auto& dev=g_devices[state->devIndex];
        return !__atomic_load_n(&dev.gone,__ATOMIC_ACQUIRE) &&
            !__atomic_load_n(&dev.mscQuarantined,__ATOMIC_ACQUIRE);
    }
    bool transfer(uint64_t lba,uint32_t count,bool read,uint64_t physical=0) {
        if(!usable())return false;
        uint8_t cdb[10]{};cdb[0]=read?SCSI_READ_10:SCSI_WRITE_10;
        cdb[2]=lba>>24;cdb[3]=lba>>16;cdb[4]=lba>>8;cdb[5]=lba;
        cdb[7]=count>>8;cdb[8]=count;
        const uint64_t bytes=uint64_t(count)*blockSize();
        auto& dev=g_devices[state->devIndex];
        const bool direct=physical!=0;
        if(!physical)physical=state->bouncePhys;
        auto* buffer=direct?reinterpret_cast<uint8_t*>(PhysToVirt(PhysicalAddress(physical)).raw()):state->bounce;
        MscCommandGuard guard;
        if(dev.uas && count>=128 && bytes>=256*1024) {
            uint8_t commands[UsbUasDepth][10]{};
            XhciUasRequest requests[UsbUasDepth]{};
            uint32_t queued=0;
            for(unsigned i=0;i<UsbUasDepth;++i) {
                const uint64_t at=lba+queued;
                const uint32_t blocks=(count-queued)/(UsbUasDepth-i);
                if(!read && g_storageReadOnly &&
                   !state->windows.Allows(at*blockSize(),uint64_t(blocks)*blockSize()))return false;
                auto* command=commands[i];
                command[0]=cdb[0];
                command[2]=at>>24;command[3]=at>>16;command[4]=at>>8;command[5]=at;
                command[7]=blocks>>8;command[8]=blocks;
                requests[i]={command,physical+uint64_t(queued)*blockSize(),blocks*blockSize(),10};
                queued+=blocks;
            }
            auto result=XhciUasExecute(*state->ctrl,dev,requests,UsbUasDepth,read);
            if(read && result.transport && UsbUasRetryRead(result.status,result.sense.current,result.sense.key))
                result=XhciUasExecute(*state->ctrl,dev,requests,UsbUasDepth,read);
            const bool ok=result.success();
            if(ok) {
                StorageIoAccount(uint32_t(StorageIoSource::Usb),read?0:1,bytes);
                if(direct)dev.mscTiming.directBytes+=bytes;
            }
            return ok;
        }
        auto execute=[&] {
            return XhciMscExecuteLocked(*state->ctrl,dev,cdb,10,
                buffer,physical,uint32_t(bytes),read);
        };
        auto result=execute();
        // Retry only reads with a fully retired exchange.
        if(read && usable() && result.transport &&
           (dev.uas?UsbUasRetryRead(result.status,result.sense.current,result.sense.key):
            (state->windows.system.length && result.status==1 && result.sense.current && result.sense.key==0xb)))
            result=execute();
        if(result.success()) {
            StorageIoAccount(uint32_t(StorageIoSource::Usb),read?0:1,bytes);
            if(direct)dev.mscTiming.directBytes+=bytes;
        }
        return result.success();
    }
    bool readBlocks(uint64_t lba,uint32_t count) {return transfer(lba,count,true);}
    bool writeBlocks(uint64_t lba,uint32_t count) {return transfer(lba,count,false);}
};
struct MscIoGuard {
    MscIoGuard(){KMutexLock(&g_mscIoLock);}
    ~MscIoGuard(){KMutexUnlock(&g_mscIoLock);}
};
static void UsbMscFenceSystem(UsbMscState& state) {
    if(state.windows.system.length) {
        state.windows.system={};
        __atomic_store_n(&state.systemFailed,true,__ATOMIC_RELEASE);
    }
}
static int UsbMscRead(Device* dev,uint64_t offset,void* buf,uint64_t len) {
    MscIoGuard guard;
    auto* state=static_cast<UsbMscState*>(dev->priv);
    UsbMscByteTransport io{state};
    const int result=UsbBlockReadBytes(io,offset,buf,len);
    if(result==-5 || (result>=0 && uint64_t(result)!=len &&
        UsbWriteWindows::Contains(state->windows.system,offset,len)))UsbMscFenceSystem(*state);
    return result;
}
static int UsbMscWrite(Device* dev,uint64_t offset,const void* buf,uint64_t len) {
    MscIoGuard guard;
    auto* state=static_cast<UsbMscState*>(dev->priv);
    if(__atomic_load_n(&state->systemFailed,__ATOMIC_ACQUIRE) ||
       (g_storageReadOnly && !state->windows.Allows(offset,len)))return -30;
    UsbMscByteTransport io{state};
    const int result=UsbBlockWriteBytes(io,offset,buf,len);
    if(result==-5 || (result>=0 && uint64_t(result)!=len))UsbMscFenceSystem(*state);
    return result;
}

static int UsbMscReadDma(UsbMscState& state,XhciDevice& device,UsbDmaRead& request) {
    MscIoGuard guard;
    UsbMscByteTransport io{&state};
    const uint32_t block=io.blockSize();
    const uint64_t limit=g_dma32?(4ull<<30):(16ull<<30);
    if(!state.windows.CanReadDma(request,block,io.bounceBytes(),limit))return -95;
    if(!io.usable() || __atomic_load_n(&state.systemFailed,__ATOMIC_ACQUIRE))return -95;
    if(io.transfer(request.offset/block,request.bytes/block,true,request.physical))return request.bytes;
    // A transport failure may leave the xHC holding a pointer into the
    // caller's allocation. Neither the allocator nor the caller may reuse it.
    UsbMscFenceSystem(state);
    return UsbDmaTransferFailure(request,__atomic_load_n(&device.mscQuarantined,__ATOMIC_ACQUIRE));
}

static int UsbMscUnmapLimits(UsbMscState& state,XhciDevice& dev) {
    auto* data=state.bounce;
    UsbMscByteTransport io{&state};
    auto page=[&](uint8_t code,uint8_t bytes,uint8_t needed) {
        uint8_t cdb[6]={SCSI_INQUIRY,1,code,0,bytes,0};
        memset(data,0,bytes);
        const auto result=XhciMscExecuteLocked(*state.ctrl,dev,cdb,sizeof(cdb),
            data,state.bouncePhys,bytes,true,true);
        return result.success() && ScsiUnmapVpd(data,result.actual,code,needed);
    };
    // Read the exact B2 minimum and allow standard short responses.
    if(!page(0xb2,8,6) || !(data[5]&0x80) || !page(0xb0,64,28))
        return io.usable()?-95:-5;
    const uint32_t sectors=ScsiUnmapBe32(data+20);
    state.unmapLimits={sectors>UsbUnmapMaxSectors?UsbUnmapMaxSectors:sectors,ScsiUnmapBe32(data+24)};
    return state.unmapLimits.maxSectors && state.unmapLimits.maxRanges?0:-95;
}

static int UsbMscUnmap(UsbMscState& state,XhciDevice& dev,uint32_t request,void* arg) {
    if(!arg)return -22;
    MscIoGuard ioGuard;
    UsbMscByteTransport io{&state};
    if(!state.windows.system.length || !g_storageReadOnly || io.blockSize()!=512)return -30;
    if(!io.usable() || __atomic_load_n(&state.systemFailed,__ATOMIC_ACQUIRE))return -5;
    MscCommandGuard commandGuard;
    if(request==DEVICE_IOCTL_USB_UNMAP_LIMITS) {
        const int result=UsbMscUnmapLimits(state,dev);
        if(!result)*static_cast<UsbUnmapLimits*>(arg)=state.unmapLimits;
        else if(result==-5)UsbMscFenceSystem(state);
        return result;
    }
    const auto& batch=*static_cast<const UsbUnmapBatch*>(arg);
    const int admission=state.windows.CheckUnmap(batch);
    if(admission)return admission;
    const uint32_t bytes=ScsiEncodeUnmap(state.bounce,state.unmapLimits,batch.ranges,batch.count);
    if(!bytes)return -22;
    const uint8_t cdb[10]={0x42,0,0,0,0,0,0,uint8_t(bytes>>8),uint8_t(bytes),0};
    const auto result=XhciMscExecuteLocked(*state.ctrl,dev,cdb,sizeof(cdb),
        state.bounce,state.bouncePhys,bytes,false);
    if(result.success())return 0;
    UsbMscFenceSystem(state);
    return -5;
}

static int XhciPromoteSystemStorage(UsbMscState& state);

static int UsbMscIoctl(Device* dev, uint32_t cmd, void* arg)
{
    if (cmd == DEVICE_IOCTL_READ_ONLY) return g_storageReadOnly ? 1 : 0;
    auto* st=static_cast<UsbMscState*>(dev->priv);
    if (cmd == DEVICE_IOCTL_USB_STORAGE_TIMING) {
        if(!arg)return -22;
        MscCommandGuard commandGuard;
        *static_cast<UsbStorageTiming*>(arg)=g_devices[st->devIndex].mscTiming;
        return 0;
    }
    if(cmd==DEVICE_IOCTL_USB_SYSTEM_TRANSPORT)return XhciPromoteSystemStorage(*st);
    if(cmd==DEVICE_IOCTL_USB_READ_DMA)
        return arg?UsbMscReadDma(*st,g_devices[st->devIndex],*static_cast<UsbDmaRead*>(arg)):-22;
    if(cmd==DEVICE_IOCTL_USB_UNMAP_LIMITS || cmd==DEVICE_IOCTL_USB_UNMAP)
        return UsbMscUnmap(*st,g_devices[st->devIndex],cmd,arg);
    if (cmd == DEVICE_IOCTL_PREFERRED_READ_BYTES) return UsbMscByteTransport{st}.bounceBytes();
    if (cmd == DEVICE_IOCTL_WRITE_FENCED) {
        const auto& device=g_devices[st->devIndex];
        return __atomic_load_n(&st->systemFailed,__ATOMIC_ACQUIRE) ||
            __atomic_load_n(&device.gone,__ATOMIC_ACQUIRE) ||
            __atomic_load_n(&device.mscQuarantined,__ATOMIC_ACQUIRE);
    }
    if (cmd == DEVICE_IOCTL_WRITE_WINDOW || cmd == DEVICE_IOCTL_USB_SYSTEM_WINDOW) {
        const auto* window=static_cast<const DeviceWriteWindow*>(arg);
        const bool persistent=cmd==DEVICE_IOCTL_USB_SYSTEM_WINDOW;
        if(!g_storageReadOnly || !window || (persistent && !UsbSystemRequested()))return -22;
        MscIoGuard guard;
        if(window->length && !persistent && !st->windows.CanOpenUpdate(UsbSystemRequested()))return -30;
        UsbMscByteTransport io{st};
        const auto block=io.blockSize();
        if(window->length && (!io.usable() || __atomic_load_n(&st->systemFailed,__ATOMIC_ACQUIRE) ||
                               (persistent && block!=512)))return -5;
        if(!st->windows.Set(persistent,*window,io.blockCount()*block,block))return -22;
        KPrintf("xhci: %s %s write window %s\n",st->name,
            persistent?"USB system":"boot-stick update",window->length?"open":"closed");
        return 0;
    }
    if (cmd != DEVICE_IOCTL_FLUSH) return -95;
    MscIoGuard ioGuard;
    if(__atomic_load_n(&st->systemFailed,__ATOMIC_ACQUIRE))return -5;
    const bool system=st->windows.system.length!=0;
    if(g_storageReadOnly && !system && !st->windows.update.length)return 0;
    UsbMscByteTransport io{st};
    if(!io.usable()){UsbMscFenceSystem(*st);return -5;}
    MscCommandGuard commandGuard;
    XhciDevice& udev=g_devices[st->devIndex];
    if(!system && udev.mscUnsupportedSync &&
       (udev.mscCache==UsbScsiCache::WriteThrough||udev.mscCache==UsbScsiCache::AssumedWriteThrough))return 0;
    // IMMED=0, LBA=0 and length=0: wait for the entire device cache.
    uint8_t cdb[10] = {};cdb[0] = SCSI_SYNCHRONIZE_CACHE_10;
    auto result=XhciMscExecuteLocked(*st->ctrl,udev,cdb,sizeof(cdb),nullptr,0,0,false);
    if(result.success()){StorageIoAccount(uint32_t(StorageIoSource::Usb),2,0);return 0;}
    if(!system && UsbScsiUnsupportedSync(result.sense)) {
        if(udev.mscCache==UsbScsiCache::Unknown)udev.mscCache=XhciMscCacheLocked(*st->ctrl,udev);
        if(!__atomic_load_n(&udev.mscQuarantined,__ATOMIC_ACQUIRE)&&UsbScsiWriteThroughFlush(udev.mscCache,result.sense)) {
            udev.mscUnsupportedSync=true;
            if(udev.mscCache==UsbScsiCache::WriteThrough)
                KPuts("xhci: MSC flush uses reported write-through policy; unsupported opcode 35\n");
            else KPuts("xhci: MSC flush assumes write-through: complete all-pages mode has no caching page; unsupported opcode 35\n");
            return 0;
        }
    }
    UsbMscFenceSystem(*st);
    return -5;
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

static void XhciDelayMs(uint32_t ms);

static bool XhciIdentifyStorage(XhciController& ctrl,XhciDevice& dev) {
    XhciMscInquiry(ctrl,dev);
    for(int retry=0;retry<3;++retry) {
        if(XhciMscTestUnitReady(ctrl,dev))break;
        XhciDelayMs(10);
    }
    return XhciMscReadCapacity(ctrl,dev);
}

// Try UAS before the device is registered and before any write lease exists.
// On a refused init, retire every UAS endpoint and select BOT again. A lost
// Configure Endpoint completion leaves ownership unknown and forbids fallback.
static bool XhciTryUas(XhciController& ctrl,XhciDevice& dev,bool& fallback) {
    const bool bot=dev.bulkInAddr && dev.bulkOutAddr;
    fallback=bot;
    if(!UsbSpeedIsSuper(dev.portSpeed) ||
       ((xhci_read32(ctrl.capBase,XHCI_CAP_HCCPARAMS1)>>12)&15)<2)return false;
    UsbUasConfig parsed{};
    if(!UsbUasParse(dev.rawConfig,dev.rawConfigLen,bot?int(dev.mscInterface):-1,parsed) ||
       !XhciUasCanClaim(ctrl,dev,parsed))return false;
    auto* uas=XhciAllocateUas(parsed);
    if(!uas)return false;
    const auto selectStart=MscNowNs();
    const bool selected=!parsed.alternate ||
        XhciSelectStorageInterface(ctrl,dev,parsed.interface,parsed.alternate);
    dev.mscTiming.uasSetInterfaceUs=(MscNowNs()-selectStart)/1000;
    if(!selected) {
        if(bot)fallback=XhciSelectStorageInterface(ctrl,dev,dev.mscInterface,dev.mscAlternate);
        XhciFreeUas(uas);
        return false;
    }
    const auto configureStart=MscNowNs();
    const uint32_t cc=XhciConfigureUas(ctrl,dev,*uas);
    dev.mscTiming.uasConfigureUs=(MscNowNs()-configureStart)/1000;
    if(cc==TRB_CC_SUCCESS) {
        dev.uas=uas;
        const auto identifyStart=MscNowNs();
        const bool identified=XhciIdentifyStorage(ctrl,dev);
        dev.mscTiming.uasIdentifyUs=(MscNowNs()-identifyStart)/1000;
        if(identified) {
            KPrintf("xhci: UAS ready slot=%u iface=%u alt=%u depth=%u\n",
                dev.slotId,parsed.interface,parsed.alternate,UsbUasDepth);
            return true;
        }
    }
    uint32_t mask=0;
    for(const auto& ep:parsed.pipes)mask|=1u<<XhciUasDci(ep.address);
    fallback=UsbUasRestoreBot(cc!=0,bot,[&] {
        if(XhciDropStorageEndpoints(ctrl,dev,mask)!=TRB_CC_SUCCESS)return false;
        XhciDiscardStorageEvents(ctrl,dev,mask);
        dev.uas=nullptr;
        XhciFreeUas(uas);
        return true;
    },[&] {
        if(!XhciSelectStorageInterface(ctrl,dev,dev.mscInterface,dev.mscAlternate))return false;
        __atomic_store_n(&dev.mscDiscardDcis,0,__ATOMIC_RELEASE);
        __atomic_store_n(&dev.mscQuarantined,false,__ATOMIC_RELEASE);
        return true;
    });
    KPrintf("xhci: UAS init failed slot=%u cc=%u BOT fallback=%u\n",
        dev.slotId,cc,fallback?1u:0u);
    return false;
}

static bool XhciStartBot(XhciController& ctrl,XhciDevice& dev) {
    return XhciConfigureBulkEndpoint(ctrl,dev,dev.bulkInAddr,dev.bulkInMaxPacket,
           dev.bulkInRing,dev.bulkInRingPhys,dev.bulkInEnqueue,dev.bulkInCycle,dev.bulkInDci) &&
       XhciConfigureBulkEndpoint(ctrl,dev,dev.bulkOutAddr,dev.bulkOutMaxPacket,
           dev.bulkOutRing,dev.bulkOutRingPhys,dev.bulkOutEnqueue,dev.bulkOutCycle,dev.bulkOutDci) &&
       XhciIdentifyStorage(ctrl,dev);
}

static void XhciProbeBotLimit(UsbMscState& storage,XhciDevice& dev) {
    auto* state=&storage;
    auto& ctrl=*state->ctrl;
    MscCommandGuard guard;
    auto inquiryPage=[&](uint8_t page) {
        constexpr uint8_t length=64;
        const uint8_t cdb[6]={SCSI_INQUIRY,1,page,0,length,0};
        memset(state->bounce,0,length);
        return XhciMscExecuteLocked(ctrl,dev,cdb,sizeof(cdb),state->bounce,
            state->bouncePhys,length,true,true);
    };
    const auto supported=inquiryPage(0);
    auto* data=state->bounce;
    if(supported.success() && ScsiUnmapVpd(data,supported.actual,0,4)) {
        const uint32_t length=uint32_t(data[2])<<8|data[3];
        bool hasLimits=false;
        for(uint32_t i=0;i<length && i<supported.actual-4;++i)hasLimits|=data[4+i]==0xb0;
        if(hasLimits) {
            const auto limits=inquiryPage(0xb0);
            if(limits.success() && ScsiUnmapVpd(data,limits.actual,0xb0,12)) {
                const uint64_t maximum=uint64_t(ScsiUnmapBe32(data+8))*dev.mscBlockSize;
                if(maximum && maximum<state->bounceCapacity)state->botMaxBytes=maximum;
            }
        }
    }
}

static int XhciPromoteSystemStorage(UsbMscState& state) {
    MscIoGuard guard;
    auto& ctrl=*state.ctrl;
    auto& dev=g_devices[state.devIndex];
    if(dev.uas)return 0;
    if(state.windows.heldSystem || !UsbMscByteTransport{&state}.usable())return -5;
    const uint32_t mask=(1u<<dev.bulkInDci)|(1u<<dev.bulkOutDci);
    UsbUasConfig parsed{};
    if(!g_storageUas || !UsbSpeedIsSuper(dev.portSpeed) ||
       ((xhci_read32(ctrl.capBase,XHCI_CAP_HCCPARAMS1)>>12)&15)<2 ||
       !UsbUasParse(dev.rawConfig,dev.rawConfigLen,dev.mscInterface,parsed) ||
       !XhciUasCanClaim(ctrl,dev,parsed,mask)) {
        XhciProbeBotLimit(state,dev);
        return UsbMscByteTransport{&state}.usable()?-95:-5;
    }
    const auto block=dev.mscBlockSize;
    const auto sectors=dev.mscSectorCount;
    const auto dropStart=MscNowNs();
    if(XhciDropStorageEndpoints(ctrl,dev,mask)!=TRB_CC_SUCCESS)return -5;
    XhciDiscardStorageEvents(ctrl,dev,mask);
    FreeDmaBuffer(dev.bulkInRingPhys,1);
    FreeDmaBuffer(dev.bulkOutRingPhys,1);
    dev.bulkInRing=nullptr;dev.bulkOutRing=nullptr;
    dev.bulkInRingPhys=0;dev.bulkOutRingPhys=0;
    dev.mscTiming.uasDropBotUs=(MscNowNs()-dropStart)/1000;
    bool fallback=false;
    const bool attached=XhciTryUas(ctrl,dev,fallback) || (fallback && XhciStartBot(ctrl,dev));
    if(attached && !dev.uas)XhciProbeBotLimit(state,dev);
    return attached && UsbMscByteTransport{&state}.usable() && dev.mscBlockSize==block && dev.mscSectorCount==sectors?0:-5;
}

static bool XhciInitMassStorage(XhciController& ctrl,XhciDevice& dev,uint32_t devIndex) {
    SerialPuts("xhci: initializing mass storage device\n");
    if(dev.bulkInAddr && dev.bulkOutAddr) {
        if(!XhciStartBot(ctrl,dev))return false;
    } else {
        bool fallback=false;
        if(!XhciUasFixture(dev) || !XhciTryUas(ctrl,dev,fallback))return false;
    }

    // Register as block device
    auto* state = static_cast<UsbMscState*>(kmalloc(sizeof(UsbMscState)));
    if (!state) return false;
    state->ctrl = &ctrl;
    state->devIndex = devIndex;
    state->windows={};
    state->systemFailed=false;
    state->unmapLimits={};
    state->botMaxBytes=0;
    state->bounceCapacity=65536;
    state->bounce=nullptr;
    if(UsbSystemRequested() && !g_storagePageChunks && !g_storageLegacyBurst) {
        state->bounce=static_cast<uint8_t*>(AllocDmaBuffer(USB_MSC_MAX_BULK_BYTES/4096,state->bouncePhys));
        if(state->bounce)state->bounceCapacity=USB_MSC_MAX_BULK_BYTES;
    }
    if(!state->bounce)
        state->bounce=static_cast<uint8_t*>(AllocDmaBuffer(state->bounceCapacity/4096,state->bouncePhys));
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
// Topology: enumeration, hubs, root ports, hotplug and the status text
// ---------------------------------------------------------------------------

static bool g_threadContext = false; // true on the hotplug worker (may sleep)

static void XhciDelayMs(uint32_t ms)
{
    if (g_threadContext) { SchedulerSleepMs(ms); return; }
    const uint64_t start = ApicTickCount();
    while (ApicTickCount() - start < ms) __asm__ volatile("pause");
}

static void XhciPathText(const XhciDevice& dev, char* out, uint32_t cap)
{
    // Root port first, then each hub port: "3", "3.2", "3.2.4".
    uint8_t ports[UsbMaxRouteDepth + 1]; uint32_t n = 0;
    const XhciDevice* cur = &dev;
    while (cur && n <= UsbMaxRouteDepth) {
        ports[n++] = cur->parent ? cur->parentPort : static_cast<uint8_t>(cur->topo.rootPort);
        cur = cur->parent ? &g_devices[cur->parent - 1] : nullptr;
    }
    uint32_t len = 0;
    for (uint32_t i = n; i-- > 0;) {
        if (len + 5 >= cap) break;
        uint32_t v = ports[i];
        if (v >= 100) out[len++] = char('0' + v / 100);
        if (v >= 10) out[len++] = char('0' + (v / 10) % 10);
        out[len++] = char('0' + v % 10);
        if (i) out[len++] = '.';
    }
    out[len] = 0;
}

static const char* XhciDeviceKind(const XhciDevice& dev)
{
    if (dev.isHub) return "hub";
    if (dev.network || dev.ecm.value) return "net";
    if (dev.isKeyboard && dev.isMouse) return "kbd+mouse";
    if (dev.isKeyboard) return "kbd";
    if (dev.isMouse) return "mouse";
    if (dev.isMassStorage) return "storage";
    return "device";
}

// Detach: return the device's own DMA buffers (contexts, EP0 ring, scratch,
// HID rings/report buffers) once its slot is disabled, so hot-plug does not
// exhaust low memory or the reserve. HID rings are unhooked first so the
// event path can never match a freed page.
static void XhciReleaseDeviceDma(XhciDevice& dev)
{
    const uint32_t ctxSize = CtxEntrySize(*dev.ctrl);
    const uint32_t inputPages = (ctxSize * 33 + 4095) / 4096, outputPages = (ctxSize * 32 + 4095) / 4096;
    XhciHidFn* fns[] = {&dev.kbd, &dev.mouse, &dev.fidoIn, &dev.fidoOut};
    constexpr unsigned endpointCount = sizeof(fns) / sizeof(fns[0]);
    const auto saved = IrqSpinLockAcquire(&g_eventRingLock);
    uint64_t hidRing[endpointCount] = {}, hidReport[endpointCount] = {};
    for (unsigned i = 0; i < endpointCount; ++i) {
        hidRing[i] = fns[i]->ringPhys; hidReport[i] = fns[i]->reportPhys;
        fns[i]->ring = nullptr; fns[i]->ringPhys = 0; fns[i]->report = nullptr; fns[i]->reportPhys = 0;
    }
    IrqSpinLockRelease(&g_eventRingLock, saved);
    for (unsigned i = 0; i < endpointCount; ++i) { FreeDmaBuffer(hidRing[i], 1); FreeDmaBuffer(hidReport[i], 1); }
    FreeDmaBuffer(dev.ep0RingPhys, 1); dev.ep0Ring = nullptr; dev.ep0RingPhys = 0;
    FreeDmaBuffer(dev.inputCtxPhys, inputPages); dev.inputCtx = nullptr; dev.inputCtxPhys = 0;
    FreeDmaBuffer(dev.outputCtxPhys, outputPages); dev.outputCtx = nullptr; dev.outputCtxPhys = 0;
    FreeDmaBuffer(dev.scratchPhys, 1); dev.scratch = nullptr; dev.scratchPhys = 0;
}
static bool XhciUacOwnsDevice(uint32_t index);
// Devices other threads may still touch keep their buffers (rare on hot-plug).
static bool XhciDeviceDmaReleasable(uint32_t index)
{
    const auto& d = g_devices[index];
    return !d.network && !d.isMassStorage && !d.ecm.value && !XhciUacOwnsDevice(index);
}

static void XhciDeviceGone(uint32_t index)
{
    auto& dev = g_devices[index];
    if (dev.gone) return;
    if (dev.isHub) {
        for (uint32_t p = 0; p < dev.hubPortCount; ++p) {
            if (dev.hubChild[p]) XhciDeviceGone(dev.hubChild[p] - 1u);
            dev.hubChild[p] = 0;
        }
    }
    const auto saved = IrqSpinLockAcquire(&g_eventRingLock);
    __atomic_store_n(&dev.gone,true,__ATOMIC_RELEASE);
    const bool hadHid = dev.kbd.active || dev.mouse.active;
    XhciEndHid(dev.kbd,true);
    XhciEndHid(dev.mouse,false);
    dev.fidoIn.active = false;
    if (dev.network) dev.network->failed = true;
    IrqSpinLockRelease(&g_eventRingLock, saved);
    if (dev.fidoIdentity) HidrawDetach(dev.fidoIdentity);
    if (hadHid) {
        CompositorWake();
        InputWakeWaiters();
    }
    char path[24]; XhciPathText(dev, path, sizeof(path));
    KPrintf("xhci: c%u p%s %s %04x:%04x disconnected (slot %u)\n", dev.ctrl->index, path,
            XhciDeviceKind(dev), dev.vendorId, dev.productId, dev.slotId);
    if (XhciDisableSlot(*dev.ctrl, dev.slotId) && XhciDeviceDmaReleasable(index)) XhciReleaseDeviceDma(dev);
}

// ---- HID ------------------------------------------------------------------

static void XhciKeyboardPoll(InputDevice* /*dev*/);
static void XhciMousePoll(InputDevice* /*dev*/);
static bool g_kbdRegistered = false, g_mouseRegistered = false;

static void XhciQueueHidTd(XhciController& ctrl, XhciDevice& dev, XhciHidFn& fn)
{
    uint32_t idx = fn.enqueue;
    if (idx >= XFER_RING_SIZE - 1) {
        fn.ring[XFER_RING_SIZE - 1].control = TRB_TYPE_LINK | TRB_TC | (fn.cycle ? TRB_CYCLE : 0);
        fn.cycle = !fn.cycle;
        idx = 0;
    }
    Trb& trb = fn.ring[idx];
    trb.param = fn.reportPhys;
    trb.status = fn.length;
    const uint32_t shortPacketEvent = (fn.dci & 1) ? TRB_ISP : 0;
    trb.control = TRB_TYPE_NORMAL | TRB_IOC | shortPacketEvent | (fn.cycle ? TRB_CYCLE : 0);
    fn.enqueue = idx + 1;
    __asm__ volatile("mfence" ::: "memory");
    XhciRingDoorbell(ctrl, dev.slotId, fn.dci);
}

static bool XhciStartHid(XhciController& ctrl, XhciDevice& dev, XhciHidFn& fn, bool keyboard)
{
    const bool bootProtocol = XhciSetBootProtocol(ctrl, dev, fn.iface); // A stall is recovered.
    if (!XhciConfigureInterrupt(ctrl, dev, fn.ep, fn.packet, fn.interval, fn.ring, fn.ringPhys,
                            fn.enqueue, fn.cycle, fn.dci)) return false;
    fn.report = static_cast<uint8_t*>(AllocDmaBuffer(1, fn.reportPhys));
    if (!fn.report) return false;
    const uint32_t packet = fn.packet & 0x7ffu;
    fn.length = keyboard ? 8u : (packet < 8 ? packet : 8u);
    if (!keyboard) {
        auto& ep = fn.pointerEndpoint;
        XhciPathText(dev, ep.port, sizeof(ep.port));
        ep.vendor = dev.vendorId;
        ep.product = dev.productId;
        ep.controller = ctrl.index;
        ep.slot = dev.slotId;
        ep.endpoint = fn.ep;
        ep.speed = dev.portSpeed;
        ep.ttSlot = dev.topo.ttSlot;
        ep.ttPort = dev.topo.ttPort;
        ep.interval = fn.interval;
        uint32_t exponent = 0;
        UsbXhciInterval(dev.portSpeed, fn.interval, exponent);
        ep.esitExponent = exponent;
        ep.requestedBytes = fn.length;
        ep.bootProtocol = bootProtocol;
    }
    if (keyboard && !g_kbdRegistered) {
        g_usbKbdOps.poll = XhciKeyboardPoll;
        g_usbKbdDev.ops = &g_usbKbdOps;
        g_usbKbdDev.head = 0;
        g_usbKbdDev.tail = 0;
        if (!InputRegister(&g_usbKbdDev)) return false;
        g_kbdRegistered = true;
        SerialPuts("xhci: USB keyboard registered as input device\n");
    }
    if (!keyboard && !g_mouseRegistered) {
        g_usbMouseOps.poll = XhciMousePoll;
        g_usbMouseDev.ops = &g_usbMouseOps;
        g_usbMouseDev.head = 0;
        g_usbMouseDev.tail = 0;
        if (!InputRegister(&g_usbMouseDev)) return false;
        g_mouseRegistered = true;
        SerialPuts("xhci: USB mouse registered as input device\n");
    }
    const auto saved = IrqSpinLockAcquire(&g_eventRingLock);
    fn.active = true;
    if (!keyboard && ++g_activeUsbMice == 1) MouseSetUsbAvailable(true);
    XhciQueueHidTd(ctrl, dev, fn);
    if (!keyboard) XhciMarkPointer(fn, UsbPointerStage::Queued);
    IrqSpinLockRelease(&g_eventRingLock, saved);
    return true;
}

static int XhciFidoQueueOutput(void* cookie, uint32_t identity, const uint8_t* report)
{
    auto& dev = *static_cast<XhciDevice*>(cookie);
    const auto saved = IrqSpinLockAcquire(&g_eventRingLock);
    const bool live = !dev.gone && dev.fidoIn.active && dev.fidoIdentity == identity;
    if (live) {
        memcpy(dev.fidoOut.report, report, UsbFidoReportBytes);
        dev.fidoTxPending = true;
        dev.fidoTxStarted = ApicTickCount();
        XhciQueueHidTd(*dev.ctrl, dev, dev.fidoOut);
    }
    IrqSpinLockRelease(&g_eventRingLock, saved);
    return live ? 0 : -19;
}

static void XhciProbeFido(XhciController& ctrl, XhciDevice& dev, const char* path)
{
    static_assert(UsbFidoReportBytes == HidrawReportBytes);
    static_assert(UsbFidoDescriptorLimit == HidrawDescriptorLimit);
    UsbFidoInterface iface{};
    unsigned first = 0;
    while (UsbFidoFindInterface(dev.rawConfig, dev.rawConfigLen, first, iface)) {
        first = unsigned(iface.number) + 1;
        UsbSetupPacket setup{};
        setup.bmRequestType = 0x81; // Standard, device to host, interface.
        setup.bRequest = USB_REQ_GET_DESCRIPTOR;
        setup.wValue = 0x2200;
        setup.wIndex = iface.number;
        setup.wLength = iface.descriptorBytes;
        uint32_t received = 0;
        if (!XhciControlTransfer(ctrl, dev, setup, dev.scratch, iface.descriptorBytes,
                                 dev.scratchPhys, 3, &received) || received != iface.descriptorBytes)
            continue;
        const bool fido = UsbFidoReportDescriptor(dev.scratch, received);
        if (fido || dev.vendorId == 0x1050) {
            constexpr unsigned LogChunk = 48;
            constexpr char digits[] = "0123456789abcdef";
            for (unsigned offset = 0; offset < received; offset += LogChunk) {
                char hex[2 * LogChunk + 1];
                const unsigned count = received - offset < LogChunk ? received - offset : LogChunk;
                for (unsigned i = 0; i < count; ++i) {
                    const uint8_t byte = dev.scratch[offset + i];
                    hex[i * 2] = digits[byte >> 4];
                    hex[i * 2 + 1] = digits[byte & 15];
                }
                hex[count * 2] = 0;
                KPrintf("USB_FIDO_DESCRIPTOR c=%u port=%s vid=%04x pid=%04x iface=%u in=%02x out=%02x match=%u offset=%u size=%u bytes=%s\n",
                        ctrl.index, path, dev.vendorId, dev.productId, iface.number, iface.in, iface.out,
                        fido, offset, received, hex);
            }
        }
        if (!fido) continue;
        auto configure = [&](XhciHidFn& endpoint, uint8_t address, uint8_t interval) {
            endpoint.length = UsbFidoReportBytes;
            endpoint.report = static_cast<uint8_t*>(AllocDmaBuffer(1, endpoint.reportPhys));
            return endpoint.report &&
                XhciConfigureInterrupt(ctrl, dev, address, UsbFidoReportBytes, interval, endpoint.ring,
                                       endpoint.ringPhys, endpoint.enqueue, endpoint.cycle, endpoint.dci);
        };
        XhciHidFn input{}, output{};
        const bool endpoints = configure(input, iface.in, iface.inInterval) &&
                               configure(output, iface.out, iface.outInterval);
        const auto installed = IrqSpinLockAcquire(&g_eventRingLock);
        dev.fidoIn = input;
        dev.fidoOut = output;
        IrqSpinLockRelease(&g_eventRingLock, installed);
        if (!endpoints) {
            dev.note = "FIDO endpoint setup failed";
            return;
        }
        const uint32_t identity = HidrawAttach(dev.vendorId, dev.productId, dev.scratch, received,
                                              XhciFidoQueueOutput, &dev);
        if (!identity) {
            dev.note = "FIDO node capacity exhausted";
            return;
        }
        const auto saved = IrqSpinLockAcquire(&g_eventRingLock);
        dev.fidoIdentity = identity;
        dev.fidoIn.active = true;
        XhciQueueHidTd(ctrl, dev, dev.fidoIn);
        IrqSpinLockRelease(&g_eventRingLock, saved);
        HidrawActivate(identity);
        return;
    }
}

static void XhciFidoCheckTimeouts()
{
    const auto saved = IrqSpinLockAcquire(&g_eventRingLock);
    const auto now = ApicTickCount();
    const auto count = __atomic_load_n(&g_deviceCount, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < count; ++i) {
        auto& dev = g_devices[i];
        if (!dev.gone && dev.fidoIn.active && dev.fidoTxPending && now - dev.fidoTxStarted >= 2000) {
            dev.fidoIn.active = false;
            HidrawTransportFailed(dev.fidoIdentity);
        }
    }
    IrqSpinLockRelease(&g_eventRingLock, saved);
}

// ---- Enumeration ----------------------------------------------------------

struct XhciEnumResult { const char* stage; uint8_t cc; };
static bool XhciHubSetup(XhciController& ctrl, uint32_t index);
static bool XhciHubScan(XhciController& ctrl, uint32_t index, bool initial);

// Enumerate one device on a root port (parent < 0) or a hub port. Returns the
// device index, or -1 with the failing stage in 'result'.
static int XhciEnumerate(XhciController& ctrl, int parent, uint8_t port, uint32_t speed,
                         XhciEnumResult& result)
{
    result = {nullptr, 0};
    if (g_deviceCount >= MAX_DEVICES) { result.stage = "device table full"; return -1; }
    if (speed < UsbSpeedFull || speed > UsbSpeedSuperPlus) { result.stage = "unknown speed"; return -1; }
    UsbTopology topo;
    if (parent < 0) topo = UsbRootTopology(port, speed);
    else {
        const auto& hub = g_devices[parent];
        if (!UsbChildTopology(hub.topo, static_cast<uint8_t>(hub.slotId), hub.hubMulti, port, speed, topo)) {
            result.stage = "hub tier limit"; return -1;
        }
    }
    const int slot = XhciEnableSlot(ctrl);
    if (slot < 0) { result.stage = "enable slot"; return -1; }

    const uint32_t index = g_deviceCount;
    XhciDevice& dev = g_devices[index];
    dev = {};
    dev.ctrl = &ctrl;
    dev.slotId = static_cast<uint32_t>(slot);
    dev.portNum = topo.rootPort;
    dev.portSpeed = speed;
    dev.topo = topo;
    dev.parent = static_cast<uint8_t>(parent + 1);
    dev.parentPort = port;
    dev.ep0Cycle = true;
    dev.intCycle = true;
    dev.scratch = static_cast<uint8_t*>(AllocDmaBuffer(1, dev.scratchPhys));

    const char* stage = nullptr;
    if (!dev.scratch) stage = "scratch allocation";
    else if (!XhciAddressDevice(ctrl, dev)) stage = "address device";
    else if (!XhciGetDeviceDescriptor(ctrl, dev)) stage = "device descriptor";
    else if (!XhciConfigureDevice(ctrl, dev)) stage = "configuration";
    if (stage) {
        result.stage = stage; result.cc = dev.lastCc;
        // Not yet published: no other thread can reach this device's buffers.
        if (XhciDisableSlot(ctrl, dev.slotId) || !dev.slotId) XhciReleaseDeviceDma(dev);
        dev.slotId = 0;
        return -1;
    }
    __atomic_store_n(&g_deviceCount, index + 1, __ATOMIC_RELEASE);

    char path[24]; XhciPathText(dev, path, sizeof(path));
    KPrintf("xhci: c%u p%s %s %04x:%04x %s (slot %u)\n", ctrl.index, path, UsbSpeedName(speed),
            dev.vendorId, dev.productId, XhciDeviceKind(dev), dev.slotId);

    if (dev.isKeyboard && !XhciStartHid(ctrl, dev, dev.kbd, true)) dev.note = "keyboard setup failed";
    if (dev.isMouse) {
        BootTimingUsbPointer(false, ctrl.index, path, dev.vendorId, dev.productId);
        if (XhciStartHid(ctrl, dev, dev.mouse, false))
            BootTimingUsbPointer(true, ctrl.index, path, dev.vendorId, dev.productId);
        else
            dev.note = "mouse setup failed";
    }
    XhciProbeFido(ctrl, dev, path);
    if (dev.ecm.value && !XhciInitEcm(ctrl, dev)) dev.note = "ECM setup failed";
    if(dev.isMassStorage && !XhciInitMassStorage(ctrl,dev,index))dev.note="storage setup failed";
    if (dev.isHub && XhciHubSetup(ctrl, index)) XhciHubScan(ctrl, index, true);
    return static_cast<int>(index);
}

// ---- Hubs -------------------------------------------------------------------

static bool XhciHubRequest(XhciController& ctrl, XhciDevice& hub, uint8_t type, uint8_t request,
                           uint16_t value, uint16_t index)
{
    UsbSetupPacket setup{};
    setup.bmRequestType = type; setup.bRequest = request; setup.wValue = value; setup.wIndex = index;
    return XhciControlTransfer(ctrl, hub, setup, nullptr, 0, 0, 0);
}

static bool XhciHubPortStatus(XhciController& ctrl, XhciDevice& hub, uint8_t port,
                              uint16_t& status, uint16_t& change)
{
    UsbSetupPacket setup{};
    setup.bmRequestType = 0xA3; setup.bRequest = USB_HUB_REQ_GET_STATUS; setup.wIndex = port; setup.wLength = 4;
    if (!XhciControlTransfer(ctrl, hub, setup, hub.scratch, 4, hub.scratchPhys, 3)) return false;
    status = static_cast<uint16_t>(hub.scratch[0] | (hub.scratch[1] << 8));
    change = static_cast<uint16_t>(hub.scratch[2] | (hub.scratch[3] << 8));
    return true;
}

static void XhciHubAck(XhciController& ctrl, XhciDevice& hub, uint8_t port, uint16_t change)
{
    uint16_t features[8];
    const uint32_t n = UsbHubChangeFeatures(change, hub.hubSuper, features, 8);
    for (uint32_t i = 0; i < n; ++i)
        XhciHubRequest(ctrl, hub, 0x23, USB_HUB_REQ_CLEAR_FEATURE, features[i], port);
}

// Reset one hub port and wait for C_PORT_RESET (or C_BH_PORT_RESET).
static bool XhciHubPortReset(XhciController& ctrl, XhciDevice& hub, uint8_t port, bool warm,
                             uint16_t& status)
{
    if (!XhciHubRequest(ctrl, hub, 0x23, USB_HUB_REQ_SET_FEATURE,
                        warm ? UsbHubFeatBhPortReset : UsbHubFeatPortReset, port)) return false;
    const uint16_t done = warm ? 0x20 : 0x10;
    for (uint32_t waited = 0; waited < 1000; waited += 10) {
        XhciDelayMs(10);
        uint16_t change = 0;
        if (!XhciHubPortStatus(ctrl, hub, port, status, change)) return false;
        if ((change & done) && !(status & 0x10)) {
            XhciHubAck(ctrl, hub, port, change);
            return XhciHubPortStatus(ctrl, hub, port, status, change);
        }
    }
    return false;
}

static bool XhciHubSetup(XhciController& ctrl, uint32_t index)
{
    auto& hub = g_devices[index];
    hub.hubSuper = UsbSpeedIsSuper(hub.portSpeed);
    if (hub.topo.depth >= UsbMaxRouteDepth) { hub.note = "hub beyond tier limit"; return false; }
    // USB 3.2 §10.16.2.9: a SuperSpeed hub must learn its depth first.
    if (hub.hubSuper && !XhciHubRequest(ctrl, hub, 0x20, 12 /* SET_HUB_DEPTH */, hub.topo.depth, 0)) {
        hub.note = "SET_HUB_DEPTH failed"; return false;
    }
    UsbSetupPacket setup{};
    setup.bmRequestType = 0xA0; setup.bRequest = USB_HUB_REQ_GET_DESCRIPTOR;
    setup.wValue = static_cast<uint16_t>((hub.hubSuper ? USB_DESC_HUB_USB3 : USB_DESC_HUB_USB2) << 8);
    setup.wLength = 16;
    uint32_t got = 0;
    if (!XhciControlTransfer(ctrl, hub, setup, hub.scratch, 16, hub.scratchPhys, 3, &got) || got < 7) {
        hub.note = "hub descriptor failed"; return false;
    }
    uint8_t ports = hub.scratch[2];
    const uint16_t characteristics = static_cast<uint16_t>(hub.scratch[3] | (hub.scratch[4] << 8));
    const uint32_t powerDelay = hub.scratch[5] * 2u;
    if (!ports) { hub.note = "hub reports no ports"; return false; }
    if (ports > XHCI_HUB_MAX_PORTS) ports = XHCI_HUB_MAX_PORTS;
    hub.hubPortCount = ports;
    hub.hubThink = static_cast<uint8_t>((characteristics >> 5) & 3u);
    hub.hubMulti = false; // interface alternate 0 runs single-TT

    // Mark the slot as a hub (Hub, Number of Ports, TTT) so the xHC can
    // address and schedule devices behind it (xHCI 1.2 §4.6.6/§6.2.2).
    const uint32_t ctxSize = CtxEntrySize(ctrl);
    auto* input = static_cast<uint8_t*>(hub.inputCtx);
    const auto* out = reinterpret_cast<const uint32_t*>(hub.outputCtx);
    memset(input, 0, ctxSize * 33);
    reinterpret_cast<uint32_t*>(input)[1] = 1u; // A0: slot
    uint32_t entries = (out[0] >> 27) & 31u;
    if (!entries) entries = 1;
    const auto words = UsbSlotContext(hub.topo, entries, true, ports, hub.hubThink, hub.hubMulti);
    auto* slot = reinterpret_cast<uint32_t*>(input + ctxSize);
    slot[0] = words.dw0; slot[1] = words.dw1; slot[2] = words.dw2; slot[3] = 0; // dw3 is xHC-owned
    __asm__ volatile("mfence" ::: "memory");
    // Hub, MTT, Number of Ports and TTT are evaluated by Configure Endpoint
    // with A0 (xHCI 1.2 §4.6.6, §6.2.2.2); Evaluate Context only on 0.95
    // hosts (as Linux xhci_update_hub_device). Fall back to the other one.
    const uint32_t firstCommand = ctrl.hciVersion > 0x95 ? TRB_TYPE_CONFIG_EP : TRB_TYPE_EVAL_CTX;
    const uint32_t otherCommand = ctrl.hciVersion > 0x95 ? TRB_TYPE_EVAL_CTX : TRB_TYPE_CONFIG_EP;
    uint32_t cc = XhciSlotCommand(ctrl, hub.inputCtxPhys, firstCommand | (hub.slotId << 24), 2000);
    if (cc != TRB_CC_SUCCESS) {
        const uint32_t first = cc;
        cc = XhciSlotCommand(ctrl, hub.inputCtxPhys, otherCommand | (hub.slotId << 24), 2000);
        if (cc != TRB_CC_SUCCESS) {
            SerialPrintf("xhci: hub slot %u context update failed cc=%u/%u\n", hub.slotId, first, cc);
            hub.note = "hub slot context not accepted";
        }
    }

    for (uint8_t p = 1; p <= ports; ++p)
        XhciHubRequest(ctrl, hub, 0x23, USB_HUB_REQ_SET_FEATURE, UsbHubFeatPortPower, p);
    // Power-on to power-good, then the 100 ms connect debounce.
    XhciDelayMs((powerDelay > 100 ? powerDelay : 100) + 100);
    hub.hubReady = true;
    SerialPrintf("xhci: hub slot %u %s %u ports think=%u ready\n", hub.slotId,
                 hub.hubSuper ? "SuperSpeed" : "USB2", ports, hub.hubThink);
    return true;
}

static const char* const kHubPortStatusFailed = "port status failed";
static bool XhciHubScan(XhciController& ctrl, uint32_t index, bool initial)
{
    auto& hub = g_devices[index];
    if (!hub.hubReady || hub.gone || hub.controlQuarantined) return false;
    bool changed = false;
    for (uint8_t p = 1; p <= hub.hubPortCount; ++p) {
        const uint32_t i = p - 1u;
        uint16_t status = 0, change = 0;
        if (!XhciHubPortStatus(ctrl, hub, p, status, change)) {
            if (hub.hubError[i] != kHubPortStatusFailed) changed = true;
            hub.hubError[i] = kHubPortStatusFailed;
            if (hub.controlQuarantined) { hub.note = "hub control endpoint failed"; return true; }
            continue;
        }
        if (change) XhciHubAck(ctrl, hub, p, change);
        hub.hubPortStatus[i] = status;
        auto state = UsbHubDecodePort(status, hub.hubSuper);
        if (hub.hubChild[i] && (!state.connected || (change & 1u))) {
            XhciDeviceGone(hub.hubChild[i] - 1u);
            hub.hubChild[i] = 0; changed = true;
        }
        if (!state.connected) {
            if (hub.hubError[i] || hub.hubTries[i]) changed = true;
            hub.hubError[i] = nullptr; hub.hubTries[i] = 0; hub.hubCc[i] = 0;
            continue;
        }
        if (hub.hubChild[i] || hub.hubTries[i] >= 3) continue;
        ++hub.hubTries[i]; changed = true;
        if (!initial && (change & 1u)) XhciDelayMs(100); // connect debounce
        // A SuperSpeed hub port may still be training; give it ~1 s.
        for (uint32_t waited = 0; hub.hubSuper && !state.enabled && waited < 1000 &&
             (state.linkState == 5 || state.linkState == 7 || state.linkState == 8 || state.linkState == 9);
             waited += 50) {
            XhciDelayMs(50);
            uint16_t again = 0;
            if (!XhciHubPortStatus(ctrl, hub, p, status, again)) break;
            if (again) XhciHubAck(ctrl, hub, p, again);
            state = UsbHubDecodePort(status, hub.hubSuper);
        }
        hub.hubPortStatus[i] = status;
        const bool warm = hub.hubSuper && (state.linkState == 6 || state.linkState == 10);
        if (!hub.hubSuper || !state.enabled || warm) {
            if (!XhciHubPortReset(ctrl, hub, p, warm, status)) {
                hub.hubError[i] = "port reset failed"; continue;
            }
            hub.hubPortStatus[i] = status;
            state = UsbHubDecodePort(status, hub.hubSuper);
        }
        if (!state.connected || !state.enabled) { hub.hubError[i] = "not enabled after reset"; continue; }
        XhciDelayMs(10); // reset recovery (USB 2.0 §7.1.7.5)
        XhciEnumResult result;
        const int child = XhciEnumerate(ctrl, static_cast<int>(index), p, state.speed, result);
        if (child >= 0) {
            hub.hubChild[i] = static_cast<uint8_t>(child + 1);
            hub.hubError[i] = nullptr; hub.hubTries[i] = 0; hub.hubCc[i] = 0;
        } else {
            hub.hubError[i] = result.stage; hub.hubCc[i] = result.cc;
            KPrintf("xhci: c%u hub slot %u port %u: %s failed cc=%u (try %u/3)\n", ctrl.index,
                    hub.slotId, p, result.stage, result.cc, hub.hubTries[i]);
        }
    }
    return changed;
}

// ---- Root ports ---------------------------------------------------------------

static constexpr uint32_t PORTSC_WRC = 1u << 19;
static constexpr uint32_t PORTSC_WPR = 1u << 31;
static constexpr uint32_t PORTSC_CHANGES = (1u << 17) | (1u << 18) | (1u << 19) | (1u << 20) |
                                           (1u << 21) | (1u << 22) | (1u << 23);

static uint32_t XhciPortRead(XhciController& ctrl, uint32_t port0)
{
    return xhci_read32(ctrl.opBase, XHCI_OP_PORTSC_BASE + port0 * 0x10);
}
static void XhciPortWrite(XhciController& ctrl, uint32_t port0, uint32_t value)
{
    xhci_write32(ctrl.opBase, XHCI_OP_PORTSC_BASE + port0 * 0x10, value);
}

static bool XhciWarmResetRootPort(XhciController& ctrl, uint32_t port0)
{
    const uint32_t before = XhciPortRead(ctrl, port0);
    if (before == 0xffffffffu) return false;
    XhciPortWrite(ctrl, port0, XhciPortPersistent(before) | PORTSC_WPR);
    for (uint32_t waited = 0; waited < 1000; waited += 5) {
        XhciDelayMs(5);
        const uint32_t state = XhciPortRead(ctrl, port0);
        if (state == 0xffffffffu) return false;
        if ((state & (PORTSC_WRC | PORTSC_PRC)) && !(state & PORTSC_PR)) {
            XhciPortWrite(ctrl, port0, XhciPortPersistent(state) | (state & (PORTSC_WRC | PORTSC_PRC)));
            const uint32_t after = XhciPortRead(ctrl, port0);
            return (after & (PORTSC_CCS | PORTSC_PED)) == (PORTSC_CCS | PORTSC_PED);
        }
    }
    return false;
}

static bool XhciPortIsUsb3(const XhciController& ctrl, uint32_t port0, uint32_t portsc)
{
    if (port0 < XHCI_MAX_ROOT_PORTS && ctrl.portMajor[port0]) return ctrl.portMajor[port0] >= 3;
    return ((portsc >> 10) & 15u) >= UsbSpeedSuper;
}

// One pass over the root ports. Returns true when the topology or a port's
// error changed. 'waiting' reports ports still training.
static bool XhciRootScan(XhciController& ctrl, bool initial, bool* waiting = nullptr)
{
    bool changed = false;
    const uint32_t ports = ctrl.maxPorts < XHCI_MAX_ROOT_PORTS ? ctrl.maxPorts : XHCI_MAX_ROOT_PORTS;
    for (uint32_t port0 = 0; port0 < ports; ++port0) {
        auto& st = ctrl.ports[port0];
        uint32_t v = XhciPortRead(ctrl, port0);
        if (v == 0xffffffffu) { st.error = "registers inaccessible"; continue; }
        const uint32_t changes = v & PORTSC_CHANGES;
        // Instrumentation: every root-port state transition (CCS, PED, PR,
        // PLS, PP, speed) and any change bits, once per transition.
        const uint32_t stateBits = v & (PORTSC_CCS | PORTSC_PED | PORTSC_PR | PORTSC_PLS_MASK | PORTSC_PP | PORTSC_SPEED_MASK);
        if (!initial && (stateBits != st.logged || changes))
            KPrintf("xhci: c%u port %u PORTSC %08x -> %08x (CCS=%u PED=%u PP=%u PLS=%s chg=%05x dev=%u tries=%u)\n",
                    ctrl.index, port0 + 1, st.portsc, v, v & 1u, (v >> 1) & 1u, (v >> 9) & 1u,
                    XhciLinkStateName((v >> 5) & 15u), changes >> 17, st.device, st.tries);
        st.logged = stateBits;
        if (changes) XhciPortWrite(ctrl, port0, XhciPortPersistent(v) | changes);
        st.portsc = v;
        // Port power must stay on: a port that lost PP (over-current, a
        // firmware hand-back or a controller quirk) can never see a connect,
        // and a bus-powered hub tree behind it stays dark.
        if (!initial && !(v & PORTSC_PP)) {
            XhciPortWrite(ctrl, port0, XhciPortPersistent(v) | PORTSC_PP);
            KPrintf("xhci: c%u port %u lost port power; PP restored\n", ctrl.index, port0 + 1);
            st.tries = 0; st.waits = 0; st.error = nullptr;
            continue; // re-read after the port powers up (next pass)
        }
        // A new physical connect always gets a fresh set of tries.
        if ((changes & PORTSC_CSC) && (v & PORTSC_CCS)) { st.tries = 0; st.waits = 0; st.error = nullptr; }
        const bool usb3 = XhciPortIsUsb3(ctrl, port0, v);
        if (st.device) {
            if ((v & PORTSC_CCS) && !(changes & PORTSC_CSC)) continue;
            XhciDeviceGone(st.device - 1u);
            st.device = 0; changed = true;
            if (!(v & PORTSC_CCS)) continue;
        }
        auto action = XhciRootPortAction(v, usb3);
        const uint32_t pls = (v >> 5) & 15u;
        if (action == XhciRootAction::Idle) {
            if (pls != 6 && pls != 10 && (st.error || st.tries || st.waits)) {
                st.error = nullptr; st.tries = 0; st.waits = 0; st.cc = 0; changed = true;
            }
            continue;
        }
        if (action == XhciRootAction::Wait) {
            if (++st.waits <= 8) { if (waiting) *waiting = true; continue; }
            action = usb3 ? XhciRootAction::WarmReset : XhciRootAction::Reset;
            st.waits = 0;
        }
        // Three tries, then retry every 5 s while something stays connected
        // (a KVM switch-back can bounce the connect through several failures).
        if (st.tries >= 3) {
            if (initial || ApicTickCount() - st.lastTryMs < 5000) continue;
            KPrintf("xhci: c%u port %u retrying enumeration after back-off (%s)\n", ctrl.index, port0 + 1,
                    st.error ? st.error : "?");
            st.tries = 0;
        }
        ++st.tries; changed = true; st.lastTryMs = ApicTickCount();
        if (action == XhciRootAction::Reset) {
            struct PortIo {
                XhciController& controller;uint32_t offset;
                uint32_t read(){return xhci_read32(controller.opBase,offset);}
                void write(uint32_t value){xhci_write32(controller.opBase,offset,value);}
                uint64_t now(){return ApicTickCount();}
                bool wait(){__asm__ volatile("pause");return true;}
            } io{ctrl,XHCI_OP_PORTSC_BASE+port0*0x10};
            if (!XhciResetRootPort(io, 1000)) { st.error = "port reset failed"; st.portsc = XhciPortRead(ctrl, port0); continue; }
        } else if (action == XhciRootAction::WarmReset) {
            if (!XhciWarmResetRootPort(ctrl, port0)) { st.error = "warm reset failed"; st.portsc = XhciPortRead(ctrl, port0); continue; }
        }
        if (action != XhciRootAction::Enumerate) XhciDelayMs(10); // reset recovery
        v = XhciPortRead(ctrl, port0);
        st.portsc = v;
        SerialPrintf("xhci: c%u port %u %s PORTSC=0x%08x\n", ctrl.index, port0 + 1,
                     action == XhciRootAction::Enumerate ? "enabled" : "after reset", v);
        if ((v & (PORTSC_CCS | PORTSC_PED)) != (PORTSC_CCS | PORTSC_PED)) {
            st.error = "not enabled after reset"; continue;
        }
        XhciEnumResult result;
        const int index = XhciEnumerate(ctrl, -1, static_cast<uint8_t>(port0 + 1), (v >> 10) & 15u, result);
        if (index >= 0) { st.device = static_cast<uint8_t>(index + 1); st.error = nullptr; st.tries = 0; st.cc = 0; }
        else {
            st.error = result.stage; st.cc = result.cc;
            KPrintf("xhci: c%u port %u: %s failed cc=%u (try %u/3)\n", ctrl.index, port0 + 1,
                    result.stage, result.cc, st.tries);
        }
    }
    return changed;
}

static void XhciInitialRootScan(XhciController& ctrl)
{
    // Firmware is no longer responsible for powering the root hub. Power all
    // ports first, then allow connection/debounce/link training to settle.
    const uint32_t ports = ctrl.maxPorts < XHCI_MAX_ROOT_PORTS ? ctrl.maxPorts : XHCI_MAX_ROOT_PORTS;
    KPrintf("xhci: c%u scanning %u root ports (%04x:%04x)\n", ctrl.index, ports,
            ctrl.pciDev.vendorId, ctrl.pciDev.deviceId);
    for (uint32_t port0 = 0; port0 < ports; ++port0) {
        const uint32_t state = XhciPortRead(ctrl, port0);
        SerialPrintf("xhci: c%u port %u before power PORTSC=0x%08x major=%u\n", ctrl.index, port0 + 1,
                     state, ctrl.portMajor[port0]);
        if (state == 0xffffffffu) { KPuts("xhci: root port registers inaccessible\n"); return; }
        if (!(state & PORTSC_PP)) {
            XhciPortWrite(ctrl, port0, XhciPortPersistent(state) | PORTSC_PP);
            (void)XhciPortRead(ctrl, port0); // flush posted MMIO write
        }
    }
    XhciDelayMs(250);
    // SuperSpeed links can still be training; give them up to ~1.5 s.
    for (uint32_t pass = 0; pass < 6; ++pass) {
        bool waiting = false;
        XhciRootScan(ctrl, true, &waiting);
        if (!waiting) break;
        XhciDelayMs(250);
    }
}

// ---- Status text ----------------------------------------------------------------

static void XhciStatusPrefix(XhciText& t, const XhciController& ctrl)
{
    t.s("USB xhci"); t.u(ctrl.index); t.c(' ');
}

static void XhciStatusDevice(XhciText& t, const XhciController& ctrl, uint32_t index)
{
    const auto& dev = g_devices[index];
    char path[24]; XhciPathText(dev, path, sizeof(path));
    XhciStatusPrefix(t, ctrl);
    for (uint32_t i = 0; i < dev.topo.depth; ++i) t.s("  ");
    t.c('p'); t.s(path); t.c(' '); t.s(UsbSpeedName(dev.portSpeed)); t.c(' ');
    t.s(XhciDeviceKind(dev)); t.c(' ');
    t.x(dev.vendorId, 4); t.c(':'); t.x(dev.productId, 4);
    t.s(" slot "); t.u(dev.slotId);
    if (dev.isHub) { t.c(' '); t.u(dev.hubPortCount); t.s(dev.hubSuper ? " ports SS" : " ports"); }
    if (dev.network) {
        t.s(" MAC ");
        for (uint32_t i = 0; i < 6; ++i) { if (i) t.c(':'); t.x(dev.network->interface.mac.b[i], 2); }
        if (dev.network->failed) t.s(" FAILED");
    }
    if (dev.isKeyboard) { t.s(dev.kbd.active ? " kbd=on" : " kbd=off"); if (dev.kbd.errorCc) { t.s(" kbd_cc="); t.u(dev.kbd.errorCc); } }
    if (dev.isMouse) t.s(dev.mouse.active ? " mouse=on" : " mouse=off");
    if (dev.note) { t.s(" note="); t.s(dev.note); }
    if (dev.gone) t.s(" GONE");
    t.nl();
    if (!dev.isHub || dev.gone) return;
    for (uint32_t p = 0; p < dev.hubPortCount; ++p) {
        if (dev.hubChild[p]) { XhciStatusDevice(t, ctrl, dev.hubChild[p] - 1u); continue; }
        const auto state = UsbHubDecodePort(dev.hubPortStatus[p], dev.hubSuper);
        if (!state.connected && !dev.hubError[p]) continue;
        XhciStatusPrefix(t, ctrl);
        for (uint32_t i = 0; i <= dev.topo.depth; ++i) t.s("  ");
        t.c('p'); t.s(path); t.c('.'); t.u(p + 1);
        t.s(state.connected ? " connected" : " empty"); t.s(state.enabled ? " enabled" : " disabled");
        t.s(" st="); t.x(dev.hubPortStatus[p], 4);
        if (dev.hubError[p]) { t.s(" err="); t.s(dev.hubError[p]); t.s(" cc="); t.u(dev.hubCc[p]);
                               t.s(" tries="); t.u(dev.hubTries[p]); }
        t.nl();
    }
}

static uint32_t XhciStatusText(char* out, uint32_t cap)
{
    XhciText t{out, cap, 0};
    uint32_t up = 0, devices = 0, hubs = 0, kbd = 0, mouse = 0, net = 0, storage = 0;
    for (uint32_t c = 0; c < g_controllerCount; ++c) {
        auto& ctrl = g_controllers[c];
        const auto& pci = ctrl.pciDev;
        XhciStatusPrefix(t, ctrl);
        t.x(pci.vendorId, 4); t.c(':'); t.x(pci.deviceId, 4); t.c(' ');
        t.x(pci.bus, 2); t.c(':'); t.x(pci.dev, 2); t.c('.'); t.u(pci.fn);
        if (ctrl.capBase) { t.s(" hci="); t.x(ctrl.hciVersion >> 8, 1); t.c('.'); t.x(ctrl.hciVersion & 0xff, 2); }
        t.s(" ports="); t.u(ctrl.maxPorts);
        t.s(" handoff="); t.s(ctrl.handoffText ? ctrl.handoffText : "-");
        t.s(" reset="); t.s(ctrl.resetText[0] ? ctrl.resetText : "-");
        t.s(" run="); t.s(ctrl.runText ? ctrl.runText : "-");
        if (ctrl.initialized) {
            t.s(" sts="); t.x(xhci_read32(ctrl.opBase, XHCI_OP_USBSTS), 8);
            t.s(" irq="); t.s(ctrl.irqText ? ctrl.irqText : "poll");
            t.s(ctrl.dma32 ? " dma=32" : " dma=64");
        }
        t.nl();
        if (!ctrl.initialized) {
            XhciStatusPrefix(t, ctrl); t.s("NOT DRIVEN: "); t.s(ctrl.failure ? ctrl.failure : "unknown"); t.nl();
            continue;
        }
        ++up;
        const uint32_t ports = ctrl.maxPorts < XHCI_MAX_ROOT_PORTS ? ctrl.maxPorts : XHCI_MAX_ROOT_PORTS;
        // Raw PORTSC of every root port, six per line.
        for (uint32_t port0 = 0; port0 < ports; ++port0) {
            if (port0 % 6 == 0) { if (port0) t.nl(); XhciStatusPrefix(t, ctrl); t.s("PORTSC"); }
            t.c(' '); t.u(port0 + 1); t.c(ctrl.portMajor[port0] >= 3 ? 's' : 'h'); t.c(':');
            t.x(XhciPortRead(ctrl, port0), 8);
        }
        if (ports) t.nl();
        for (uint32_t port0 = 0; port0 < ports; ++port0) {
            const auto& st = ctrl.ports[port0];
            const uint32_t v = XhciPortRead(ctrl, port0);
            if (st.device) { XhciStatusDevice(t, ctrl, st.device - 1u); continue; }
            if (!(v & PORTSC_CCS) && !st.error) continue;
            XhciStatusPrefix(t, ctrl);
            t.c('p'); t.u(port0 + 1); t.s(XhciPortIsUsb3(ctrl, port0, v) ? " usb3" : " usb2");
            t.s((v & PORTSC_CCS) ? " CCS" : " -"); t.s((v & PORTSC_PED) ? " PED" : " !PED");
            t.c(' '); t.s(XhciLinkStateName((v >> 5) & 15u));
            t.s(" speed="); t.u((v >> 10) & 15u);
            if (st.error) { t.s(" err="); t.s(st.error); t.s(" cc="); t.u(st.cc); t.s(" tries="); t.u(st.tries); }
            else if (st.waits) t.s(" waiting (link training)");
            t.nl();
        }
    }
    for (uint32_t i = 0; i < g_deviceCount; ++i) {
        const auto& dev = g_devices[i];
        if (dev.gone) continue;
        ++devices;
        hubs += dev.isHub; kbd += dev.kbd.active; mouse += dev.mouse.active;
        net += dev.network && !dev.network->failed; storage += dev.isMassStorage;
    }
    if (!g_controllerCount) { t.s("USB no xHCI controller found on PCI"); t.nl(); }
    t.s("USB summary: controllers="); t.u(g_controllerCount); t.s(" driven="); t.u(up);
    t.s(" devices="); t.u(devices); t.s(" hubs="); t.u(hubs); t.s(" kbd="); t.u(kbd);
    t.s(" mouse="); t.u(mouse); t.s(" net="); t.u(net); t.s(" storage="); t.u(storage);
    t.s(" keys="); t.u(__atomic_load_n(&g_keyPresses, __ATOMIC_RELAXED));
    t.nl();
    out[t.len] = 0;
    return t.len;
}

// ---- Hotplug worker ---------------------------------------------------------------

static void XhciUacHotplugTick();

static void XhciHotplugThread(void*)
{
    g_threadContext = true;
    uint32_t tick = 0;
    for (;;) {
        SchedulerSleepMs(500);
        ++tick;
        bool changed = false;
        for (uint32_t c = 0; c < g_controllerCount; ++c) {
            auto& ctrl = g_controllers[c];
            if (ctrl.initialized) changed |= XhciRootScan(ctrl, false);
        }
        // Hubs: GET_PORT_STATUS once a second (the status-change endpoint is not used).
        if (tick % 2 == 0) {
            const uint32_t count = __atomic_load_n(&g_deviceCount, __ATOMIC_ACQUIRE);
            for (uint32_t i = 0; i < count; ++i) {
                auto& dev = g_devices[i];
                if (dev.isHub && dev.hubReady && !dev.gone) changed |= XhciHubScan(*dev.ctrl, i, false);
            }
        }
        if (changed) UsbStatusChanged();
        XhciUacHotplugTick();
        XhciPollDrainEvents();
        XhciFidoCheckTimeouts();
        // Echo the first keyboard input on the console (bounded).
        static uint32_t reported = 0;
        const uint32_t keys = __atomic_load_n(&g_keyPresses, __ATOMIC_RELAXED);
        if (keys != reported && reported < 64) {
            char text[80]; XhciText t{text, sizeof(text), 0};
            uint32_t first = keys >= 8 ? keys - 8 : 0;
            if (reported > first) first = reported;
            for (uint32_t k = first; k < keys; ++k) {
                t.s(" 0x"); t.x(g_keyRecent[k % 16], 2);
            }
            text[t.len] = 0;
            KPrintf("xhci: USB keyboard input: %u key presses (scancodes%s)%s\n", keys, text,
                    keys >= 64 ? "; further key logging suppressed" : "");
            reported = keys;
        }
        static uint32_t mouseReported = 0;
        const uint32_t moves = __atomic_load_n(&g_mouseReports, __ATOMIC_RELAXED);
        if (moves != mouseReported && mouseReported < 16) {
            KPrintf("xhci: USB mouse input: %u reports%s\n", moves, moves >= 16 ? "; further mouse logging suppressed" : "");
            mouseReported = moves;
        }
    }
}

// ---------------------------------------------------------------------------
// Phase 8: Interrupt-driven event ring processing (ISR)
// ---------------------------------------------------------------------------

// Match a transfer event's TRB pointer to a device. The event ring's param
// field holds the physical address of the completed TRB. We check each
// device's interrupt and bulk transfer ring physical address ranges.
static int XhciMatchTransferEvent(const XhciController& ctrl, const Trb& evt)
{
    const uint64_t trbPhys = evt.param;
    const uint32_t ringBytes = XFER_RING_SIZE * sizeof(Trb);
    auto in = [&](uint64_t base) { return base && trbPhys >= base && trbPhys < base + ringBytes; };
    const uint32_t count = __atomic_load_n(&g_deviceCount, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < count; i++) {
        const XhciDevice& dev = g_devices[i];
        if (dev.ctrl != &ctrl) continue;
        // A quarantined UAS ring is identified by slot and endpoint, including
        // aborted events with no usable transfer TRB pointer.
        if(!dev.gone && dev.slotId==(evt.control>>24) &&
           (__atomic_load_n(&dev.mscDiscardDcis,__ATOMIC_ACQUIRE)&(1u<<((evt.control>>16)&31))))
            return static_cast<int>(i);
        if (in(dev.intRingPhys) || in(dev.bulkInRingPhys) || in(dev.bulkOutRingPhys) ||
            in(dev.ep0RingPhys) || in(dev.kbd.ringPhys) || in(dev.mouse.ringPhys) ||
            in(dev.fidoIn.ringPhys) || in(dev.fidoOut.ringPhys))
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

static void XhciLogPciState(const PciDevice& dev,const char* stage)
{
    const uint16_t command=PciConfigRead16(dev.bus,dev.dev,dev.fn,4);
    const uint16_t status=PciConfigRead16(dev.bus,dev.dev,dev.fn,6);
    bool valid=command!=0xffffu && status!=0xffffu;
    uint8_t cap=valid && (status&(1u<<4))?PciConfigRead8(dev.bus,dev.dev,dev.fn,0x34):0;
    uint16_t pmcsr=0;bool found=false;uint64_t visited=0;
    for(unsigned i=0;cap && i<48;++i) {
        if(cap<0x40 || cap>0xf8 || (cap&3) || (visited&(1ull<<((cap-0x40)/4)))) {valid=false;break;}
        visited|=1ull<<((cap-0x40)/4);
        const uint8_t id=PciConfigRead8(dev.bus,dev.dev,dev.fn,cap);
        if(id==0xff) {valid=false;break;}
        if(id==1) {
            pmcsr=PciConfigRead16(dev.bus,dev.dev,dev.fn,cap+4);found=true;
            if(pmcsr==0xffffu)valid=false;
            break;
        }
        cap=PciConfigRead8(dev.bus,dev.dev,dev.fn,cap+1);
    }
    if(cap && !found)valid=false;
    KPrintf("xhci: pci %s cmd=0x%x status=0x%x caps_valid=%u pm_present=%u pmcsr=0x%x\n",
            stage,command,status,valid?1u:0u,found?1u:0u,pmcsr);
}

static void XhciParseProtocols(XhciController& ctrl)
{
    // Supported Protocol capabilities (ID 2) map root ports to USB2/USB3.
    uint32_t offset=(xhci_read32(ctrl.capBase,XHCI_CAP_HCCPARAMS1)>>16)*4;
    for(unsigned hops=0;offset && offset<=ctrl.mapBytes-16 && !(offset&3) && hops<256;++hops) {
        const uint32_t header=xhci_read32(ctrl.capBase,offset);
        if(header==0xffffffffu)break;
        if((header&255)==2) {
            const uint32_t major=header>>24;
            const uint32_t ports=xhci_read32(ctrl.capBase,offset+8);
            const uint32_t first=ports&255,count=(ports>>8)&255;
            for(uint32_t p=first;p && p<first+count && p<=XHCI_MAX_ROOT_PORTS;++p)ctrl.portMajor[p-1]=static_cast<uint8_t>(major);
            SerialPrintf("xhci: c%u protocol USB%u ports %u-%u\n",ctrl.index,major,first,first+count-1);
        }
        const uint32_t next=((header>>8)&255)*4;
        if(!next)break;
        offset+=next;
    }
}

static bool XhciBringUp(XhciController& ctrl)
{
    if (!XhciPciDetect(ctrl, ctrl.pciDev)) {
        if (!ctrl.failure) ctrl.failure = "register discovery failed";
        return false;
    }
    XhciLogPciState(ctrl.pciDev,"before-handoff");
    struct OwnershipIo {
        XhciController& ctrl;uint64_t deadline;
        uint32_t read(uint32_t offset){return xhci_read32(ctrl.capBase,offset);}
        void write(uint32_t offset,uint32_t value){xhci_write32(ctrl.capBase,offset,value);}
        void writeByte(uint32_t offset,uint8_t value){*(ctrl.capBase+offset)=value;}
        bool wait(){__asm__ volatile("pause");return ApicTickCount()<deadline;}
    } ownership{ctrl,ApicTickCount()+1000};
    uint32_t legacy=0;
    const auto extended=(xhci_read32(ctrl.capBase,XHCI_CAP_HCCPARAMS1)>>16)*4;
    if(!XhciAcquireFirmwareOwnership(ownership,extended,ctrl.mapBytes,legacy)) {
        KPrintf("xhci: c%u firmware ownership handoff failed; controller untouched\n",ctrl.index);
        ctrl.handoffText="FAIL(BIOS owned)";ctrl.failure="firmware handoff failed; controller untouched";return false;
    }
    KPrintf("xhci: c%u firmware handoff complete legacy=0x%x\n",ctrl.index,legacy);
    ctrl.handoffText=legacy?"ok":"ok(no-legacy-cap)";
    if(legacy)KPrintf("xhci: ownership after handoff sem=0x%x ctl=0x%x\n",
                       ownership.read(legacy),ownership.read(legacy+4));
    XhciLogPciState(ctrl.pciDev,"after-handoff");
    if(ctrl.pciDev.vendorId==0x8086 && ctrl.pciDev.deviceId==0x9cb1) {
        const auto& dev=ctrl.pciDev;
        KPrintf("xhci: routing before reset d0=%08x d4=%08x d8=%08x dc=%08x\n",
                PciConfigRead32(dev.bus,dev.dev,dev.fn,0xd0),
                PciConfigRead32(dev.bus,dev.dev,dev.fn,0xd4),
                PciConfigRead32(dev.bus,dev.dev,dev.fn,0xd8),
                PciConfigRead32(dev.bus,dev.dev,dev.fn,0xdc));
    }
    // Reset only after firmware releases ownership and halt is observed.
    if (!XhciReset(ctrl)) {KPrintf("xhci: c%u controller reset failed; controller not driven\n",ctrl.index);ctrl.failure="controller reset failed";return false;}
    PciEnableBusMaster(ctrl.pciDev);

    if(ctrl.pciDev.vendorId==0x8086 && ctrl.pciDev.deviceId!=0x9cb1) {
        // Read-only diagnostics: 100/200-series PCHs have no EHCI routing,
        // but record the legacy routing words in case a board uses them.
        const auto& dev=ctrl.pciDev;
        SerialPrintf("xhci: c%u intel cfg d0=%08x d4=%08x d8=%08x dc=%08x\n",ctrl.index,
                PciConfigRead32(dev.bus,dev.dev,dev.fn,0xd0),PciConfigRead32(dev.bus,dev.dev,dev.fn,0xd4),
                PciConfigRead32(dev.bus,dev.dev,dev.fn,0xd8),PciConfigRead32(dev.bus,dev.dev,dev.fn,0xdc));
    }

    if (!XhciAllocRings(ctrl)) {KPrintf("xhci: c%u ring allocation failed\n",ctrl.index);ctrl.failure="ring allocation failed";return false;}
    if (!XhciStartController(ctrl)) {KPrintf("xhci: c%u controller start failed\n",ctrl.index);ctrl.failure="controller start failed (HCH)";return false;}
    XhciParseProtocols(ctrl);
    __atomic_store_n(&ctrl.initialized,true,__ATOMIC_RELEASE);

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
    return true;
}

static void XhciSetupInterrupts(XhciController& ctrl)
{
    // Clear any stale interrupt state from polling phase.  The controller
    // won't generate new MSI-X messages while IMAN.IP or USBSTS.EINT are set.
    {
        uint32_t sts = xhci_read32(ctrl.opBase, XHCI_OP_USBSTS);
        if (sts & USBSTS_EINT)
            xhci_write32(ctrl.opBase, XHCI_OP_USBSTS, USBSTS_EINT); // W1C

        uint32_t iman = xhci_read32(ctrl.rtBase, XHCI_RT_IMAN);
        xhci_write32(ctrl.rtBase, XHCI_RT_IMAN, iman | 0x3); // W1C IP, keep IE

        // Write ERDP with EHB=1 to clear Event Handler Busy
        const auto saved=IrqSpinLockAcquire(&g_eventRingLock);
        uint64_t erdpPhys = ctrl.evtRingPhys + ctrl.evtDequeue * sizeof(Trb);
        xhci_write64(ctrl.rtBase, XHCI_RT_ERDP, erdpPhys | (1ULL << 3));
        IrqSpinLockRelease(&g_eventRingLock,saved);
    }

    // Set up MSI-X for interrupt delivery.  QEMU's qemu-xhci uses MSI-X and
    // does not support legacy INTx fallback, so we must program the MSI-X
    // table directly.  We configure a single entry (interrupter 0) to deliver
    // our chosen vector to the BSP's LAPIC. Every controller shares the
    // vector; the handler drains all of them.
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

    ctrl.irqText = msixConfigured ? "msix" : msiConfigured ? "msi" : ctrl.irqVector ? "intx" : "poll";
    if (msixConfigured || msiConfigured || ctrl.irqVector) g_irqActive = true;
    SerialPrintf("xhci: interrupt-driven mode active (MSI-X=%s)\n",
                 msixConfigured ? "yes" : "no");
}


// Pre-reset quiesce (Linux xhci_shutdown + pci_disable_device): clear Run/Stop
// and interrupt enable, wait a bounded time for HCHalted, then drop bus
// mastering and MSI/MSI-X. Never blocks the reset: at most ~20 ms/controller.
static void XhciQuiesceForReset()
{
    const uint32_t count = __atomic_load_n(&g_controllerCount, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < count; ++i) {
        auto& ctrl = g_controllers[i];
        if (!ctrl.initialized || !ctrl.opBase) continue;
        const uint32_t cmd = xhci_read32(ctrl.opBase, XHCI_OP_USBCMD);
        if (cmd != 0xffffffffu) {
            xhci_write32(ctrl.opBase, XHCI_OP_USBCMD, cmd & ~(USBCMD_RS | USBCMD_INTE | USBCMD_HSEE));
            const uint64_t start = ApicTickCount();
            for (uint32_t spin = 0; spin < 200000u; ++spin) {
                const uint32_t sts = xhci_read32(ctrl.opBase, XHCI_OP_USBSTS);
                if (sts == 0xffffffffu || (sts & USBSTS_HCH)) break;
                if (ApicTickCount() - start > 20) break;
                __asm__ volatile("pause");
            }
        }
        (void)PciQuiesceFunctionForReset(ctrl.pciDev.bus, ctrl.pciDev.dev, ctrl.pciDev.fn);
    }
}

// ---- Read-only diagnostics: bridge hardware usb ------------------------------
// "list", "desc <i> [offset]" (raw device + configuration descriptors as hex),
// "get <i> <bmRequestType> <bRequest> <wValue> <wIndex> <wLength>" (hex fields;
// device-to-host requests only, and only on devices no Brook function owns).
static bool XhciArgHex(const char*& p, uint32_t& v)
{
    while (*p == ' ') ++p;
    uint32_t n = 0; v = 0;
    while (true) {
        const char ch = *p; uint32_t d;
        if (ch >= '0' && ch <= '9') d = uint32_t(ch - '0');
        else if (ch >= 'a' && ch <= 'f') d = uint32_t(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') d = uint32_t(ch - 'A' + 10);
        else break;
        if (++n > 8) return false;
        v = (v << 4) | d; ++p;
    }
    return n != 0;
}
static bool XhciArgIs(const char*& p, const char* word)
{
    const char* q = p;
    while (*word) if (*q++ != *word++) return false;
    if (*q && *q != ' ') return false;
    p = q; return true;
}
struct XhciText;
static uint32_t XhciUacCommand(const char* p, XhciText& t);
static uint32_t XhciUsbCommand(const char* args, char* out, uint32_t cap)
{
    XhciText t{out, cap, 0};
    const char* p = args;
    const uint32_t count = __atomic_load_n(&g_deviceCount, __ATOMIC_ACQUIRE);
    if (!*p || XhciArgIs(p, "list")) {
        for (uint32_t i = 0; i < count; ++i) {
            const auto& dev = g_devices[i];
            char path[24]; XhciPathText(dev, path, sizeof(path));
            t.u(i); t.s(" c"); t.u(dev.ctrl ? dev.ctrl->index : 0); t.s(" p"); t.s(path);
            t.c(' '); t.s(UsbSpeedName(dev.portSpeed)); t.c(' ');
            t.x(dev.vendorId, 4); t.c(':'); t.x(dev.productId, 4); t.c(' '); t.s(XhciDeviceKind(dev));
            t.s(" slot="); t.u(dev.slotId); t.s(" cfg="); t.u(dev.rawConfigLen);
            if (dev.gone) t.s(" gone");
            t.nl();
        }
        return t.len;
    }
    if (XhciArgIs(p, "audio")) return XhciUacCommand(p, t);
    uint32_t index = 0;
    if (XhciArgIs(p, "desc")) {
        uint32_t offset = 0;
        if (!XhciArgHex(p, index) || index >= count) { t.s("bad device index"); return t.len; }
        if (*p) (void)XhciArgHex(p, offset);
        const auto& dev = g_devices[index];
        t.s("dev "); for (uint8_t b : dev.rawDevice) t.x(b, 2); t.nl();
        t.s("cfg len="); t.u(dev.rawConfigLen); t.s(" off="); t.x(offset, 4); t.nl();
        for (uint32_t o = offset; o < dev.rawConfigLen && o < offset + 1024; ++o) {
            t.x(dev.rawConfig[o], 2);
            if ((o - offset) % 32 == 31) t.nl();
        }
        t.nl();
        return t.len;
    }
    if (XhciArgIs(p, "get")) {
        uint32_t f[5];
        if (!XhciArgHex(p, index) || index >= count) { t.s("bad device index"); return t.len; }
        for (auto& v : f) if (!XhciArgHex(p, v)) { t.s("usage: get <i> <bmRT> <bReq> <wValue> <wIndex> <wLength>"); return t.len; }
        auto& dev = g_devices[index];
        if (!(f[0] & 0x80) || f[0] > 0xff || f[1] > 0xff || f[2] > 0xffff || f[3] > 0xffff || !f[4] || f[4] > 512) {
            t.s("refused: device-to-host requests of 1..512 bytes only"); return t.len;
        }
        if (dev.gone || !dev.ctrl || !dev.scratch || dev.isHub || dev.isKeyboard || dev.isMouse ||
            dev.isMassStorage || dev.ecm.value) { t.s("refused: device gone or owned by a driver"); return t.len; }
        UsbSetupPacket setup{};
        setup.bmRequestType = uint8_t(f[0]); setup.bRequest = uint8_t(f[1]);
        setup.wValue = uint16_t(f[2]); setup.wIndex = uint16_t(f[3]); setup.wLength = uint16_t(f[4]);
        uint32_t got = 0;
        memset(dev.scratch, 0, f[4]);
        if (!XhciControlTransfer(*dev.ctrl, dev, setup, dev.scratch, f[4], dev.scratchPhys, 3, &got)) {
            t.s("failed cc="); t.u(dev.lastCc); return t.len;
        }
        t.s("ok len="); t.u(got); t.nl();
        for (uint32_t o = 0; o < got; ++o) { t.x(dev.scratch[o], 2); if (o % 32 == 31) t.nl(); }
        t.nl();
        return t.len;
    }
    t.s("usage: bridge hardware usb [list|desc <i> [off]|get <i> <bmRT> <bReq> <wValue> <wIndex> <wLength>]");
    return t.len;
}


// ---- UAC2 playback (EVO4-class devices): bridge hardware audio ---------------
// Started only by an explicit "bridge hardware audio usb start". Streams 48 kHz
// 16-bit silence through the isochronous OUT endpoint with explicit feedback;
// "tone <dB> audible" raises the feature-unit volume for one 1 s 440 Hz tone and
// then returns it to the device minimum. Capture and DFU interfaces are never
// touched.
struct XhciUacStream {
    XhciController* ctrl = nullptr;
    uint32_t device = 0, slotId = 0, dataDci = 0, fbDci = 0, frameBytes = 0;
    Uac2Playback desc{};
    Trb* dataRing = nullptr; uint64_t dataRingPhys = 0; uint32_t dataEnq = 0; bool dataCycle = true;
    Trb* fbRing = nullptr; uint64_t fbRingPhys = 0; uint32_t fbEnq = 0; bool fbCycle = true;
    uint8_t* dataBuf = nullptr; uint64_t dataBufPhys = 0;
    uint8_t* fbBuf = nullptr; uint64_t fbBufPhys = 0;
    uint64_t dataSeq[XFER_RING_SIZE]{}, fbSeq[XFER_RING_SIZE]{};
    volatile uint64_t completedHw = 0; // only from transfer events: bounds ring occupancy
    volatile uint64_t produced = 0, completed = 0, fbProduced = 0, fbCompleted = 0, framesSent = 0;
    volatile uint32_t dataEvents = 0, fbEvents = 0, underruns = 0, missed = 0, resyncs = 0, fbValues = 0;
    volatile uint32_t lastFbRaw = 0, lastFbBytes = 0, lastBadCc = 0;
    volatile uint64_t underrunTick[4]{};
    volatile uint64_t maxLoopGap = 0, lastLoop = 0;
    volatile uint32_t ccCount[32]{};
    volatile bool active = false, running = false, stopRequested = false, resync = false;
    volatile bool toneRequested = false, toneDone = false;
    volatile uint64_t toneStartTick = 0, toneEndTick = 0;
    Uac2FeedbackPacer pacer{};
    Uac2Tone tone{};
    int16_t volumeMin = 0, volumeMax = 0;
    bool volumeControl = false;
    uint32_t volumeSkip = 0;            // channels whose SET_CUR stalls (best-effort)
    // Feature-unit level: owned by the stream thread once it runs.
    volatile bool fuOverride = false;   // explicit command level (tone, mixer-tone)
    volatile int16_t fuOverrideQ8 = 0;
    volatile int16_t unmutedQ8 = -20 * 256; // mixer mode, after an explicit user unmute
    volatile int16_t fuCurrentQ8 = 0;
    volatile bool fuKnown = false;
    // Mixer mode: the kernel mixer (44.1 kHz stereo S16) feeds channels 1-2.
    volatile bool mixerMode = false, primed = false, flushRequested = false;
    uint32_t primeWait = 0;
    // Native clocked contract (AudioDriver trySubmit/progress/restart): byte
    // watermarks of 44.1 kHz stereo S16 in this epoch; completion is frames
    // the stream has taken from the ring into isochronous packets.
    volatile uint64_t nativeEpoch = 1, nativeSubmitted = 0, nativeOutBase = 0;
    volatile bool nativeDiscontinuity = false;
    volatile uint32_t mixWrite = 0, mixRead = 0; // frame counters (SPSC)
    volatile uint64_t mixFramesIn = 0, mixFramesOut = 0, mixUnderflows = 0, mixDropped = 0;
    uint32_t rate = 0, rateReadback = 0;
};
static XhciUacStream g_uac;
static bool XhciUacOwnsDevice(uint32_t index) { return g_uac.device == index && (g_uac.active || g_uac.running); }
static constexpr uint32_t UAC_MIX_FRAMES = 8192, UAC_MIX_PRIME = 441; // 186 ms ring, 10 ms prime
static int16_t g_uacMix[UAC_MIX_FRAMES * 2];
static bool g_uacAudioRegistered = false;
static bool XhciUacSetVolume(XhciDevice& dev, int16_t q8);
static constexpr uint32_t UAC_DATA_SLOT = 128, UAC_DATA_LEAD = 224, UAC_FB_LEAD = 16, UAC_IOC_EVERY = 8;

static bool XhciUacEventHit(const XhciController& ctrl, const Trb& event)
{
    if (!__atomic_load_n(&g_uac.active, __ATOMIC_ACQUIRE) || &ctrl != g_uac.ctrl) return false;
    const uint32_t slot = event.control >> 24, dci = (event.control >> 16) & 31;
    return slot == g_uac.slotId && (dci == g_uac.dataDci || dci == g_uac.fbDci);
}

static uint64_t XhciUacSeqFor(const Trb* ring, uint64_t ringPhys, const uint64_t* seq, uint64_t param, bool& ok)
{
    (void)ring;
    ok = param >= ringPhys && param < ringPhys + (XFER_RING_SIZE - 1) * sizeof(Trb) && !((param - ringPhys) % sizeof(Trb));
    return ok ? seq[(param - ringPhys) / sizeof(Trb)] : 0;
}

// Event ring context (IRQ or poll, g_eventRingLock held): bookkeeping only.
static void XhciUacEvent(const Trb& event)
{
    auto& s = g_uac;
    const uint32_t cc = event.status >> 24, dci = (event.control >> 16) & 31;
    bool ok = false;
    if (dci == s.dataDci) {
        __atomic_fetch_add(&s.dataEvents, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s.ccCount[cc & 31], 1, __ATOMIC_RELAXED);
        if (cc == 14 || cc == 15) {
            const uint32_t n = __atomic_fetch_add(&s.underruns, 1, __ATOMIC_RELAXED);
            s.underrunTick[n & 3] = ApicTickCount(); s.resync = true; return;
        }
        if (cc == 23) __atomic_fetch_add(&s.missed, 1, __ATOMIC_RELAXED);
        else if (cc != TRB_CC_SUCCESS && cc != TRB_CC_SHORT_PACKET) s.lastBadCc = cc;
        const uint64_t seq = XhciUacSeqFor(s.dataRing, s.dataRingPhys, s.dataSeq, event.param, ok);
        if (ok && seq + 1 > s.completedHw) __atomic_store_n(&s.completedHw, seq + 1, __ATOMIC_RELEASE);
        if (ok && seq + 1 > s.completed) __atomic_store_n(&s.completed, seq + 1, __ATOMIC_RELEASE);
        return;
    }
    __atomic_fetch_add(&s.fbEvents, 1, __ATOMIC_RELAXED);
    const uint64_t seq = XhciUacSeqFor(s.fbRing, s.fbRingPhys, s.fbSeq, event.param, ok);
    if (!ok) return;
    if (seq + 1 > s.fbCompleted) __atomic_store_n(&s.fbCompleted, seq + 1, __ATOMIC_RELEASE);
    if (cc != TRB_CC_SUCCESS && cc != TRB_CC_SHORT_PACKET) { s.lastBadCc = cc; return; }
    const uint32_t residue = event.status & 0xffffff;
    const uint32_t bytes = residue <= 4 ? 4 - residue : 0;
    const uint8_t* p = s.fbBuf + ((event.param - s.fbRingPhys) / sizeof(Trb)) * 16;
    if (bytes >= 3) {
        s.lastFbRaw = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | (bytes > 3 ? uint32_t(p[3]) << 24 : 0);
        s.lastFbBytes = bytes;
        __atomic_fetch_add(&s.fbValues, 1, __ATOMIC_RELEASE);
    }
}

// Producer side (stream thread only). One TRB per TD; Link TRBs wrap the ring.
static Trb& XhciUacSlot(Trb* ring, uint32_t& enq, bool& cycle, uint32_t& index)
{
    if (enq >= XFER_RING_SIZE - 1) {
        ring[XFER_RING_SIZE - 1].control = TRB_TYPE_LINK | TRB_TC | (cycle ? TRB_CYCLE : 0);
        cycle = !cycle; enq = 0;
    }
    index = enq++;
    return ring[index];
}

static void XhciUacQueueData()
{
    auto& s = g_uac;
    uint32_t index = 0;
    Trb& trb = XhciUacSlot(s.dataRing, s.dataEnq, s.dataCycle, index);
    const uint32_t frames = s.pacer.Next();
    uint8_t* out = s.dataBuf + index * UAC_DATA_SLOT;
    if (s.flushRequested) { s.mixRead = s.mixWrite; s.primed = false; s.flushRequested = false; }
    for (uint32_t f = 0; f < frames; ++f) {
        int32_t left = 0, right = 0;
        if (s.mixerMode) {
            const uint32_t level = __atomic_load_n(&s.mixWrite, __ATOMIC_ACQUIRE) - s.mixRead;
            if (!level) s.primeWait = 0;
            else if (!s.primed && ++s.primeWait >= 160 * 6) s.primed = true; // ~20 ms of frames waiting
            if (!s.primed && level >= UAC_MIX_PRIME) s.primed = true;
            if (s.primed && level) {
                const uint32_t at = (s.mixRead % UAC_MIX_FRAMES) * 2;
                left = g_uacMix[at]; right = g_uacMix[at + 1];
                __atomic_store_n(&s.mixRead, s.mixRead + 1, __ATOMIC_RELEASE);
                ++s.mixFramesOut;
            } else if (s.primed) { s.primed = false; ++s.mixUnderflows; }
        }
        if (s.tone.Active()) { const int16_t v = s.tone.NextSample(); left += v; right += v; }
        if (left > 32767) left = 32767; else if (left < -32768) left = -32768;
        if (right > 32767) right = 32767; else if (right < -32768) right = -32768;
        for (uint32_t c = 0; c < s.desc.channels; ++c) {
            const int16_t sample = int16_t(c == 0 ? left : c == 1 ? right : 0);
            uint8_t* q = out + f * s.frameBytes + c * s.desc.subslot;
            q[0] = uint8_t(sample); q[1] = uint8_t(uint16_t(sample) >> 8);
        }
    }
    const uint64_t seq = s.produced;
    s.dataSeq[index] = seq;
    trb.param = s.dataBufPhys + index * UAC_DATA_SLOT;
    trb.status = XhciIsochStatus(frames * s.frameBytes);
    __atomic_store_n(&trb.control, XhciIsochControl(s.dataCycle, seq % UAC_IOC_EVERY == UAC_IOC_EVERY - 1, true),
                     __ATOMIC_RELEASE);
    s.framesSent += frames;
    __atomic_store_n(&s.produced, seq + 1, __ATOMIC_RELEASE);
}

static void XhciUacQueueFeedback()
{
    auto& s = g_uac;
    uint32_t index = 0;
    Trb& trb = XhciUacSlot(s.fbRing, s.fbEnq, s.fbCycle, index);
    const uint64_t seq = s.fbProduced;
    s.fbSeq[index] = seq;
    trb.param = s.fbBufPhys + index * 16;
    trb.status = XhciIsochStatus(s.desc.feedbackMaxPacket & 0x7ff);
    __atomic_store_n(&trb.control, XhciIsochControl(s.fbCycle, true, true), __ATOMIC_RELEASE);
    __atomic_store_n(&s.fbProduced, seq + 1, __ATOMIC_RELEASE);
}

static void XhciUacThread(void*)
{
    auto& s = g_uac;
    uint32_t seenFb = 0;
    s.running = true;
    while (!s.stopRequested && !g_devices[s.device].gone) {
        if (s.resync) {
            s.resync = false; ++s.resyncs;
            __atomic_store_n(&s.completed, s.produced, __ATOMIC_RELEASE);
        }
        const uint32_t values = __atomic_load_n(&s.fbValues, __ATOMIC_ACQUIRE);
        if (values != seenFb) { seenFb = values; (void)s.pacer.Feed(s.lastFbRaw, s.lastFbBytes); }
        if (s.toneRequested && !s.tone.Active() && !s.toneStartTick) {
            s.tone.Start(440, s.rate, s.rate, s.rate / 100, 8192); // 1 s, 10 ms fades, -12 dBFS
            s.toneStartTick = ApicTickCount();
        }
        bool fb = false, data = false;
        while (s.fbProduced - __atomic_load_n(&s.fbCompleted, __ATOMIC_ACQUIRE) < UAC_FB_LEAD) { XhciUacQueueFeedback(); fb = true; }
        // A resync after an underrun trusts the ring is empty; the event-only
        // bound keeps a stale underrun report from overrunning queued TDs.
        while (s.produced - __atomic_load_n(&s.completed, __ATOMIC_ACQUIRE) < UAC_DATA_LEAD &&
               s.produced - __atomic_load_n(&s.completedHw, __ATOMIC_ACQUIRE) < XFER_RING_SIZE - 16) { XhciUacQueueData(); data = true; }
        __asm__ volatile("mfence" ::: "memory");
        if (fb) XhciRingDoorbell(*s.ctrl, s.slotId, s.fbDci);
        if (data) XhciRingDoorbell(*s.ctrl, s.slotId, s.dataDci);
        if (s.toneStartTick && !s.toneEndTick && !s.tone.Active()) { s.toneEndTick = ApicTickCount(); s.toneDone = true; }
        const uint64_t now = ApicTickCount();
        if (s.lastLoop && now - s.lastLoop > s.maxLoopGap) s.maxLoopGap = now - s.lastLoop;
        s.lastLoop = now;
        SchedulerSleepMs(1);
    }
    s.running = false;
}

// Feature-unit level, on its own worker so a slow or failing control
// endpoint can never stall the isochronous refill: explicit override, else
// (mixer mode) the user's explicit unmute of an unmuted mixer, else minimum.
// Failures back off from 200 ms to 5 s.
static volatile bool g_uacControlRunning = false;
static void XhciUacControlThread(void*)
{
    auto& s = g_uac;
    uint64_t retryAt = 0; uint32_t backoff = 200;
    while (s.active && !s.stopRequested && !g_devices[s.device].gone) {
        const uint32_t control = AudioOutputControlState();
        const bool userUnmuted = (control & AudioUserUnmuteBit) && !(control & BROOK_AUDIO_MUTED);
        const int16_t want = s.fuOverride ? s.fuOverrideQ8 : (s.mixerMode && userUnmuted) ? s.unmutedQ8 : s.volumeMin;
        const uint64_t now = ApicTickCount();
        if (s.volumeControl && (!s.fuKnown || want != s.fuCurrentQ8) && now >= retryAt) {
            auto& dev = g_devices[s.device];
            const bool ok = XhciUacSetVolume(dev, want);
            s.fuCurrentQ8 = want; s.fuKnown = ok;
            KPrintf("uac: EVO4 volume %d/256 dB at tick %u (%s%s)\n", int(want), unsigned(now),
                    s.fuOverride ? "command" : want != s.volumeMin ? "user unmute" : "minimum", ok ? "" : ", FAILED");
            if (ok) backoff = 200;
            else { retryAt = now + backoff; if (backoff < 5000) backoff *= 2; }
        }
        SchedulerSleepMs(20);
    }
    g_uacControlRunning = false;
}

static bool XhciUacControl(XhciDevice& dev, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                           const void* out, uint32_t length, void* in = nullptr)
{
    UsbSetupPacket setup{};
    setup.bmRequestType = type; setup.bRequest = request; setup.wValue = value; setup.wIndex = index;
    setup.wLength = uint16_t(length);
    if (length && out) memcpy(dev.scratch, out, length);
    const bool ok = XhciControlTransfer(*dev.ctrl, dev, setup, length ? dev.scratch : nullptr, length,
                                        length ? dev.scratchPhys : 0, length ? ((type & 0x80) ? 3 : 2) : 0);
    if (ok && in && length) memcpy(in, dev.scratch, length);
    return ok;
}

// Returns a mask of channels whose SET_CUR failed. Channels 1 and 2 carry the
// stream (the others stay silent) and must succeed; the rest are best-effort.
static volatile uint32_t g_uacVolumeFailMask = 0;
static volatile uint8_t g_uacVolumeFailCc = 0;
static bool XhciUacSetVolume(XhciDevice& dev, int16_t q8)
{
    auto& s = g_uac;
    uint32_t failed = 0;
    for (uint32_t ch = 0; ch <= s.desc.featureChannels && ch < 32; ++ch) {
        if (!(s.desc.featureVolumeMask & (1u << ch)) || (s.volumeSkip & (1u << ch))) continue;
        if (!XhciUacControl(dev, 0x21, 1, uint16_t(0x0200 | ch), uint16_t(s.desc.featureId << 8 | s.desc.controlInterface),
                            &q8, 2)) { failed |= 1u << ch; g_uacVolumeFailCc = dev.lastCc; }
    }
    g_uacVolumeFailMask = failed;
    if (failed) KPrintf("uac: volume %d/256 dB SET_CUR failed on channel mask %x (cc %u)\n", int(q8), failed,
                        unsigned(g_uacVolumeFailCc));
    return !(failed & 0x6u);
}

// Configure Endpoint adding (or dropping) the data and feedback endpoints.
static uint32_t XhciUacConfigureEndpoints(XhciController& ctrl, XhciDevice& dev, bool add)
{
    auto& s = g_uac;
    const uint32_t ctxSize = CtxEntrySize(ctrl);
    auto* input = static_cast<uint8_t*>(dev.inputCtx);
    memset(input, 0, ctxSize * 33);
    auto* icc = reinterpret_cast<uint32_t*>(input);
    const uint32_t flags = (1u << s.dataDci) | (1u << s.fbDci);
    if (add) icc[1] = 1u | flags; else { icc[0] = flags; icc[1] = 1u; }
    auto* slotCtx = reinterpret_cast<uint32_t*>(input + ctxSize);
    const auto* outSlot = reinterpret_cast<const uint32_t*>(dev.outputCtx);
    for (int i = 0; i < 4; ++i) slotCtx[i] = outSlot[i];
    const uint32_t maxDci = s.dataDci > s.fbDci ? s.dataDci : s.fbDci;
    if (add && maxDci > ((slotCtx[0] >> 27) & 31)) slotCtx[0] = (slotCtx[0] & ~(0x1Fu << 27)) | (maxDci << 27);
    if (add) {
        struct { uint32_t dci; bool in; uint8_t interval; uint16_t packet; uint32_t average; uint64_t ring; } eps[2] = {
            {s.dataDci, false, s.desc.dataInterval, s.desc.dataMaxPacket, 6u * s.frameBytes, s.dataRingPhys},
            {s.fbDci, true, s.desc.feedbackInterval, s.desc.feedbackMaxPacket, 4u, s.fbRingPhys}};
        for (const auto& e : eps) {
            auto* ep = reinterpret_cast<uint32_t*>(input + ctxSize * (e.dci + 1));
            const auto words = XhciIsochContext(e.in, e.interval, e.packet, e.average);
            ep[0] = words.dw0; ep[1] = words.dw1;
            ep[2] = uint32_t(e.ring | 1); ep[3] = uint32_t(e.ring >> 32); ep[4] = words.dw4;
        }
    }
    __asm__ volatile("mfence" ::: "memory");
    return XhciSlotCommand(ctrl, dev.inputCtxPhys, TRB_TYPE_CONFIG_EP | (dev.slotId << 24), 2000);
}

static uint32_t XhciUacEpState(uint32_t dci)
{
    auto& dev = g_devices[g_uac.device];
    if (!g_uac.ctrl || !dev.outputCtx) return 0;
    const auto* ep = reinterpret_cast<const volatile uint32_t*>(static_cast<uint8_t*>(dev.outputCtx) +
                                                               CtxEntrySize(*g_uac.ctrl) * dci);
    return ep[0] & 7;
}

static void XhciUacStatus(XhciText& t)
{
    auto& s = g_uac;
    t.s("uac active="); t.u(s.active); t.s(" running="); t.u(s.running);
    if (!s.ctrl) { t.nl(); return; }
    t.s(" dev="); t.u(s.device); t.s(" slot="); t.u(s.slotId); t.s(" if="); t.u(s.desc.streamInterface);
    t.s(" alt="); t.u(s.desc.alternate); t.s(" rate="); t.u(s.rate); t.s(" readback="); t.u(s.rateReadback);
    t.s(" ep_state data="); t.u(XhciUacEpState(s.dataDci)); t.s(" fb="); t.u(XhciUacEpState(s.fbDci)); t.nl();
    t.s("data produced="); t.u(s.produced); t.s(" completed="); t.u(s.completed); t.s(" events="); t.u(s.dataEvents);
    t.s(" frames="); t.u(s.framesSent); t.s(" underruns="); t.u(s.underruns); t.s(" missed="); t.u(s.missed);
    t.s(" resyncs="); t.u(s.resyncs); t.s(" max_loop_gap_ms="); t.u(s.maxLoopGap); t.s(" underrun_ticks=");
    for (uint32_t i = 0; i < 4; ++i) { if (i) t.c(','); t.u(s.underrunTick[i]); } t.s(" last_bad_cc="); t.u(s.lastBadCc); t.s(" cc:");
    for (uint32_t i = 0; i < 32; ++i) if (s.ccCount[i]) { t.c(' '); t.u(i); t.c('='); t.u(s.ccCount[i]); }
    t.nl();
    t.s("feedback produced="); t.u(s.fbProduced); t.s(" completed="); t.u(s.fbCompleted); t.s(" events="); t.u(s.fbEvents);
    t.s(" values="); t.u(s.fbValues); t.s(" raw="); t.x(s.lastFbRaw, 8); t.s(" bytes="); t.u(s.lastFbBytes);
    t.s(" pacer="); t.x(s.pacer.current, 8); t.s(" nominal="); t.x(s.pacer.nominal, 8);
    t.s(" accepted="); t.u(s.pacer.accepted); t.s(" rejected="); t.u(s.pacer.rejected); t.nl();
    t.s("mixer mode="); t.u(s.mixerMode); t.s(" primed="); t.u(s.primed); t.s(" level=");
    t.u(s.mixWrite - s.mixRead); t.s(" in="); t.u(s.mixFramesIn); t.s(" out="); t.u(s.mixFramesOut);
    t.s(" underflows="); t.u(s.mixUnderflows); t.s(" dropped="); t.u(s.mixDropped);
    t.s(" registered="); t.u(g_uacAudioRegistered); t.s(" native epoch="); t.u(s.nativeEpoch);
    t.s(" submitted="); t.u(s.nativeSubmitted); t.s(" discontinuity="); t.u(s.nativeDiscontinuity);
    t.s(" clock_ns="); t.u(AudioClockNowNs()); t.nl();
    t.s("fu current="); t.u(uint16_t(s.fuCurrentQ8)); t.s(" known="); t.u(s.fuKnown); t.s(" override=");
    t.u(s.fuOverride); t.s(" unmuted_level="); t.u(uint16_t(s.unmutedQ8)); t.s(" skip="); t.x(s.volumeSkip, 2);
    t.s(" mixer_control="); t.x(AudioOutputControlState(), 5); t.nl();
    t.s("volume fail_mask="); t.x(g_uacVolumeFailMask, 2); t.s(" fail_cc="); t.u(g_uacVolumeFailCc);
    t.s(" min="); t.u(uint16_t(s.volumeMin)); t.s(" max="); t.u(uint16_t(s.volumeMax)); t.nl();
    t.s("tone start_tick="); t.u(s.toneStartTick); t.s(" end_tick="); t.u(s.toneEndTick);
    t.s(" now="); t.u(ApicTickCount()); t.nl();
}

static bool XhciUacFindDevice(uint32_t& index)
{
    const uint32_t count = __atomic_load_n(&g_deviceCount, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& d = g_devices[i];
        if (!d.gone && d.ctrl && d.rawConfig && d.scratch && !d.isHub && !d.isKeyboard && !d.isMouse &&
            !d.isMassStorage && !d.ecm.value && d.vendorId == 0x2708 && d.productId == 0x0006) { index = i; return true; }
    }
    return false;
}

static void XhciUacStart(XhciText& t, uint32_t rate, bool mixer)
{
    auto& s = g_uac;
    if (s.active) { t.s("already active\n"); return; }
    uint32_t index = 0;
    if (!XhciUacFindDevice(index)) { t.s("no EVO4 (2708:0006) present\n"); return; }
    auto& dev = g_devices[index];
    Uac2Playback desc{};
    if (!Uac2ParsePlayback(dev.rawConfig, dev.rawConfigLen, 16, desc)) { t.s("no 16-bit UAC2 playback alternate\n"); return; }
    if (!s.dataRing && !XhciAllocTransferRing(s.dataRing, s.dataRingPhys, s.dataEnq, s.dataCycle)) { t.s("ring alloc\n"); return; }
    if (!s.fbRing && !XhciAllocTransferRing(s.fbRing, s.fbRingPhys, s.fbEnq, s.fbCycle)) { t.s("ring alloc\n"); return; }
    if (!s.dataBuf && !(s.dataBuf = static_cast<uint8_t*>(AllocDmaBuffer(8, s.dataBufPhys)))) { t.s("buffer alloc\n"); return; }
    if (!s.fbBuf && !(s.fbBuf = static_cast<uint8_t*>(AllocDmaBuffer(1, s.fbBufPhys)))) { t.s("buffer alloc\n"); return; }
    // Fresh rings each start: zero, re-link, producer cycle one.
    memset(s.dataRing, 0, XFER_RING_SIZE * sizeof(Trb)); XhciInitializeLink(s.dataRing[XFER_RING_SIZE - 1], s.dataRingPhys);
    memset(s.fbRing, 0, XFER_RING_SIZE * sizeof(Trb)); XhciInitializeLink(s.fbRing[XFER_RING_SIZE - 1], s.fbRingPhys);
    s.dataEnq = s.fbEnq = 0; s.dataCycle = s.fbCycle = true;
    s.ctrl = dev.ctrl; s.device = index; s.slotId = dev.slotId; s.desc = desc;
    s.dataDci = (desc.dataEp & 15) * 2; s.fbDci = (desc.feedbackEp & 15) * 2 + 1;
    s.frameBytes = uint32_t(desc.channels) * desc.subslot;
    s.completedHw = 0;
    s.produced = s.completed = s.fbProduced = s.fbCompleted = s.framesSent = 0;
    s.dataEvents = s.fbEvents = s.underruns = s.missed = s.resyncs = s.fbValues = s.lastBadCc = 0;
    for (auto& c : s.ccCount) c = 0;
    for (auto& u : s.underrunTick) u = 0;
    s.maxLoopGap = s.lastLoop = 0;
    s.stopRequested = s.resync = s.toneRequested = s.toneDone = false; s.toneStartTick = s.toneEndTick = 0;
    s.rate = rate;
    s.mixerMode = mixer; s.primed = s.flushRequested = false; s.mixRead = s.mixWrite = 0;
    s.mixFramesIn = s.mixFramesOut = s.mixUnderflows = 0;
    s.primeWait = 0; s.nativeOutBase = 0; s.nativeSubmitted = 0;
    // A stream restarted under an already registered mixer driver is a new
    // epoch (the mixer sees the discontinuity and calls restart()).
    s.nativeDiscontinuity = g_uacAudioRegistered;
    s.fuOverride = false; s.fuKnown = false; s.volumeSkip = 0;

    // Playback volume range (feature unit), then the clock rate.
    uint8_t range[8]{};
    const uint16_t fuIndex = uint16_t(desc.featureId << 8 | desc.controlInterface);
    if (!XhciUacControl(dev, 0xa1, 2, 0x0201, fuIndex, nullptr, 8, range)) { t.s("volume range read failed\n"); return; }
    s.volumeMin = int16_t(range[2] | range[3] << 8); s.volumeMax = int16_t(range[4] | range[5] << 8);
    // Silence is streamed either way; a failed minimum is reported and blocks tones.
    s.volumeControl = XhciUacSetVolume(dev, s.volumeMin);
    s.volumeSkip = g_uacVolumeFailMask & ~0x6u; // stalled extra channels are not retried
    s.fuCurrentQ8 = s.volumeMin; s.fuKnown = s.volumeControl;
    if (!s.volumeControl) t.s("warning: volume SET_CUR (minimum) failed on channel 1/2; tones disabled\n");
    const uint16_t clockIndex = uint16_t(desc.clockId << 8 | desc.controlInterface);
    uint32_t readback = 0;
    if (!XhciUacControl(dev, 0x21, 1, 0x0100, clockIndex, &rate, 4)) { t.s("SET_CUR sample rate failed\n"); return; }
    (void)XhciUacControl(dev, 0xa1, 1, 0x0100, clockIndex, nullptr, 4, &readback);
    s.rateReadback = readback;
    s.pacer.Start(s.rate, 8000, (desc.dataMaxPacket & 0x7ff) / s.frameBytes);

    const uint32_t cc = XhciUacConfigureEndpoints(*dev.ctrl, dev, true);
    if (cc != TRB_CC_SUCCESS) { t.s("Configure Endpoint failed cc="); t.u(cc); t.nl(); return; }
    if (!XhciUacControl(dev, 0x01, 11, desc.alternate, desc.streamInterface, nullptr, 0)) {
        (void)XhciUacConfigureEndpoints(*dev.ctrl, dev, false); t.s("SET_INTERFACE failed\n"); return;
    }
    __atomic_store_n(&s.active, true, __ATOMIC_RELEASE);
    if (!g_uacControlRunning) {
        g_uacControlRunning = true;
        if (!KernelWorkerStart("uac_control", XhciUacControlThread, nullptr)) g_uacControlRunning = false;
    }
    if (!KernelWorkerStart("uac_stream", XhciUacThread, nullptr)) {
        s.active = false;
        (void)XhciUacControl(dev, 0x01, 11, 0, desc.streamInterface, nullptr, 0);
        (void)XhciUacConfigureEndpoints(*dev.ctrl, dev, false);
        t.s("worker queue full\n"); return;
    }
    KPrintf("uac: EVO4 %s stream started if%u alt%u %u Hz (readback %u) volume min %d/256 dB\n", mixer ? "mixer" : "test",
            desc.streamInterface, desc.alternate, s.rate, readback, int(s.volumeMin));
    t.s("started\n");
}

static void XhciUacStop(XhciText& t)
{
    auto& s = g_uac;
    if (!s.active) { t.s("not active\n"); return; }
    s.stopRequested = true;
    for (uint32_t i = 0; i < 200 && s.running; ++i) SchedulerSleepMs(1);
    auto& dev = g_devices[s.device];
    if (!dev.gone) {
        (void)XhciUacSetVolume(dev, s.volumeMin);
        (void)XhciUacControl(dev, 0x01, 11, 0, s.desc.streamInterface, nullptr, 0);
        const uint32_t dcis[2] = {s.dataDci, s.fbDci};
        for (uint32_t dci : dcis)
            (void)XhciSlotCommand(*s.ctrl, 0, TRB_TYPE_STOP_EP | (s.slotId << 24) | (dci << 16));
        const uint32_t cc = XhciUacConfigureEndpoints(*s.ctrl, dev, false);
        t.s("drop cc="); t.u(cc); t.nl();
    }
    __atomic_store_n(&s.active, false, __ATOMIC_RELEASE);
    KPrintf("uac: EVO4 stream stopped (produced %u, underruns %u, missed %u)\n", unsigned(s.produced),
            unsigned(s.underruns), unsigned(s.missed));
    t.s("stopped\n");
}

// Parse "<dB> [audible]"; enforce the range and the audio-muted marker.
static bool XhciUacLevelArg(XhciText& t, const char*& p, int minimum, int maximum, int& dB)
{
    while (*p == ' ') ++p;
    bool negative = false; if (*p == '-') { negative = true; ++p; }
    if (*p < '0' || *p > '9') { t.s("expected a level in dB\n"); return false; }
    dB = 0; while (*p >= '0' && *p <= '9') dB = dB * 10 + (*p++ - '0');
    if (negative) dB = -dB;
    while (*p == ' ') ++p;
    const bool audible = XhciArgIs(p, "audible");
    bool muted = false;
    if (auto* marker = VfsOpen("/etc/audio-muted")) { VfsClose(marker); muted = true; }
    if (dB > maximum || dB < minimum) { t.s("refused: level out of range\n"); return false; }
    if (muted && !audible) { t.s("refused: audio-muted marker present; append 'audible'\n"); return false; }
    if (!g_uac.volumeControl) { t.s("refused: volume control unavailable\n"); return false; }
    return true;
}

// Hand the feature unit to an explicit level and wait (<=300 ms) until the
// stream thread has applied it.
static bool XhciUacApplyOverride(bool on, int16_t q8)
{
    auto& s = g_uac;
    s.fuOverrideQ8 = q8; s.fuOverride = on;
    const int16_t want = on ? q8 : s.volumeMin;
    for (uint32_t i = 0; i < 300; ++i) {
        if (s.fuKnown && s.fuCurrentQ8 == want) return true;
        SchedulerSleepMs(1);
    }
    return false;
}

static void XhciUacTone(XhciText& t, const char* p)
{
    auto& s = g_uac;
    if (!s.active || !s.running) { t.s("stream not running\n"); return; }
    int dB = 0;
    if (!XhciUacLevelArg(t, p, -90, -20, dB)) return;
    s.toneStartTick = s.toneEndTick = 0; s.toneDone = false;
    const uint64_t volumeTick = ApicTickCount();
    if (!XhciUacApplyOverride(true, Uac2VolumeQ8(dB, s.volumeMin, s.volumeMax))) {
        (void)XhciUacApplyOverride(false, 0); t.s("volume set failed\n"); return;
    }
    KPrintf("uac: EVO4 volume %d dB from tick %u; 440 Hz tone for 1 s\n", dB, unsigned(volumeTick));
    s.toneRequested = true;
    for (uint32_t i = 0; i < 2500 && !s.toneDone; ++i) SchedulerSleepMs(1);
    s.toneRequested = false;
    SchedulerSleepMs(40); // let the queued tail (stream lead 28 ms) drain
    const bool restored = XhciUacApplyOverride(false, 0);
    const uint64_t restoreTick = ApicTickCount();
    KPrintf("uac: EVO4 tone generated ticks %u..%u, level released at %u (%s)\n", unsigned(s.toneStartTick),
            unsigned(s.toneEndTick), unsigned(restoreTick), restored ? "ok" : "FAILED");
    t.s("tone volume_tick="); t.u(volumeTick); t.s(" start="); t.u(s.toneStartTick); t.s(" end="); t.u(s.toneEndTick);
    t.s(" restore="); t.u(restoreTick); t.s(restored ? " released\n" : " RELEASE FAILED\n");
}

// Kernel mixer sink (legacy play contract: 44.1 kHz stereo S16, partial
// acceptance, never blocks). Without a mixer-mode stream, data is discarded
// so mixer clients never stall on an absent device.
static int XhciUacPlay(const void* samples, uint32_t bytes, uint32_t rate, uint8_t channels, uint8_t bits, bool)
{
    auto& s = g_uac;
    if (rate != 44100 || channels != 2 || bits != 16 || (bytes & 3)) return -22;
    if (!s.active || !s.mixerMode || !s.running) { s.mixDropped += bytes / 4; return int(bytes); }
    const uint32_t used = s.mixWrite - __atomic_load_n(&s.mixRead, __ATOMIC_ACQUIRE);
    uint32_t frames = bytes / 4, space = UAC_MIX_FRAMES - used;
    if (frames > space) frames = space;
    const auto* in = static_cast<const int16_t*>(samples);
    for (uint32_t i = 0; i < frames; ++i) {
        const uint32_t at = ((s.mixWrite + i) % UAC_MIX_FRAMES) * 2;
        g_uacMix[at] = in[i * 2]; g_uacMix[at + 1] = in[i * 2 + 1];
    }
    __atomic_store_n(&s.mixWrite, s.mixWrite + frames, __ATOMIC_RELEASE);
    s.mixFramesIn += frames;
    return int(frames * 4);
}
static void XhciUacAudioStop() { g_uac.flushRequested = true; }
static bool XhciUacStreaming() { return g_uac.active && g_uac.mixerMode && g_uac.running && !g_devices[g_uac.device].gone; }
static bool XhciUacProgress(AudioDeviceProgress* result)
{
    auto& s = g_uac;
    if (!result || !AudioClockNowNs()) return false;
    if (!XhciUacStreaming()) s.nativeDiscontinuity = true;
    const uint64_t out = (__atomic_load_n(&s.mixFramesOut, __ATOMIC_ACQUIRE) - s.nativeOutBase) * 4;
    const uint64_t submitted = s.nativeSubmitted;
    // Capacity is the publication lead the mixer may keep queued (12 KiB, as
    // intel_hda's native target), well inside the 32 KiB sink ring.
    *result = {s.nativeEpoch, submitted, out < submitted ? out : submitted, 12288,
               XhciUacStreaming(), s.nativeDiscontinuity};
    return true;
}
static AudioSubmitReceipt XhciUacTrySubmit(const void* samples, uint32_t bytes)
{
    auto& s = g_uac;
    AudioSubmitReceipt receipt{s.nativeEpoch, s.nativeSubmitted, 0, 0};
    if (!samples || !bytes || (bytes & 3)) { receipt.error = -22; return receipt; }
    if (!AudioClockNowNs()) { receipt.error = -95; return receipt; }
    if (s.nativeDiscontinuity || !XhciUacStreaming()) { s.nativeDiscontinuity = true; receipt.error = -32; return receipt; }
    const uint32_t used = s.mixWrite - __atomic_load_n(&s.mixRead, __ATOMIC_ACQUIRE);
    uint32_t frames = bytes / 4, space = UAC_MIX_FRAMES - used;
    if (frames > space) frames = space;
    const auto* in = static_cast<const int16_t*>(samples);
    for (uint32_t i = 0; i < frames; ++i) {
        const uint32_t at = ((s.mixWrite + i) % UAC_MIX_FRAMES) * 2;
        g_uacMix[at] = in[i * 2]; g_uacMix[at + 1] = in[i * 2 + 1];
    }
    __atomic_store_n(&s.mixWrite, s.mixWrite + frames, __ATOMIC_RELEASE);
    s.mixFramesIn += frames;
    s.nativeSubmitted += frames * 4;
    receipt.accepted = frames * 4;
    return receipt;
}
static bool XhciUacRestart(uint64_t expectedEpoch)
{
    auto& s = g_uac;
    if (!s.nativeDiscontinuity || expectedEpoch != s.nativeEpoch || !XhciUacStreaming()) return false;
    s.flushRequested = true;
    for (uint32_t i = 0; i < 50 && s.flushRequested; ++i) SchedulerSleepMs(1);
    s.nativeOutBase = s.mixFramesOut; s.nativeSubmitted = 0;
    s.nativeDiscontinuity = false; ++s.nativeEpoch;
    return true;
}
static bool XhciUacAudioPlaying() { return g_uac.active && g_uac.mixerMode && g_uac.mixWrite != g_uac.mixRead; }
static uint32_t XhciUacAudioPosition() { return uint32_t(g_uac.mixFramesOut * 4); }
static AudioDriver g_uacAudioDriver = {"usb_uac2", XhciUacPlay, XhciUacAudioStop, XhciUacAudioPlaying, XhciUacAudioPosition,
                                       nullptr, XhciUacTrySubmit, XhciUacProgress, XhciUacRestart};

static uint32_t XhciUacCommand(const char* p, XhciText& t)
{
    auto& s = g_uac;
    while (*p == ' ') ++p;
    if (XhciArgIs(p, "usb")) while (*p == ' ') ++p;
    if (!*p || XhciArgIs(p, "status")) XhciUacStatus(t);
    else if (XhciArgIs(p, "start")) { XhciUacStart(t, 48000, false); XhciUacStatus(t); }
    else if (XhciArgIs(p, "mixer")) {
        // Stream first, so the mixer's first refresh already sees a running
        // native clock (it latches legacy delivery otherwise).
        XhciUacStart(t, 44100, true);
        if (!g_uacAudioRegistered && s.active) {
            g_uacAudioRegistered = AudioRegister(&g_uacAudioDriver);
            if (!g_uacAudioRegistered) t.s("another audio driver is registered\n");
        }
        XhciUacStatus(t);
    }
    else if (XhciArgIs(p, "stop")) { XhciUacStop(t); XhciUacStatus(t); }
    else if (XhciArgIs(p, "tone")) { XhciUacTone(t, p); XhciUacStatus(t); }
    else if (XhciArgIs(p, "level")) {
        int dB = 0;
        if (XhciUacLevelArg(t, p, -90, 0, dB)) {
            s.unmutedQ8 = Uac2VolumeQ8(dB, s.volumeMin, s.volumeMax);
            t.s("unmuted level set (applies after an explicit user unmute)\n");
        }
        XhciUacStatus(t);
    }
    else if (XhciArgIs(p, "drain")) {
        // Wait (<=2 s) until the mixer sink ring is empty, plus the stream lead.
        uint32_t waited = 0;
        while (s.active && s.mixerMode && s.mixWrite != s.mixRead && waited < 2000) { SchedulerSleepMs(1); ++waited; }
        SchedulerSleepMs(40);
        t.s("drained after "); t.u(waited); t.s(" ms\n");
    }
    else if (XhciArgIs(p, "override")) {
        while (*p == ' ') ++p;
        if (XhciArgIs(p, "off")) t.s(XhciUacApplyOverride(false, 0) ? "override off\n" : "override off: not applied\n");
        else { int dB = 0; if (s.running && XhciUacLevelArg(t, p, -90, -20, dB))
            t.s(XhciUacApplyOverride(true, Uac2VolumeQ8(dB, s.volumeMin, s.volumeMax)) ? "override on\n" : "override: not applied\n"); }
    }
    else t.s("usage: bridge hardware audio usb [status|start|mixer|stop|tone <dB> audible|level <dB> audible|override <dB> audible|off]");
    return t.len;
}

// Desktop live stick (/etc/usb-audio-mixer): once the EVO4 is present, make
// it the mixer sink. Volume stays at the device minimum until an explicit
// user unmute (see the stream thread); the mixer itself starts muted when
// /etc/audio-muted exists.
// Rings and buffers for the UAC stream are allocated once, at module init,
// before large boot-time RAM images move the allocator above the DMA limit.
static bool XhciUacPrealloc()
{
    auto& s = g_uac;
    if (!s.dataRing && !XhciAllocTransferRing(s.dataRing, s.dataRingPhys, s.dataEnq, s.dataCycle)) return false;
    if (!s.fbRing && !XhciAllocTransferRing(s.fbRing, s.fbRingPhys, s.fbEnq, s.fbCycle)) return false;
    if (!s.dataBuf && !(s.dataBuf = static_cast<uint8_t*>(AllocDmaBuffer(8, s.dataBufPhys)))) return false;
    if (!s.fbBuf && !(s.fbBuf = static_cast<uint8_t*>(AllocDmaBuffer(1, s.fbBufPhys)))) return false;
    return true;
}

// Set once the /etc/usb-audio-mixer autostart ran: the hotplug worker then
// keeps the EVO4 as the mixer sink across detach/attach (KVM switch).
static volatile bool g_uacAutoMixer = false;
static void XhciUacAutoStart(void*)
{
    g_uacAutoMixer = true;
    uint32_t index = 0;
    for (uint32_t i = 0; i < 60 && !XhciUacFindDevice(index); ++i) SchedulerSleepMs(500);
    static char text[1024];
    XhciText t{text, sizeof(text), 0};
    XhciUacCommand("usb mixer", t);
    text[t.len < sizeof(text) ? t.len : sizeof(text) - 1] = 0;
    char* end = text; while (*end && *end != '\n') ++end; *end = 0;
    KPrintf("uac: autostart (usb-audio-mixer marker): %s\n", text);
}

// EVO4 detach/attach: the stream thread exits when its device is gone; clear
// the stream so a re-attached EVO4 (new slot) can be brought up again as the
// mixer sink. The mixer driver stays registered; the restart is a new epoch
// (discontinuity), so mixer clients carry on.
static void XhciUacHotplugTick()
{
    static uint64_t nextTryMs = 0;
    static uint32_t fails = 0;
    auto& s = g_uac;
    if (s.active && g_devices[s.device].gone) {
        s.stopRequested = true;
        for (uint32_t i = 0; i < 200 && s.running; ++i) SchedulerSleepMs(1);
        if (s.running) return; // stream thread still unwinding; next tick
        __atomic_store_n(&s.active, false, __ATOMIC_RELEASE);
        KPrintf("uac: EVO4 detached; stream cleared (produced %u, underruns %u)\n",
                unsigned(s.produced), unsigned(s.underruns));
        nextTryMs = 0; fails = 0;
    }
    if (!g_uacAutoMixer || s.active) return;
    uint32_t index = 0;
    if (!XhciUacFindDevice(index)) return;
    const uint64_t now = ApicTickCount();
    if (now < nextTryMs) return;
    static char text[256];
    XhciText t{text, sizeof(text), 0};
    XhciUacCommand("usb mixer", t); // same path as the boot autostart
    text[t.len < sizeof(text) ? t.len : sizeof(text) - 1] = 0;
    char* end = text; while (*end && *end != '\n') ++end; *end = 0;
    if (s.active) { fails = 0; KPrintf("uac: EVO4 re-attached; mixer stream restarted: %s\n", text); return; }
    ++fails; nextTryMs = now + (fails >= 5 ? 30000 : 2000);
    KPrintf("uac: EVO4 re-attach start failed (%u): %s\n", fails, text);
}

static int XhciModuleInit()
{
    SerialPuts("xhci: initializing USB 3.0 host controller driver\n");
    g_storageReadOnly=UsbSystemRequested();
    if(auto* marker=VfsOpen("/etc/usb-read-only")) {
        g_storageReadOnly=true;VfsClose(marker);KPuts("xhci: physical storage probe is read-only\n");
    }
    if(auto* marker=VfsOpen("/etc/usb-msc-page-chunks")) {
        g_storagePageChunks=true;VfsClose(marker);
        KPuts("xhci: diagnostic MSC limit=4096 bytes (single data TRB)\n");
    }

    if(UsbSystemRequested()) {
        if(auto* marker=VfsOpen("/etc/usb-uas")) {
            g_storageUas=true;
            VfsClose(marker);
            if(auto* fixture=VfsOpen("/etc/usb-uas-fixture")) {
                uint32_t eax=1,ecx=0,ebx,edx;
                __asm__ volatile("cpuid":"+a"(eax),"=b"(ebx),"+c"(ecx),"=d"(edx));
                g_storageUasFixture=(ecx>>31)!=0;
                VfsClose(fixture);
            }
        }
        if(auto* marker=VfsOpen("/etc/usb-bot-legacy")) {
            g_storageLegacyBurst=true;
            VfsClose(marker);
        }
        if(auto* marker=VfsOpen("/etc/usb-bot-pipeline")) {
            g_storagePipeline=!g_storageLegacyBurst;
            VfsClose(marker);
        }
    }

    // Drive every xHCI function: chipset and add-in controllers alike.
    PciDevice found[MAX_CONTROLLERS];
    const uint32_t count = XhciFindControllers(found, MAX_CONTROLLERS);
    if (!count) {
        SerialPuts("xhci: no controller detected, module inactive\n");
        static char none[128];
        const uint32_t length = XhciStatusText(none, sizeof(none));
        UsbStatusPublish(none, length);
        return 0; // not an error — just no hardware present
    }
    uint32_t up = 0;
    for (uint32_t i = 0; i < count; ++i) {
        auto& ctrl = g_controllers[i];
        ctrl.index = i;
        ctrl.pciDev = found[i];
        __atomic_store_n(&g_controllerCount, i + 1, __ATOMIC_RELEASE);
        if (XhciBringUp(ctrl)) ++up;
        else KPrintf("xhci: c%u %04x:%04x not driven: %s\n", i, found[i].vendorId, found[i].deviceId,
                     ctrl.failure ? ctrl.failure : "unknown");
    }

    // Scan ports and enumerate connected devices, hubs included.
    for (uint32_t i = 0; i < count; ++i) {
        auto& ctrl = g_controllers[i];
        if (!ctrl.initialized) continue;
        XhciInitialRootScan(ctrl);
        // Process any pending port status change events
        while (XhciWaitForEvent(ctrl, 0, 50)) {}
    }
    if(g_storageReadOnly)KPrintf("xhci: initial port scan found %u USB devices\n",g_deviceCount);

    for (uint32_t i = 0; i < count; ++i) {
        auto& ctrl = g_controllers[i];
        if (!ctrl.initialized) continue;
        ctrl.completionFailed = false;
        XhciSetupInterrupts(ctrl);
    }

    KPrintf("xhci: driver initialized (%u of %u controller%s, %u device%s)\n",
                 up, count, count > 1 ? "s" : "",
                 g_deviceCount, g_deviceCount != 1 ? "s" : "");

    static char text[6144];
    const uint32_t length = XhciStatusText(text, sizeof(text));
    UsbStatusPublish(text, length);
    if (up) {
        // The provider and worker live in this module: register them only
        // when init succeeds, because a failed module is unloaded. Hotplug
        // (root and hub port polling) is opt-in so existing payloads keep
        // their boot-time-only enumeration.
        UsbStatusSetProvider(XhciStatusText);
        DebugUsbCommandSetHandler(XhciUsbCommand);
        if (!XhciUacPrealloc()) KPuts("uac: stream buffers unavailable at init\n");
        // Late-enumeration reserve (see AllocDmaReserve): 8 MiB, else 2 MiB.
        static const uint32_t reserveSizes[] = {2048u, 512u};
        for (uint32_t pages : reserveSizes) {
            uint64_t phys = 0;
            if (auto* r = static_cast<uint8_t*>(AllocDmaBufferDirect(pages, phys, PmmOwner::XhciDma))) {
                g_dmaReservePhys = phys; g_dmaReservePages = pages; g_dmaReserveMap.init(pages); g_dmaReserve = r;
                KPrintf("xhci: DMA reserve %u pages at %lx for hot-plug enumeration\n", pages,
                        static_cast<unsigned long>(phys));
                break;
            }
        }
        if (auto* marker = VfsOpen("/etc/usb-audio-mixer")) {
            VfsClose(marker);
            if (!KernelWorkerStart("uac_auto", XhciUacAutoStart, nullptr)) KPuts("uac: autostart worker queue full\n");
        }
        (void)PlatformPowerAddQuiesce(XhciQuiesceForReset);
        bool hotplug = false;
        static const char* const markers[] = {"/etc/usb-hotplug", "/etc/boot-status"};
        for (const char* path : markers)
            if (auto* marker = VfsOpen(path)) { VfsClose(marker); hotplug = true; }
        if (hotplug && !KernelWorkerStart("xhci_hotplug", XhciHotplugThread, nullptr))
            KPuts("xhci: hotplug worker unavailable (no late devices)\n");
    }
    BootStatusPrint("xhci init");
    return up ? 0 : -1;
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
