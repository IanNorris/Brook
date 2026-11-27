// Intel 8254x Software Developer's Manual, sections 3 and 13.
// Intel 82574 GbE Controller Family Datasheet, chapters 7, 8 and 10.
// One legacy RX/TX queue, standard Ethernet MTU, optional 82574 MSI or shared PCI INTx.
#include "e1000_ring.h"
#include "e1000_msi.h"
#include "e1000_pci_path.h"

#include "apic.h"
#include "idt.h"
#include "irq_wrapper.h"
#include "kprintf.h"
#include "memory/physical_memory.h"
#include "memory/virtual_memory.h"
#include "module_abi.h"
#include "net.h"
#include "pci.h"
#include "serial.h"
#include "spinlock.h"
#include "vfs.h"
#ifdef BROOK_HOST_TEST
#include <string.h>
#else
#include "string.h"
#endif

MODULE_IMPORT_SYMBOL(ApicTickCount);
MODULE_IMPORT_SYMBOL(ApicGetId);
MODULE_IMPORT_SYMBOL(ApicSendEoi);
MODULE_IMPORT_SYMBOL(IdtInstallHandler);
MODULE_IMPORT_SYMBOL(NetSetMsiPolling);
MODULE_IMPORT_SYMBOL(NetWakeMsiPollThread);
MODULE_IMPORT_SYMBOL(PciConfigWrite32);
MODULE_IMPORT_SYMBOL(IoApicRegisterHandler);
MODULE_IMPORT_SYMBOL(IoApicResolvePin);
MODULE_IMPORT_SYMBOL(IoApicUnregisterHandler);
MODULE_IMPORT_SYMBOL(KPrintf);
MODULE_IMPORT_SYMBOL(NetReceive);
MODULE_IMPORT_SYMBOL(NetRegisterIf);
MODULE_IMPORT_SYMBOL(NetWakePollThread);
MODULE_IMPORT_SYMBOL(PciConfigRead8);
MODULE_IMPORT_SYMBOL(PciConfigRead16);
MODULE_IMPORT_SYMBOL(PciConfigRead32);
MODULE_IMPORT_SYMBOL(PciConfigWrite16);
MODULE_IMPORT_SYMBOL(PciFindDevice);
MODULE_IMPORT_SYMBOL(PmmFreePage);
MODULE_IMPORT_SYMBOL(SerialPrintf);
MODULE_IMPORT_SYMBOL(VfsClose);
MODULE_IMPORT_SYMBOL(VfsOpen);
MODULE_IMPORT_SYMBOL(VmmAllocPages);
MODULE_IMPORT_SYMBOL(VmmAllocPagesAccounted);
MODULE_IMPORT_SYMBOL(VmmDetachPage);
MODULE_IMPORT_SYMBOL(VmmFreePages);
MODULE_IMPORT_SYMBOL(VmmMapPage);
MODULE_IMPORT_SYMBOL(VmmVirtToPhys);

