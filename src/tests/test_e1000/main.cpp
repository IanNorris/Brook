#include <cstdio>
#include "../../drivers/e1000/e1000_mod.cpp"

namespace {
unsigned failures = 0, packets = 0, wakes = 0, msiWakes = 0, eois = 0;
uint64_t monotonic = 0;
uint32_t registers[MmioPages * PageSize / 4];
TxDescriptor tx[RingSize];
RxDescriptor rx[RingSize];
uint8_t txBuffers[RingSize * BufferSize], rxBuffers[RingSize * BufferSize];

#define CHECK(expr) do { \
    if (!(expr)) { std::fprintf(stderr, "e1000 line %d: %s\n", __LINE__, #expr); ++failures; } \
} while (0)

struct MockBridge {
    uint8_t bus, device, function, secondary, subordinate;
    uint16_t command;
    bool writable;
};

MockBridge bridges[] = {
    {0, 4, 0, 2, 7, 0x100, true},
    {2, 1, 0, 3, 6, 0x002, true},
    {0, 5, 0, 8, 9, 0x000, true},
};
unsigned configWrites = 0;

MockBridge* LookupBridge(uint8_t bus, uint8_t device, uint8_t function)
{
    for (auto& bridge : bridges)
        if (bridge.bus == bus && bridge.device == device && bridge.function == function) return &bridge;
    return nullptr;
}

struct MsiConfigFixture {
    uint8_t bytes[256]{};
    unsigned writes = 0;
    bool acceptEnable = true;

    uint8_t read8(unsigned off) { return bytes[off]; }
    uint16_t read16(unsigned off) { return bytes[off] | (uint16_t(bytes[off + 1]) << 8); }
    uint32_t read32(unsigned off) { return read16(off) | (uint32_t(read16(off + 2)) << 16); }
    void write16(unsigned off, uint16_t val) {
        if (off == 0x52 && !acceptEnable) val &= ~MsiEnable;
        bytes[off] = val;
        bytes[off + 1] = val >> 8;
        ++writes;
    }
    void write32(unsigned off, uint32_t val) {
        write16(off, val);
        write16(off + 2, val >> 16);
    }
};

void ValidateMsi()
{
    MsiConfigFixture cfg;
    bool installed = false;
    auto install = [&](uint8_t vector) {
        CHECK(vector == I219MsiVector && !(cfg.read16(0x52) & MsiEnable));
        installed = true;
    };
    CHECK(!EnableMsi(cfg, 7, I219MsiVector, install) && !installed && cfg.writes == 0);
    cfg.bytes[6] = 1 << 4;
    cfg.bytes[0x34] = 0x50;
    cfg.bytes[0x50] = 0x05;
    cfg.bytes[0x51] = 0x50;
    CHECK(!EnableMsi(cfg, 7, I219MsiVector, install) && !installed && cfg.writes == 0);
    cfg.bytes[0x51] = 0;
    cfg.write16(0x52, MsiMultipleEnabled | MsiEnable);
    CHECK(EnableMsi(cfg, 7, I219MsiVector, install) == 0x50);
    CHECK(installed && cfg.read16(0x52) == MsiEnable);
    CHECK(cfg.read32(0x54) == 0xfee07000u && cfg.read16(0x58) == I219MsiVector);
    DisableMsi(cfg, 0x50);
    CHECK(!(cfg.read16(0x52) & MsiEnable));

    cfg.write16(0x52, MsiAddress64 | MsiVectorMask);
    cfg.write32(0x58, ~0u);
    cfg.write32(0x60, ~0u);
    CHECK(EnableMsi(cfg, 7, I219MsiVector, install) == 0x50);
    CHECK(cfg.read32(0x58) == 0 && cfg.read16(0x5c) == I219MsiVector);
    CHECK(cfg.read32(0x60) == ~1u);
    cfg.acceptEnable = false;
    CHECK(!EnableMsi(cfg, 7, I219MsiVector, install));

    cfg.bytes[0x51] = 0xfc;
    cfg.bytes[0xfc] = 0x11;
    cfg.write16(0xfe, 0x8000);
    const unsigned writes = cfg.writes;
    CHECK(!EnableMsi(cfg, 7, I219MsiVector, install) && cfg.writes == writes);
    cfg.bytes[0x34] = 0xfc;
    cfg.bytes[0xfc] = 0x05;
    CHECK(!EnableMsi(cfg, 7, I219MsiVector, install) && cfg.writes == writes);

    static_assert(I219RxRingSize * sizeof(RxDescriptor) == 65536);
    static_assert(Next<I219RxRingSize>(4094) == 4095);
    static_assert(Next<I219RxRingSize>(4095) == 0);
}

void ValidateBridgePath()
{
    g_device.bus = 0;
    CHECK(EnableUpstream() && configWrites == 0);
    g_device.bus = 3;
    CHECK(EnableUpstream() && configWrites == 2);
    CHECK(bridges[0].command == 0x106 && bridges[1].command == 0x006 && bridges[2].command == 0);
    CHECK(EnableUpstream() && configWrites == 2);

    g_device.bus = 6; // configuration numbers cover it, but the leaf is absent
    CHECK(!EnableUpstream() && configWrites == 2);
    g_device.bus = 3;
    bridges[2].secondary = 3; // two routes covering endpoint, no side effects
    CHECK(!EnableUpstream() && configWrites == 2);
    bridges[2].secondary = 8;

    bridges[1].command = 2;
    bridges[1].writable = false;
    CHECK(!EnableUpstream() && configWrites == 3 && bridges[1].command == 2);
}

void ValidateTransmit()
{
    uint8_t frame[64]{};
    CHECK(Submit(frame, 13, true) == -22);
    CHECK(Submit(frame, 1515, true) == -22);
    CHECK(Submit(nullptr, 64, true) == -22);

    for (uint16_t i = 0; i < RingSize - 1; ++i) {
        frame[0] = i;
        CHECK(Submit(frame, sizeof(frame), true) == 0);
        CHECK(tx[i].length == sizeof(frame));
        CHECK(tx[i].command == TxCommand && tx[i].status == 0);
        CHECK(txBuffers[i * BufferSize] == i);
    }
    CHECK(Read(Tdt) == RingSize - 1);
    frame[0] = 0xfe;
    CHECK(Submit(frame, sizeof(frame), true) == -11);
    tx[1].status = DescriptorDone; // head still owned by the device
    CHECK(Submit(frame, sizeof(frame), true) == -11);
    CHECK(txBuffers[0] == 0);

    tx[0].status = DescriptorDone;
    CHECK(Submit(frame, sizeof(frame), true) == 0);
    CHECK(Read(Tdt) == 0 && g_tx.head == 2);
    CHECK(Submit(frame, sizeof(frame), false) == 0);
    CHECK(Read(Tdt) == 1 && txBuffers[0] == 0xfe && !tx[0].status);
    CHECK(Submit(frame, sizeof(frame), true) == -11);

    SpinLockAcquire(&g_txLock);
    CHECK(Submit(frame, sizeof(frame), true) == -11);
    SpinLockRelease(&g_txLock);
}

void CompleteRx(uint16_t slot, uint8_t status, uint16_t length, uint8_t errors)
{
    rx[slot].status = status;
    rx[slot].length = length;
    rx[slot].errors = errors;
    rxBuffers[slot * BufferSize] = 0x34;
}

void ValidateReceive()
{
    const uint8_t complete = DescriptorDone | RxEndOfPacket;
    g_rx.next = RingSize - 1;
    CompleteRx(RingSize - 1, DescriptorDone, BufferSize, 0);
    Poll(&g_interface);
    CHECK(packets == 0 && g_rx.discarding && g_rx.next == 0);
    CHECK(Read(Rdt) == RingSize - 1 && !rx[RingSize - 1].status);

    CompleteRx(0, complete, 64, 0); // final fragment across polls and wrap
    CompleteRx(1, complete, 64, 1);
    CompleteRx(2, complete, MinFrameSize - 1, 0);
    CompleteRx(3, complete, MaxFrameSize + 1, 0);
    CompleteRx(4, complete, 64, 0);
    CompleteRx(5, RxEndOfPacket, 64, 0); // owned by device
    Poll(&g_interface);
    CHECK(packets == 1 && !g_rx.discarding && g_rx.next == 5);
    CHECK(Read(Rdt) == 4 && !rx[4].status && rx[5].status == RxEndOfPacket);
    Poll(&g_interface);
    CHECK(packets == 1 && Read(Rdt) == 4);
}
}

namespace brook {
void NetReceive(NetIf* nif, const void* frame, uint32_t length)
{
    CHECK(nif == &g_interface && length == 64);
    CHECK(static_cast<const uint8_t*>(frame)[0] == 0x34);
    ++packets;
    Poll(nif); // nested stack polling must not consume the same descriptor
}
void NetWakePollThread() { ++wakes; }
void NetWakeMsiPollThread(NetIf* intf) {
    CHECK(intf == &g_interface);
    ++msiWakes;
}
extern "C" void ApicSendEoi() { ++eois; }
uint64_t ApicTickCount() { return monotonic; }
extern "C" uint32_t PciConfigRead32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    const auto* b = LookupBridge(bus, device, function);
    if (!b) return ~0u;
    switch (offset) {
        case 0x00: return 0x244e8086; // PCI bridge function present
        case 0x08: return 0x06040000;
        case 0x0c: return 1u << 16;
        case 0x18: return b->bus | (uint32_t(b->secondary) << 8) | (uint32_t(b->subordinate) << 16);
        default: return 0;
    }
}
extern "C" uint16_t PciConfigRead16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    const auto* b = LookupBridge(bus, device, function);
    CHECK(b && offset == 4);
    return b ? b->command : 0xffffu;
}
extern "C" void PciConfigWrite16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value)
{
    auto* b = LookupBridge(bus, device, function);
    CHECK(b && offset == 4);
    ++configWrites;
    if (b && b->writable) b->command = value;
}
extern "C" void SerialPrintf(const char*, ...) {}
}

