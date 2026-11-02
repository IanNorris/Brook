// Exercise the actual xHCI ECM transmit path with fake MMIO and DMA memory.
#include "../../drivers/xhci/xhci_mod.cpp"
#include <cassert>

int main() {
    XhciController controller{};XhciDevice device{};UsbEcmState net{};
    Trb ring[XFER_RING_SIZE]{};uint8_t dma[ECM_TX_SLOTS*2048]{};
    uint32_t bells[4]{};uint8_t frame[128];memset(frame,0x5a,sizeof(frame));
    controller.dbBase=reinterpret_cast<volatile uint8_t*>(bells);
    device.slotId=1;device.bulkOutRing=ring;device.bulkOutRingPhys=0x1000;
    device.bulkOutCycle=true;device.bulkOutDci=3;device.bulkOutMaxPacket=64;
    net.controller=&controller;net.device=&device;net.tx=dma;net.txPhysical=0x20000;
    net.txDepth=2;net.interface.driverPriv=&net;
    auto& nif=net.interface;
    // With the event lock held, immediate submission cannot mutate any
    // buffer, slot, ring index or doorbell, and must not enqueue a ticket.
    const auto held=IrqSpinLockAcquire(&g_eventRingLock);
    const auto next=g_eventRingLock.next,serving=g_eventRingLock.serving;
    assert(XhciEcmTryTransmit(&nif,frame,63)==-11);
    assert(g_eventRingLock.next==next && g_eventRingLock.serving==serving);
    assert(!device.bulkOutEnqueue && !net.sends.slots[0].active && !bells[1]);
    for(auto b:dma)assert(b==0);
    for(const auto& entry:ring)assert(!entry.param && !entry.status && !entry.control);
    IrqSpinLockRelease(&g_eventRingLock,held);
    assert(XhciEcmTryTransmit(&nif,nullptr,63)==-22);
    assert(XhciEcmTryTransmit(&nif,frame,13)==-22);
    assert(XhciEcmTryTransmit(&nif,frame,1515)==-22);
    net.failed=true;assert(XhciEcmTryTransmit(&nif,frame,63)==-5);net.failed=false;
    controller.completionFailed=true;assert(XhciEcmTryTransmit(&nif,frame,63)==-5);controller.completionFailed=false;
    // Normal submission owns its DMA copy until completion.
    assert(XhciEcmTryTransmit(&nif,frame,63)==0);
    assert(!memcmp(dma,frame,63) && net.sends.slots[0].active && bells[1]==3);
    const auto first=net.sends.slots[0].first;
    assert(net.sends.Complete(first,1,0)==UsbEcmTxSlots::Result::Finished);
    // An exact USB packet uses two TDs even when the ring wraps.
    device.bulkOutEnqueue=XFER_RING_SIZE-2;device.bulkOutCycle=true;bells[1]=0;
    assert(XhciEcmTryTransmit(&nif,frame,64)==0);
    auto slot=net.sends.slots[0];assert(slot.active && slot.first!=slot.final && bells[1]==3);
    assert(device.bulkOutEnqueue==1 && !device.bulkOutCycle);
    assert(net.sends.Complete(slot.first,1,0)==UsbEcmTxSlots::Result::Intermediate);
    assert(net.sends.slots[0].active);
    assert(net.sends.Complete(slot.final,1,0)==UsbEcmTxSlots::Result::Finished);
    // A full queue rejects without advancing the ring or writing a doorbell.
    net.sends.Reserve(0,10,10);net.sends.Reserve(1,20,20);bells[1]=0;
    const auto pos=device.bulkOutEnqueue;Trb savedRing[XFER_RING_SIZE];uint8_t savedDma[sizeof(dma)];
    memcpy(savedRing,ring,sizeof(ring));memcpy(savedDma,dma,sizeof(dma));
    assert(XhciEcmTryTransmit(&nif,frame,63)==-11);
    assert(device.bulkOutEnqueue==pos && bells[1]==0 && !memcmp(savedRing,ring,sizeof(ring)) && !memcmp(savedDma,dma,sizeof(dma)));
}
