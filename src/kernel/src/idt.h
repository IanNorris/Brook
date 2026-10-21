#pragma once
#include <stdint.h>
#include "boot_protocol/boot_protocol.h"

struct IdtEntry {
    uint16_t offsetLow;
    uint16_t selector;   // GDT_KERNEL_CODE = 0x08
    uint8_t  ist;        // 0 = no IST
    uint8_t  typeAttr;   // 0x8E = Present | DPL=0 | 64-bit interrupt gate
    uint16_t offsetMid;
    uint32_t offsetHigh;
    uint32_t _reserved;
} __attribute__((packed));

struct IdtDescriptor {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct InterruptFrame {
    uint64_t ip;
    uint64_t cs;
    uint64_t flags;
    uint64_t sp;
    uint64_t ss;
};

// exception_stubs.S: 15 saved GPRs, real/synthetic error code, hardware frame.
struct FullExceptionFrame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t errorCode, rip, cs, rflags, rsp, ss;
};
static_assert(sizeof(FullExceptionFrame)==168);
static_assert(__builtin_offsetof(FullExceptionFrame,errorCode)==120);
static_assert(__builtin_offsetof(FullExceptionFrame,rip)==128);
static_assert(__builtin_offsetof(FullExceptionFrame,rsp)==152);

// Set up 32 exception handlers and 16 IRQ stubs, then load the IDT via lidt.
void IdtInit(brook::Framebuffer* fb);

// Install a handler after IdtInit(). Vectors 48-63 except 49 belong to the
// shared IOAPIC dispatcher; direct installs may not consume/replace that pool.
// handler must be a function with C linkage and standard interrupt ABI
// (__attribute__((interrupt)) or raw ISR that saves all registers).
extern "C" void IdtInstallHandler(uint8_t vector, void* handler);

// Register a plain handler for an IOAPIC IRQ with shared-IRQ chaining.
// handler must be a plain void(*)(void) function — NOT __attribute__((interrupt)),
// and must NOT call ApicSendEoi(). The dispatch stub handles both.
// Returns the actual vector assigned (preferences outside the reserved pool or
// already owned by another line select a free vector). Repeated (IRQ,handler)
// registrations count ownership but dispatch once; pair each with unregister.
// Invalid/exhausted kernel configurations panic, never return a fake vector.
extern "C" uint8_t IoApicRegisterHandler(uint8_t irq, uint8_t preferredVector, void* handler);

// Drop one registration. The last reference removes the handler; an empty
// chain masks its line. Callback/unload quiescence requires external ownership.
extern "C" void IoApicUnregisterHandler(uint8_t irq, void* handler);

// Optional boot-only provider preflight; caller serializes boot registration.
bool IoApicHandlerCapacity(uint8_t irq, void* handler);
