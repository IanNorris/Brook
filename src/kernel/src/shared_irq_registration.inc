// Actual IDT registration/dispatch body shared with hosted tests.
// Registration ownership is counted per (line, callback), but a callback is
// dispatched once: e.g. virtio-blk's callback scans every owned device itself.
// These changes do not provide module-unload quiescence for executing callbacks.
static_assert(sizeof(g_sharedIrqStubs)/sizeof(g_sharedIrqStubs[0]) == MAX_SHARED_IRQS,
              "shared IRQ capacity must match actual stubs");

static void SharedIrqDispatch(unsigned slot)
{
    SharedIrqEntry& e = g_sharedIrqs[slot];
    for (int j = 0; j < e.count; ++j) e.handlers[j]();
    ApicSendEoi();
}

// Direct installs must not consume this pool or overwrite an owned stub.
static bool SharedIrqMayInstall(uint8_t vector, void* handler)
{
    if (!SharedIrqVectorAllowed(vector)) return true;
    for (int i = 0; i < g_sharedIrqCount; ++i)
        if (g_sharedIrqs[i].vector == vector)
            return handler == reinterpret_cast<void*>(g_sharedIrqStubs[i]);
    return false;
}

// Boot-only preflight for optional providers. Caller must serialize with boot
// registration; this is not a runtime registration/quiescence mechanism.
bool IoApicHandlerCapacity(uint8_t irq, void* handler)
{
    if (!handler || irq==255) return false;
    if (auto* entry=FindSharedIrq(irq)) {
        for (int i=0;i<entry->count;++i)
            if (entry->handlers[i]==reinterpret_cast<IrqHandlerFn>(handler))
                return entry->references[i]!=~uint32_t(0);
        return entry->count<MAX_IRQ_CHAIN;
    }
    if (g_sharedIrqCount==MAX_SHARED_IRQS) return false;
    for (unsigned vector=48;vector<=63;++vector) {
        if (!SharedIrqVectorAllowed(static_cast<uint8_t>(vector))) continue;
        bool free=true;
        for (int i=0;i<g_sharedIrqCount;++i) if(g_sharedIrqs[i].vector==vector)free=false;
        if(free)return true;
    }
    return false;
}

uint8_t IoApicRegisterHandler(uint8_t irq, uint8_t preferredVector, void* handler)
{
    // The legacy ABI returns a vector, and existing drivers do not check a
    // failure sentinel. Fail explicitly before publication rather than return
    // a success-looking vector with no installed handler.
    if (!handler || irq == 255) KernelPanic("invalid shared IRQ registration");
    auto fn = reinterpret_cast<IrqHandlerFn>(handler);
    SharedIrqEntry* existing = FindSharedIrq(irq);
    if (existing)
    {
        for (int i = 0; i < existing->count; ++i) {
            if (existing->handlers[i] != fn) continue;
            if (existing->references[i] == ~uint32_t(0))
                KernelPanic("shared IRQ registration reference overflow");
            ++existing->references[i];
            return existing->vector;
        }
        if (existing->count == MAX_IRQ_CHAIN)
            KernelPanic("shared IRQ %u handler chain full", irq);
        const bool wasEmpty = existing->count == 0;
        existing->handlers[existing->count] = fn;
        existing->references[existing->count] = 1;
        ++existing->count;
        if (wasEmpty) IoApicUnmaskIrq(irq, existing->vector);
        SerialPrintf("IRQ: chained handler on IRQ %u (vector %u, %u handlers)\n",
                     irq, existing->vector, existing->count);
        return existing->vector;
    }
    if (g_sharedIrqCount == MAX_SHARED_IRQS)
        KernelPanic("shared IRQ dispatch stubs exhausted");
    auto freeVector = [](uint8_t vector) {
        if (!SharedIrqVectorAllowed(vector)) return false;
        for (int i = 0; i < g_sharedIrqCount; ++i)
            if (g_sharedIrqs[i].vector == vector) return false;
        return true;
    };
    uint8_t vector = preferredVector;
    if (!freeVector(vector)) {
        vector = 0;
        for (unsigned candidate = 48; candidate <= 63; ++candidate)
            if (freeVector(static_cast<uint8_t>(candidate))) {
                vector = static_cast<uint8_t>(candidate);
                break;
            }
        if (!vector) KernelPanic("shared IRQ vectors exhausted");
    }
    const int slot = g_sharedIrqCount;
    SharedIrqEntry& entry = g_sharedIrqs[slot];
    entry.irq = irq;
    entry.vector = vector;
    entry.handlers[0] = fn;
    entry.references[0] = 1;
    entry.count = 1;
    ++g_sharedIrqCount;
    IdtInstallHandler(vector, reinterpret_cast<void*>(g_sharedIrqStubs[slot]));
    IoApicUnmaskIrq(irq, vector);
    return vector;
}

void IoApicUnregisterHandler(uint8_t irq, void* handler)
{
    SharedIrqEntry* entry = FindSharedIrq(irq);
    if (!entry) return;
    auto fn = reinterpret_cast<IrqHandlerFn>(handler);
    for (int i = 0; i < entry->count; ++i) {
        if (entry->handlers[i] != fn) continue;
        if (entry->references[i] > 1) { --entry->references[i]; return; }
        for (int j = i; j < entry->count - 1; ++j) {
            entry->handlers[j] = entry->handlers[j + 1];
            entry->references[j] = entry->references[j + 1];
        }
        --entry->count;
        entry->handlers[entry->count] = nullptr;
        entry->references[entry->count] = 0;
        if (!entry->count) IoApicMaskIrq(irq);
        return;
    }
}