namespace {
using namespace brook;
using namespace brook::e1000;

enum Register : uint32_t {
    Ctrl = 0x0000, Status = 0x0008, Eecd = 0x0010, CtrlExt = 0x0018,
    Icr = 0x00c0, Itr = 0x00c4, Ims = 0x00d0, Imc = 0x00d8,
    Rctl = 0x0100, Tctl = 0x0400, Tipg = 0x0410,
    Rdbal = 0x2800, Rdbah = 0x2804, Rdlen = 0x2808, Rdh = 0x2810, Rdt = 0x2818,
    Rdtr = 0x2820, Radv = 0x282c,
    Tdbal = 0x3800, Tdbah = 0x3804, Tdlen = 0x3808, Tdh = 0x3810, Tdt = 0x3818,
    Rxcsum = 0x5000, Rfctl = 0x5008, Mta = 0x5200, Ral = 0x5400, Rah = 0x5404,
};

constexpr uint32_t CtrlReset = 1u << 26, CtrlSetLinkUp = 1u << 6;
constexpr uint32_t CtrlPcieMasterDisable = 1u << 2;
constexpr uint32_t StatusLinkUp = 1u << 1, StatusPcieMasterEnabled = 1u << 19;
constexpr uint32_t EecdAutoloadDone = 1u << 9, RxExtendedStatus = 1u << 15;
constexpr uint32_t RxEnable = 1u << 1, RxBroadcast = 1u << 15, RxStripFcs = 1u << 26;
constexpr uint32_t TxEnable = 1u << 1, TxPadShort = 1u << 3;
constexpr uint32_t InterruptLink = 1u << 2;
constexpr uint32_t InterruptRx = (1u << 4) | (1u << 6) | (1u << 7);
constexpr uint16_t PciMemory = 1u << 1, PciMaster = 1u << 2, PciIntDisable = 1u << 10;
constexpr uint32_t PageSize = 4096;
constexpr uint64_t MaxPhysicalPage = 0x000ffffffffff000ULL;
constexpr uint32_t MmioPages = (Rah + PageSize) / PageSize;
constexpr uint32_t PacketPages = RingSize * BufferSize / PageSize;
static_assert(RingSize * sizeof(RxDescriptor) <= PageSize);
static_assert(PageSize % BufferSize == 0);

struct DmaRing {
    VirtualAddress descriptors;
    VirtualAddress packets;
};

PciDevice g_device{};
const Device* g_model = nullptr;
bool g_dmaEnabled = false;
uint16_t g_pciCommand = 0;
volatile uint32_t* g_regs = nullptr;
uint32_t g_mappedPages = 0;
uint8_t g_irq = 0xff;
NetIf g_interface{};
DmaRing g_rxDma{}, g_txDma{};
uint8_t g_msiCap;
RxRing g_rx{};
TxRing g_tx{};
SpinLock g_rxLock{}, g_txLock{};
bool g_linkUp = false;
uint64_t g_nextLinkSample = 0;

uint32_t Read(Register reg) { return g_regs[reg / 4]; }
void Write(Register reg, uint32_t value) { g_regs[reg / 4] = value; }

void SetPciCommand(uint16_t command)
{
    PciConfigWrite16(g_device.bus, g_device.dev, g_device.fn, 0x04, command);
}

bool EnableUpstream()
{
    Bridge path[32];
    unsigned count;
    if (!FindBridgePath(g_device.bus, PciConfigRead32, path, count)) return false;
    for (unsigned i = 0; i < count; ++i) {
        const auto& b = path[i];
        const uint16_t before = PciConfigRead16(b.bus, b.device, b.function, 0x04);
        constexpr uint16_t required = PciMemory | PciMaster;
        if ((before & required) != required)
            PciConfigWrite16(b.bus, b.device, b.function, 0x04, before | required);
        const uint16_t after = PciConfigRead16(b.bus, b.device, b.function, 0x04);
        SerialPrintf("e1000: bridge %02x:%02x.%u secondary=%u command=%04x->%04x\n",
                     b.bus, b.device, b.function, b.secondary, before, after);
        if ((after & required) != required) return false;
    }
    return true;
}

void FreeMmio()
{
    if (!g_regs) return;
    const VirtualAddress base(reinterpret_cast<uint64_t>(g_regs));
    for (uint32_t i = 0; i < g_mappedPages; ++i)
        VmmDetachPage(KernelPageTable, base + i * PageSize);
    VmmFreePages(base, MmioPages);
    g_regs = nullptr;
    g_mappedPages = 0;
}

bool MapMmio(uint64_t physical)
{
    const auto base = VmmAllocPages(MmioPages, VMM_WRITABLE | VMM_NO_EXEC, MemTag::Device, KernelPid);
    if (!base) return false;
    g_regs = reinterpret_cast<volatile uint32_t*>(base.raw());
    for (; g_mappedPages < MmioPages; ++g_mappedPages) {
        const uint64_t offset = g_mappedPages * PageSize;
        const auto address = base + offset;
        const auto backing = VmmVirtToPhys(KernelPageTable, address);
        if (!VmmMapPage(KernelPageTable, address, PhysicalAddress(physical + offset),
                        VMM_WRITABLE | VMM_NO_EXEC | VMM_CACHE_DISABLE | VMM_WRITE_THRU,
                        MemTag::Device, KernelPid)) return false;
        if (backing) PmmFreePage(backing);
    }
    return true;
}

void FreeDma(DmaRing& dma)
{
    if (dma.descriptors) VmmFreePages(dma.descriptors, 1);
    if (dma.packets) VmmFreePages(dma.packets, PacketPages);
    dma = {};
}

template <typename Descriptor>
bool AllocateDma(DmaRing& dma)
{
    constexpr uint64_t flags = VMM_WRITABLE | VMM_NO_EXEC;
    dma.descriptors = VmmAllocPagesAccounted(1, flags, MemTag::Device, KernelPid, PmmOwner::NetworkDma);
    if (!dma.descriptors) return false;
    dma.packets = VmmAllocPagesAccounted(PacketPages, flags, MemTag::Device, KernelPid, PmmOwner::NetworkDma);
    if (!dma.packets) return false;

    auto* descriptors = reinterpret_cast<Descriptor*>(dma.descriptors.raw());
    memset(descriptors, 0, RingSize * sizeof(Descriptor));
    for (uint16_t i = 0; i < RingSize; ++i) {
        // Each buffer fits in its own half page. VM allocations do not imply
        // consecutive bus addresses.
        const auto physical = VmmVirtToPhys(KernelPageTable, dma.packets + i * BufferSize);
        if (!physical) return false;
        descriptors[i].address = physical.raw();
    }
    return true;
}

void WriteRing(Register low, Register high, Register length, Register head, Register tail,
               VirtualAddress descriptors)
{
    const uint64_t physical = VmmVirtToPhys(KernelPageTable, descriptors).raw();
    Write(low, static_cast<uint32_t>(physical));
    Write(high, static_cast<uint32_t>(physical >> 32));
    Write(length, RingSize * sizeof(RxDescriptor));
    Write(head, 0);
    Write(tail, 0);
}

bool Reset(const Device& model)
{
    Write(Imc, ~0u);
    Write(Rctl, 0);
    Write(Tctl, 0);
    (void)Read(Status); // flush posted disabling writes before reset
    Write(Ctrl, Read(Ctrl) | CtrlReset);
    const uint64_t start = ApicTickCount();
    // Older parts need time for their EEPROM autoload. Give it at least 10 ms
    // after reset, and retain a hard bound if the boot clock stalls.
    for (uint32_t spin = 0; spin < 4000000; ++spin) {
        const uint64_t elapsed = ApicTickCount() - start;
        if (elapsed >= 10 && !(Read(Ctrl) & CtrlReset) &&
            (!model.pcie || (Read(Eecd) & EecdAutoloadDone))) {
            Write(Imc, ~0u);
            (void)Read(Icr);
            return true;
        }
        if (elapsed >= 100) break;
        __asm__ volatile("pause");
    }
    return false;
}

bool ReadMac(MacAddr& mac)
{
    const uint32_t low = Read(Ral), high = Read(Rah);
    for (unsigned i = 0; i < 4; ++i) mac.b[i] = low >> (8 * i);
    mac.b[4] = high;
    mac.b[5] = high >> 8;
    if ((mac.b[0] & 1) || !(low || (high & 0xffffu))) return false;
    Write(Ral, low);
    Write(Rah, (high & 0xffffu) | (1u << 31)); // address valid, destination filter
    return true;
}

int Submit(const void* frame, uint32_t length, bool tryOnly)
{
    if (!frame || length < MinFrameSize || length > MaxFrameSize) return -22;
    if (tryOnly) {
        if (!SpinLockTryAcquire(&g_txLock)) return -11;
    } else {
        SpinLockAcquire(&g_txLock);
    }
    auto* descriptors = reinterpret_cast<TxDescriptor*>(g_txDma.descriptors.raw());
    g_tx.Reap(descriptors);
    int result = -11;
    if (!g_tx.Full()) {
        const uint16_t slot = g_tx.tail;
        memcpy(reinterpret_cast<void*>((g_txDma.packets + slot * BufferSize).raw()), frame, length);
        descriptors[slot].length = static_cast<uint16_t>(length);
        descriptors[slot].command = TxCommand;
        descriptors[slot].status = 0;
        g_tx.tail = Next(slot);
        __asm__ volatile("sfence" ::: "memory");
        Write(Tdt, g_tx.tail);
        result = 0;
    }
    SpinLockRelease(&g_txLock);
    return result;
}

int Transmit(NetIf*, const void* frame, uint32_t length) { return Submit(frame, length, false); }
int TryTransmit(NetIf*, const void* frame, uint32_t length) { return Submit(frame, length, true); }

void Poll(NetIf* nif)
{
    // NetReceive can reenter polling via the stack. One poller owns the
    // completed buffers until delivery ends, while TX uses its own lock.
    if (!SpinLockTryAcquire(&g_rxLock)) return;
    if (SpinLockTryAcquire(&g_txLock)) {
        g_tx.Reap(reinterpret_cast<const TxDescriptor*>(g_txDma.descriptors.raw()));
        SpinLockRelease(&g_txLock);
    }
    const uint64_t now = ApicTickCount();
    if (now >= g_nextLinkSample) {
        g_nextLinkSample = now + 100;
        const bool linkUp = Read(Status) & StatusLinkUp;
        if (linkUp != g_linkUp) {
            g_linkUp = linkUp;
            SerialPrintf("e1000: link %s\n", linkUp ? "up" : "down");
        }
    }

    auto* descriptors = reinterpret_cast<RxDescriptor*>(g_rxDma.descriptors.raw());
    uint16_t last = g_rx.next;
    uint32_t count = 0;
    for (; count < RingSize; ++count) {
        const uint16_t slot = g_rx.next;
        auto& descriptor = descriptors[slot];
        const uint8_t status = __atomic_load_n(&descriptor.status, __ATOMIC_ACQUIRE);
        if (!(status & DescriptorDone)) break;
        const uint16_t length = descriptor.length;
        if (g_rx.Accept(status, descriptor.errors, length))
            NetReceive(nif, reinterpret_cast<const void*>((g_rxDma.packets + slot * BufferSize).raw()), length);
        descriptor.status = 0;
        g_rx.next = Next(slot);
        last = slot;
    }
    if (count) {
        __asm__ volatile("sfence" ::: "memory");
        Write(Rdt, last);
    }
    SpinLockRelease(&g_rxLock);
}

extern "C" void E1000eMsiInner()
{
    // ICR is read-to-clear. Run the network stack only on a thread.
    if (Read(Icr) & MsiCauses) NetWakeMsiPollThread(&g_interface);
    ApicSendEoi();
}

IRQ_NAKED_HANDLER(E1000eMsiIsr, E1000eMsiInner)

bool StartMsi()
{
    Write(Imc, ~0u);
    Write(CtrlExt, Read(CtrlExt) & ~InterruptAutoMask);
    (void)Read(Icr);
    Write(Itr, ModerationInterval);
    (void)Read(Status);
    PciMsiConfig config{g_device};
    g_msiCap = EnableMsi(config, ApicGetId(), E1000eMsiVector, [](uint8_t vector) {
        IdtInstallHandler(vector, reinterpret_cast<void*>(E1000eMsiIsr));
    });
    if (!g_msiCap) return false;
    NetSetMsiPolling(&g_interface, true);
    Write(Ims, MsiCauses);
    (void)Read(Status);
    return true;
}

void StopMsi()
{
    if (!g_msiCap) return;
    Write(Imc, ~0u);
    (void)Read(Status);
    PciMsiConfig config{g_device};
    DisableMsi(config, g_msiCap);
    NetSetMsiPolling(&g_interface, false);
    g_msiCap = 0;
}

void Interrupt()
{
    // ICR is read-to-clear. Do not call the network stack in hard IRQ context.
    if (Read(Icr) & (InterruptRx | InterruptLink)) NetWakePollThread();
}

bool DisablePcieMaster()
{
    Write(Ctrl, Read(Ctrl) | CtrlPcieMasterDisable);
    const uint64_t start = ApicTickCount();
    for (uint32_t spin = 0; spin < 100000; ++spin) {
        if (!(Read(Status) & StatusPcieMasterEnabled)) return true;
        if (ApicTickCount() - start >= 100) break;
        __asm__ volatile("pause");
    }
    return false;
}

void Stop()
{
    bool drained = true;
    if (g_regs && g_mappedPages == MmioPages) {
        StopMsi();
        Write(Imc, ~0u);
        Write(Rctl, 0);
        Write(Tctl, 0);
        (void)Read(Icr);
        if (g_dmaEnabled && g_model->pcie) drained = DisablePcieMaster();
    }
    SetPciCommand((g_pciCommand | PciMemory | PciIntDisable) & ~PciMaster);
    if (g_dmaEnabled) {
        const bool reset = Reset(*g_model);
        drained = drained && reset;
    }
    g_dmaEnabled = false;
    if (g_irq != 0xff) {
        IoApicUnregisterHandler(g_irq, reinterpret_cast<void*>(Interrupt));
        g_irq = 0xff;
    }
    if (drained) {
        FreeDma(g_rxDma);
        FreeDma(g_txDma);
    } else {
        SerialPrintf("e1000: DMA quiescence unconfirmed, keeping rings pinned\n");
    }
    FreeMmio();
    SetPciCommand(g_pciCommand & ~PciMaster);
}

bool Snapshot(NetIf* intf, NetDriverSnapshot* out)
{
    *out = {};
    out->received = intf->rxPackets;
    out->transmitted = intf->txPackets;
    out->rxDepth = out->txDepth = RingSize;
    out->linkKnown = true;
    out->linkUp = (Read(Status) & StatusLinkUp) != 0;
    return true;
}

int ModuleInit()
{
    const Device* model = nullptr;
    for (const auto& candidate : Devices) {
        if (PciFindDevice(0x8086, candidate.id, g_device)) {
            model = &candidate;
            break;
        }
    }
    if (!model) return -1;

    const uint32_t raw = g_device.bar[0];
    if (PciBarIsIo(raw) || (raw & 6u && !PciBarIs64(raw))) return -1;
    uint64_t physical = PciBarMemBase32(raw);
    if (PciBarIs64(raw)) physical |= uint64_t(g_device.bar[1]) << 32;
    if (!physical || physical % PageSize || physical > MaxPhysicalPage - (MmioPages - 1) * PageSize)
        return -1;

    if (!EnableUpstream()) {
        SerialPrintf("e1000: no enabled bridge path to bus %u\n", g_device.bus);
        return -1;
    }
    g_model = model;
    g_pciCommand = PciConfigRead16(g_device.bus, g_device.dev, g_device.fn, 0x04);
    SetPciCommand((g_pciCommand | PciMemory | PciIntDisable) & ~PciMaster);
    if (!MapMmio(physical) || !Reset(*model) || !ReadMac(g_interface.mac) ||
        !AllocateDma<RxDescriptor>(g_rxDma) || !AllocateDma<TxDescriptor>(g_txDma)) {
        SerialPrintf("e1000: %s setup failed\n", model->name);
        Stop();
        return -1;
    }

    Write(Ctrl, Read(Ctrl) | CtrlSetLinkUp);
    if (model->pcie) Write(Rfctl, Read(Rfctl) & ~RxExtendedStatus);
    Write(Rxcsum, 0);
    for (uint32_t i = 0; i < 128; ++i) Write(static_cast<Register>(Mta + i * 4), 0);
    Write(Rdtr, 0);
    Write(Radv, 0);
    WriteRing(Rdbal, Rdbah, Rdlen, Rdh, Rdt, g_rxDma.descriptors);
    WriteRing(Tdbal, Tdbah, Tdlen, Tdh, Tdt, g_txDma.descriptors);
    Write(Tipg, 8u | (8u << 10) | (6u << 20)); // copper

    const uint8_t firmwareLine = PciConfigRead8(g_device.bus, g_device.dev, g_device.fn, 0x3c);
    const uint16_t command = g_pciCommand | PciMemory | PciMaster | PciIntDisable;
    SetPciCommand(command);
    g_dmaEnabled = true;
    Write(Tctl, TxEnable | TxPadShort | (0x0fu << 4) | (0x40u << 12));
    Write(Rctl, RxEnable | RxBroadcast | RxStripFcs); // 2048-byte legacy buffers
    __asm__ volatile("sfence" ::: "memory");
    Write(Rdt, RingSize - 1);
    (void)Read(Icr);

    g_interface.transmit = Transmit;
    g_interface.tryTransmit = TryTransmit;
    g_interface.poll = Poll;
    g_interface.snapshot = Snapshot;
    const bool linkUp = Read(Status) & StatusLinkUp;
    g_linkUp = linkUp;
    if (!NetRegisterIf(&g_interface)) {
        Stop();
        return -1;
    }

    // Use the same MSI path as I219 when qualifying it on an 82574L.
    if (model->pcie) {
        if (auto* option = VfsOpen("/etc/e1000-msi")) {
            VfsClose(option);
            (void)StartMsi();
        }
    }
    // The firmware Interrupt Line is advisory. Only a fixture/operator
    // with a verified route should enable it. Never change the IOAPIC's
    // trigger or polarity based on that byte at module load time.
    if (auto* option = VfsOpen("/etc/e1000-intx")) {
        VfsClose(option);
        uint8_t pin;
        if (!g_msiCap && firmwareLine >= 3 && firmwareLine < 16 && IoApicResolvePin(firmwareLine, pin)) {
            IoApicRegisterHandler(pin, 47, reinterpret_cast<void*>(Interrupt));
            g_irq = pin;
            SetPciCommand(command & ~PciIntDisable);
            Write(Ims, InterruptRx | InterruptLink);
        }
    }
    SerialPrintf("e1000: PCI interrupt line=%u selected_gsi=%d mode=%s\n", firmwareLine,
                 g_irq == 0xff ? -1 : int(firmwareLine), g_msiCap ? "msi" : (g_irq == 0xff ? "poll" : "intx"));
    const auto& mac = g_interface.mac.b;
    KPrintf("e1000: %s ready, MAC=%02x:%02x:%02x:%02x:%02x:%02x link=%s irq=%u\n",
            model->name, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], linkUp ? "up" : "down", g_irq);
    return 0;
}

}

// NetRegisterIf retains the module for the interface's lifetime.
DECLARE_MODULE("e1000", ModuleInit, Stop, "Intel 8254x and 82574 Ethernet");
