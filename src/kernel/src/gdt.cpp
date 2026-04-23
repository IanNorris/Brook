#include "gdt.h"

// ---- Stacks allocated in BSS (zeroed by bootloader) ----

// Primary kernel stack — we switch to this at the very start of KernelMain.
// Grows downward; stack top = &g_kernelStack[sizeof(g_kernelStack)].
uint8_t g_kernelStack[65536];  // 64 KB

// Dedicated double-fault stack (IST1) — now per-CPU, see g_dfStacks.

// Exported for KernelMain to switch stacks.
void* g_kernelStackTop = static_cast<void*>(g_kernelStack + sizeof(g_kernelStack) - 16);

// ---- GDT storage ----
// Layout: [null, k-code, k-data, null(sysret anchor), u-data, u-code, TSS0(16B), TSS1(16B), ...]
// TSS takes two 8-byte GDT slots (a 64-bit system descriptor).
// We allocate space for up to GDT_MAX_CPUS TSS descriptors.
//
// Fixed entries: 6 × 8 = 48 bytes (0x00..0x2F)
// TSS entries: GDT_MAX_CPUS × 16 bytes starting at 0x30

static constexpr uint32_t GDT_FIXED_ENTRIES = 6;
static constexpr uint32_t GDT_TOTAL_SLOTS = GDT_FIXED_ENTRIES + GDT_MAX_CPUS * 2;

// Raw GDT as array of 8-byte entries.
static GdtEntry g_gdtRaw[GDT_TOTAL_SLOTS] __attribute__((aligned(16)));
static GdtDescriptor g_gdtDesc;

// Per-CPU TSS instances.
static Tss64 g_tssArray[GDT_MAX_CPUS];

// Per-CPU double-fault stacks (4KB each).
static uint8_t g_dfStacks[GDT_MAX_CPUS][4096] __attribute__((aligned(16)));

// Per-CPU NMI stacks (4KB each).
static uint8_t g_nmiStacks[GDT_MAX_CPUS][4096] __attribute__((aligned(16)));

// Track how many CPUs have been initialized.
static uint32_t g_cpuTssCount = 0;

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

static void SetTssDescriptorAt(uint32_t gdtSlotIndex, const Tss64* tss)
{
    auto* d = reinterpret_cast<TssDescriptor*>(&g_gdtRaw[gdtSlotIndex]);
    uint64_t base  = reinterpret_cast<uint64_t>(tss);
    uint32_t limit = static_cast<uint32_t>(sizeof(Tss64) - 1);

    d->limitLow      = static_cast<uint16_t>(limit & 0xFFFF);
    d->baseLow       = static_cast<uint16_t>(base & 0xFFFF);
    d->baseMid       = static_cast<uint8_t>((base >> 16) & 0xFF);
    d->access        = 0x89;  // Present | DPL=0 | Type=0x9 (64-bit TSS available)
    d->limitHighFlags = static_cast<uint8_t>((limit >> 16) & 0x0F);
    d->baseHigh      = static_cast<uint8_t>((base >> 24) & 0xFF);
    d->baseUpper     = static_cast<uint32_t>((base >> 32) & 0xFFFFFFFF);
    d->_reserved     = 0;
}

