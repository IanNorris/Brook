#include "gdt.h"

// ---- Stacks allocated in BSS (zeroed by bootloader) ----

// Primary kernel stack — we switch to this at the very start of KernelMain.
// Grows downward; stack top = &g_kernelStack[sizeof(g_kernelStack)].
static uint8_t g_kernelStack[32768];  // 32 KB

// Dedicated double-fault stack (IST1).  Used exclusively by the #DF handler
// so it's valid even if the primary stack is corrupt.
static uint8_t g_dfStack[4096];       // 4 KB

// Exported for KernelMain to switch stacks.
void* g_kernelStackTop = static_cast<void*>(g_kernelStack + sizeof(g_kernelStack) - 16);

// ---- GDT storage ----
// The GDT must be contiguous: [null, k-code, k-data, u-code, u-data, TSS(16B)].
// TSS takes two 8-byte GDT slots (a 64-bit system descriptor).
struct GdtTable {
    GdtEntry     null_entry;
    GdtEntry     kernelCode;
    GdtEntry     kernelData;
    GdtEntry     userCode;
    GdtEntry     userData;
    TssDescriptor tss;         // 16 bytes (occupies slots 5+6)
} __attribute__((packed));

static GdtTable      g_gdtTable;
static GdtDescriptor g_gdtDesc;
static Tss64         g_tss;

static void SetGdtEntry(GdtEntry& e, uint32_t base, uint32_t limit,
                        uint8_t access, uint8_t granularity)
{
    e.limitLow    = static_cast<uint16_t>(limit & 0xFFFF);
    e.baseLow     = static_cast<uint16_t>(base & 0xFFFF);
    e.baseMiddle  = static_cast<uint8_t>((base >> 16) & 0xFF);
    e.access      = access;
    e.granularity = static_cast<uint8_t>(granularity | ((limit >> 16) & 0x0F));
    e.baseHigh    = static_cast<uint8_t>((base >> 24) & 0xFF);
}

static void SetTssDescriptor(TssDescriptor& d, const Tss64* tss)
{
    uint64_t base  = reinterpret_cast<uint64_t>(tss);
    uint32_t limit = static_cast<uint32_t>(sizeof(Tss64) - 1);

    d.limitLow      = static_cast<uint16_t>(limit & 0xFFFF);
    d.baseLow       = static_cast<uint16_t>(base & 0xFFFF);
    d.baseMid       = static_cast<uint8_t>((base >> 16) & 0xFF);
    d.access        = 0x89;  // Present | DPL=0 | Type=0x9 (64-bit TSS available)
    d.limitHighFlags = static_cast<uint8_t>((limit >> 16) & 0x0F);
    d.baseHigh      = static_cast<uint8_t>((base >> 24) & 0xFF);
    d.baseUpper     = static_cast<uint32_t>((base >> 32) & 0xFFFFFFFF);
    d._reserved     = 0;
}

void GdtInit()
{
    // Set up the 5 regular segment descriptors.
    SetGdtEntry(g_gdtTable.null_entry, 0, 0, 0x00, 0x00);
    SetGdtEntry(g_gdtTable.kernelCode, 0, 0xFFFF, 0x9A, 0x20);   // Ring 0 64-bit code
    SetGdtEntry(g_gdtTable.kernelData, 0, 0xFFFF, 0x92, 0x00);   // Ring 0 data
    SetGdtEntry(g_gdtTable.userCode,   0, 0xFFFF, 0xFA, 0x20);   // Ring 3 64-bit code
    SetGdtEntry(g_gdtTable.userData,   0, 0xFFFF, 0xF2, 0x00);   // Ring 3 data

    // Populate TSS: only IST1 (double-fault emergency stack).
    // ist[] is 0-indexed: ist[0] = IST1, ist[1] = IST2, etc.
    void* dfTop = static_cast<void*>(g_dfStack + sizeof(g_dfStack) - 16);
    g_tss.ist[0]         = reinterpret_cast<uint64_t>(dfTop);
    g_tss.ioBitmapOffset = static_cast<uint16_t>(sizeof(Tss64));

    SetTssDescriptor(g_gdtTable.tss, &g_tss);

    // GDT descriptor: base = start of table, limit = size - 1.
    g_gdtDesc.limit = static_cast<uint16_t>(sizeof(GdtTable) - 1);
    g_gdtDesc.base  = reinterpret_cast<uint64_t>(&g_gdtTable);

    __asm__ volatile("lgdt %0" : : "m"(g_gdtDesc));

    // Reload data segment registers.
    __asm__ volatile(
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        ::: "ax"
    );

    // Far return to reload CS = 0x08.
    __asm__ volatile(
        "pushq $0x08\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        ::: "rax", "memory"
    );

    // Load TSS — selector 0x28, RPL=0.
    __asm__ volatile("ltr %0" : : "r"(static_cast<uint16_t>(GDT_TSS)));
}

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

