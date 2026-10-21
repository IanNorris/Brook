#pragma once
#include <stdint.h>

// Private storage shared by IDT registration and its actual-body host tests.
// Handlers registered here must be plain functions (NOT __attribute__((interrupt))).
// They must NOT call ApicSendEoi() — the dispatch stub handles that.
using IrqHandlerFn = void (*)();

static constexpr int MAX_IRQ_CHAIN = 4;
// This INTx pool is reserved for shared dispatch. Vector 49 remains available
// for xHCI MSI-X. Keep fallback vectors in the same priority class as 50.
static constexpr bool SharedIrqVectorAllowed(uint8_t vector)
{
    return vector >= 48 && vector <= 63 && vector != 49;
}
static constexpr int MAX_SHARED_IRQS = 4; // exactly four compiled dispatch stubs

struct SharedIrqEntry {
    uint8_t      vector;
    uint8_t      irq;
    uint8_t      count;
    IrqHandlerFn handlers[MAX_IRQ_CHAIN];
    uint32_t references[MAX_IRQ_CHAIN];
};

static SharedIrqEntry g_sharedIrqs[MAX_SHARED_IRQS];
static int g_sharedIrqCount = 0;

static SharedIrqEntry* FindSharedIrq(uint8_t irq)
{
    for (int i = 0; i < g_sharedIrqCount; i++)
        if (g_sharedIrqs[i].irq == irq) return &g_sharedIrqs[i];
    return nullptr;
}

