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

// Set up 32 exception handlers and 16 IRQ stubs, then load the IDT via lidt.
void IdtInit(brook::Framebuffer* fb);

// Install a handler for any vector 0-255.  Safe to call after IdtInit().
// handler must be a function with C linkage and standard interrupt ABI
// (__attribute__((interrupt)) or raw ISR that saves all registers).
void IdtInstallHandler(uint8_t vector, void* handler);