static void SetTssDescriptor(const Tss64* tss)
{
    uint64_t base  = reinterpret_cast<uint64_t>(tss);
    uint32_t limit = static_cast<uint32_t>(sizeof(Tss64) - 1);

    g_tssDesc.limitLow      = static_cast<uint16_t>(limit & 0xFFFF);
    g_tssDesc.baseLow       = static_cast<uint16_t>(base & 0xFFFF);
    g_tssDesc.baseMid       = static_cast<uint8_t>((base >> 16) & 0xFF);
    g_tssDesc.access        = 0x89;  // Present | DPL=0 | Type=0x9 (64-bit TSS available)
    g_tssDesc.limitHighFlags = static_cast<uint8_t>((limit >> 16) & 0x0F);  // no granularity bit
    g_tssDesc.baseHigh      = static_cast<uint8_t>((base >> 24) & 0xFF);
    g_tssDesc.baseUpper     = static_cast<uint32_t>((base >> 32) & 0xFFFFFFFF);
    g_tssDesc._reserved     = 0;
}

void GdtInit()
{
    // Set up the 5 regular GDT entries.
    SetGdtEntry(0, 0, 0, 0x00, 0x00);          // Null
    SetGdtEntry(1, 0, 0xFFFF, 0x9A, 0x20);     // Ring 0 64-bit code (L-bit)
    SetGdtEntry(2, 0, 0xFFFF, 0x92, 0x00);     // Ring 0 data
    SetGdtEntry(3, 0, 0xFFFF, 0xFA, 0x20);     // Ring 3 64-bit code
    SetGdtEntry(4, 0, 0xFFFF, 0xF2, 0x00);     // Ring 3 data

    // Populate TSS: only IST1 matters for now (double-fault emergency stack).
    void* dfTop = static_cast<void*>(g_dfStack + sizeof(g_dfStack) - 16);
    g_tss.ist[0]         = reinterpret_cast<uint64_t>(dfTop);  // IST1 (ist[] is 0-indexed, IST1=ist[0])
    g_tss.ioBitmapOffset = sizeof(Tss64);  // disable I/O permission bitmap

    SetTssDescriptor(&g_tss);

    // Point GDT descriptor at the contiguous g_gdt + g_tssDesc region.
    // sizeof(g_gdt) = 5 * 8 = 40 = 0x28; sizeof(TssDescriptor) = 16 = 0x10
    // Total GDT size = 40 + 16 = 56 bytes; limit = 55.
    g_gdtDesc.limit = static_cast<uint16_t>(sizeof(g_gdt) + sizeof(g_tssDesc) - 1);
    g_gdtDesc.base  = reinterpret_cast<uint64_t>(&g_gdt);

    __asm__ volatile("lgdt %0" : : "m"(g_gdtDesc));

    // Reload data segment registers.
    __asm__ volatile(
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        ::: "ax"
    );

    // Far return to reload CS = 0x08.
    __asm__ volatile(
        "pushq $0x08\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        ::: "rax", "memory"
    );

    // Load TSS — selector 0x28, RPL=0.
    __asm__ volatile("ltr %0" : : "r"(static_cast<uint16_t>(GDT_TSS)));
}
