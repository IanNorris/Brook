// Actual IOAPIC route writes, also exercised against host register fixtures.
bool IoApicSelectBootLevelPin(uint8_t pin)
{
    return g_ioApicVirt && pin<g_ioApicEntries && g_ioApicRoutePolicy.SelectBootLevel(pin);
}

void IoApicUnmaskIrq(uint8_t irq, uint8_t vector)
{
    const uint32_t entry=irq;
    // A repeated unmask keeps the original destination and all unrelated low
    // bits. The initial entry is masked/zeroed by IoApicInit as before.
    if(!g_ioApicRoutePolicy.Published(irq))
        IoApicWrite(static_cast<uint8_t>(0x11+2*entry),uint32_t(ApicGetId())<<24);
    const uint8_t reg=static_cast<uint8_t>(0x10+2*entry);
    IoApicWrite(reg,g_ioApicRoutePolicy.UnmaskLow(irq,IoApicRead(reg),vector));
}

void IoApicMaskIrq(uint8_t irq)
{
    const uint8_t reg=static_cast<uint8_t>(0x10+2*uint32_t(irq));
    IoApicWrite(reg,IoApicRead(reg)|0x00010000u);
}
