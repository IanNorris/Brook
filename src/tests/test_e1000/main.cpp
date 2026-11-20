#include <cstdio>
#include "../../drivers/e1000/e1000_mod.cpp"

namespace {
unsigned failures = 0, packets = 0, wakes = 0;
uint32_t registers[MmioPages * PageSize / 4];
TxDescriptor tx[RingSize];
RxDescriptor rx[RingSize];
uint8_t txBuffers[RingSize * BufferSize], rxBuffers[RingSize * BufferSize];

#define CHECK(expr) do { \
    if (!(expr)) { std::fprintf(stderr, "e1000 line %d: %s\n", __LINE__, #expr); ++failures; } \
} while (0)

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
extern "C" void SerialPrintf(const char*, ...) {}
}

int main()
{
    g_regs = registers;
    g_rxDma.descriptors = VirtualAddress(reinterpret_cast<uint64_t>(rx));
    g_rxDma.packets = VirtualAddress(reinterpret_cast<uint64_t>(rxBuffers));
    g_txDma.descriptors = VirtualAddress(reinterpret_cast<uint64_t>(tx));
    g_txDma.packets = VirtualAddress(reinterpret_cast<uint64_t>(txBuffers));

    CHECK(DeviceName(0x100e) && DeviceName(0x100f));
    CHECK(!DeviceName(0xffff));
    ValidateTransmit();
    ValidateReceive();
    Interrupt(); // unrelated shared line
    Write(Icr, InterruptRx);
    Interrupt();
    Write(Icr, InterruptLink);
    Interrupt();
    CHECK(wakes == 2 && packets == 1);
    return failures != 0;
}