void GdtInit()
{
    // Set up the fixed segment descriptors.
    SetGdtEntry(g_gdtRaw[0], 0, 0, 0x00, 0x00);          // 0x00: null
    SetGdtEntry(g_gdtRaw[1], 0, 0xFFFF, 0x9A, 0x20);     // 0x08: Ring 0 64-bit code
    SetGdtEntry(g_gdtRaw[2], 0, 0xFFFF, 0x92, 0x00);     // 0x10: Ring 0 data
    SetGdtEntry(g_gdtRaw[3], 0, 0, 0x00, 0x00);           // 0x18: null (STAR anchor)
    SetGdtEntry(g_gdtRaw[4], 0, 0xFFFF, 0xF2, 0x00);     // 0x20: Ring 3 data
    SetGdtEntry(g_gdtRaw[5], 0, 0xFFFF, 0xFA, 0x20);     // 0x28: Ring 3 64-bit code

    // Populate BSP TSS (CPU 0): IST1 = double-fault stack, IST2 = NMI stack.
    void* dfTop = static_cast<void*>(g_dfStacks[0] + sizeof(g_dfStacks[0]) - 16);
    g_tssArray[0].ist[0]         = reinterpret_cast<uint64_t>(dfTop);
    void* nmiTop = static_cast<void*>(g_nmiStacks[0] + sizeof(g_nmiStacks[0]) - 16);
    g_tssArray[0].ist[1]         = reinterpret_cast<uint64_t>(nmiTop);
    g_tssArray[0].ioBitmapOffset = static_cast<uint16_t>(sizeof(Tss64));

    // TSS descriptor for CPU 0 starts at GDT slot 6 (byte offset 0x30).
    SetTssDescriptorAt(GDT_FIXED_ENTRIES, &g_tssArray[0]);
    g_cpuTssCount = 1;

    // GDT descriptor covers all possible entries (oversizing is harmless).
    g_gdtDesc.limit = static_cast<uint16_t>(sizeof(g_gdtRaw) - 1);
    g_gdtDesc.base  = reinterpret_cast<uint64_t>(&g_gdtRaw);

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

    // Load TSS — selector 0x30, RPL=0.
    __asm__ volatile("ltr %0" : : "r"(static_cast<uint16_t>(GDT_TSS)));
}

uint16_t GdtInitAp(uint32_t cpuIndex)
{
    if (cpuIndex >= GDT_MAX_CPUS) return 0;

    // Initialize the TSS for this AP.
    Tss64* tss = &g_tssArray[cpuIndex];
    __builtin_memset(tss, 0, sizeof(Tss64));

    void* dfTop = static_cast<void*>(g_dfStacks[cpuIndex] + sizeof(g_dfStacks[cpuIndex]) - 16);
    tss->ist[0]         = reinterpret_cast<uint64_t>(dfTop);
    void* nmiTop = static_cast<void*>(g_nmiStacks[cpuIndex] + sizeof(g_nmiStacks[cpuIndex]) - 16);
    tss->ist[1]         = reinterpret_cast<uint64_t>(nmiTop);
    tss->ioBitmapOffset = static_cast<uint16_t>(sizeof(Tss64));

    // TSS descriptor: CPU N uses GDT slots (6 + N*2) and (6 + N*2 + 1).
    uint32_t slotIndex = GDT_FIXED_ENTRIES + cpuIndex * 2;
    SetTssDescriptorAt(slotIndex, tss);

    // Selector = slot byte offset.
    uint16_t selector = static_cast<uint16_t>(slotIndex * sizeof(GdtEntry));
    if (cpuIndex >= g_cpuTssCount)
        g_cpuTssCount = cpuIndex + 1;

    return selector;
}

void GdtLoadOnAp(uint16_t tssSelector)
{
    // Reload GDT (same table, shared across CPUs).
    __asm__ volatile("lgdt %0" : : "m"(g_gdtDesc));

    // Reload segment registers.
    __asm__ volatile(
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        ::: "ax"
    );

    // Reload CS.
    __asm__ volatile(
        "pushq $0x08\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        ::: "rax", "memory"
    );

    // Load this CPU's TSS.
    __asm__ volatile("ltr %0" : : "r"(tssSelector));
}

void GdtSetTssRsp0(uint64_t stackTop)
{
    g_tssArray[0].rsp[0] = stackTop;
}

void GdtSetTssRsp0ForCpu(uint32_t cpuIndex, uint64_t stackTop)
{
    if (cpuIndex < GDT_MAX_CPUS)
        g_tssArray[cpuIndex].rsp[0] = stackTop;
}

Tss64* GdtGetTss(uint32_t cpuIndex)
{
    if (cpuIndex >= GDT_MAX_CPUS) return nullptr;
    return &g_tssArray[cpuIndex];
}
