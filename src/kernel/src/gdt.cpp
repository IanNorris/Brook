#include "gdt.h"

static GdtEntry  g_gdt[5];
static GdtDescriptor g_gdtDesc;

static void SetGdtEntry(int index, uint32_t base, uint32_t limit,
                        uint8_t access, uint8_t granularity)
{
    g_gdt[index].limitLow    = static_cast<uint16_t>(limit & 0xFFFF);
    g_gdt[index].baseLow     = static_cast<uint16_t>(base & 0xFFFF);
    g_gdt[index].baseMiddle  = static_cast<uint8_t>((base >> 16) & 0xFF);
    g_gdt[index].access      = access;
    g_gdt[index].granularity = static_cast<uint8_t>(granularity | ((limit >> 16) & 0x0F));
    g_gdt[index].baseHigh    = static_cast<uint8_t>((base >> 24) & 0xFF);
}

void GdtInit()
{
    // 0x00: Null descriptor
    SetGdtEntry(0, 0, 0, 0x00, 0x00);
    // 0x08: Ring 0 64-bit code (L-bit set via granularity=0x20)
    SetGdtEntry(1, 0, 0xFFFF, 0x9A, 0x20);
    // 0x10: Ring 0 data
    SetGdtEntry(2, 0, 0xFFFF, 0x92, 0x00);
    // 0x18: Ring 3 64-bit code
    SetGdtEntry(3, 0, 0xFFFF, 0xFA, 0x20);
    // 0x20: Ring 3 data
    SetGdtEntry(4, 0, 0xFFFF, 0xF2, 0x00);

    g_gdtDesc.limit = static_cast<uint16_t>(sizeof(g_gdt) - 1);
    g_gdtDesc.base  = reinterpret_cast<uint64_t>(&g_gdt);

    // Load the GDT
    __asm__ volatile("lgdt %0" : : "m"(g_gdtDesc));

    // Reload data segment registers
    __asm__ volatile(
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        ::: "ax"
    );

    // Far return trick to reload CS with selector 0x08
    __asm__ volatile(
        "pushq $0x08\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        ::: "rax", "memory"
    );
}