int main()
{
    g_regs = registers;
    g_rxDma.descriptors = VirtualAddress(reinterpret_cast<uint64_t>(rx));
    g_rxDma.packets = VirtualAddress(reinterpret_cast<uint64_t>(rxBuffers));
    g_txDma.descriptors = VirtualAddress(reinterpret_cast<uint64_t>(tx));
    g_txDma.packets = VirtualAddress(reinterpret_cast<uint64_t>(txBuffers));

    CHECK(Devices[0].id == 0x100e && !Devices[0].pcie);
    CHECK(Devices[1].id == 0x100f && !Devices[1].pcie);
    CHECK(Devices[2].id == 0x10d3 && Devices[2].pcie);
    ValidateMsi();
    ValidateBridgePath();
    ValidateTransmit();
    ValidateReceive();
    Write(Status, StatusLinkUp);
    Poll(&g_interface);
    CHECK(!g_linkUp); // rate-limited since the initial poll
    monotonic += 100;
    Poll(&g_interface);
    CHECK(g_linkUp);
    Write(Status, 0);
    monotonic += 100;
    Poll(&g_interface);
    CHECK(!g_linkUp);
    Interrupt(); // unrelated shared line
    Write(Icr, InterruptRx);
    Interrupt();
    Write(Icr, InterruptLink);
    Interrupt();
    CHECK(wakes == 2 && packets == 1);
    Write(Icr, 0);
    E1000eMsiInner();
    Write(Icr, InterruptTx);
    E1000eMsiInner();
    Write(Icr, InterruptReceive | InterruptLsc);
    E1000eMsiInner();
    CHECK(msiWakes == 2 && eois == 3);
    return failures != 0;
}
