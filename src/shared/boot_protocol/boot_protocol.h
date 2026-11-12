#pragma once

#include <stdint.h>

namespace brook
{

constexpr uint32_t BootProtocolMagic   = 0xB007B007u;
constexpr uint32_t BootProtocolVersion = 5u;

// Our own memory type enum - does NOT use UEFI types (kernel has no UEFI dependency)
enum class MemoryType : uint32_t
{
    Free,             // Available for general use
    Reserved,         // Firmware reserved, do not touch
    AcpiReclaimable,  // ACPI tables - can be freed after use
    AcpiNvs,          // ACPI non-volatile storage - must preserve
    Mmio,             // Memory-mapped I/O region
    Unusable,         // Defective/unusable memory
    Kernel,           // Kernel image
    KernelStack,      // Initial kernel stack
    BootData,         // Boot protocol struct + memory map (free after consuming)
    Framebuffer,      // Linear framebuffer
    BootloaderCode,   // Bootloader code (can be freed after jumping to kernel)
    Initrd,           // Initial ramdisk loaded by bootloader (freeable after use)
};

struct MemoryDescriptor
{
    uint64_t   physicalStart;  // Physical base address, 4KB aligned
    uint64_t   pageCount;      // Size in 4KB pages
    MemoryType type;
    // Optional extension: zero from older loaders. Runtime descriptors carry
    // UEFI's low attribute bits and the markers below; no service is enabled
    // merely because these flags are present.
    uint32_t   _reserved;
};

constexpr uint32_t BootMemoryEfiRuntime = 1u << 31;
constexpr uint32_t BootMemoryEfiRuntimeCode = 1u << 30;
constexpr uint32_t BootMemoryEfiRuntimeData = 1u << 29;
constexpr uint32_t BootMemoryEfiAttributesMask = (1u << 28) - 1;

// Runtime memory must never enter the page allocator even if a firmware map
// reports an unexpectedly reclaimable type. MMIO keeps its classification.
constexpr MemoryDescriptor BootMemoryDescriptor(uint64_t start, uint64_t pages,
    MemoryType type, uint64_t efiAttributes, bool runtimeCode, bool runtimeData)
{
    constexpr uint64_t efiRuntime = 1ull << 63;
    if (!(efiAttributes & efiRuntime)) return {start, pages, type, 0};
    const uint32_t flags = BootMemoryEfiRuntime |
        (runtimeCode ? BootMemoryEfiRuntimeCode : 0) |
        (runtimeData ? BootMemoryEfiRuntimeData : 0) |
        (static_cast<uint32_t>(efiAttributes) & BootMemoryEfiAttributesMask);
    return {start, pages, type == MemoryType::Mmio ? type : MemoryType::Reserved, flags};
}

enum class PixelFormat : uint32_t
{
    Bgr8,     // Blue[7:0] Green[15:8] Red[23:16] X[31:24] - most common
    Rgb8,     // Red[7:0]  Green[15:8] Blue[23:16] X[31:24]
    Bitmask,  // Described by bitmask fields below
};

struct PixelBitmask
{
    uint32_t red;
    uint32_t green;
    uint32_t blue;
    uint32_t reserved;
};

struct Framebuffer
{
    uint64_t     physicalBase;  // Physical address of first pixel
    uint32_t     width;         // Pixels per row
    uint32_t     height;        // Number of rows
    uint32_t     stride;        // Bytes per row (stride >= width * 4)
    PixelFormat  format;
    PixelBitmask bitmask;       // Only valid if format == Bitmask
};

struct AcpiInfo
{
    uint64_t rsdpPhysical;      // Physical address of ACPI 2.0 RSDP
};

struct BootProtocol
{
    uint32_t magic;              // Must equal BootProtocolMagic
    uint32_t version;            // Must equal BootProtocolVersion

    Framebuffer framebuffer;
    AcpiInfo    acpi;

    MemoryDescriptor* memoryMap;   // Array of MemoryDescriptor, in physical address order
    uint32_t          memoryMapCount;
    uint32_t          _reserved;

    uint64_t kernelPhysBase;    // Physical address of the loaded kernel image
    uint64_t kernelPhysPages;   // Size of kernel image in 4KB pages (includes BSS)

    uint64_t initrdPhysBase;    // Physical address of initrd image (0 if none)
    uint64_t initrdSize;        // Size of initrd image in bytes (0 if none)

    // Version 3: physical ResetSystem entrypoint, or zero if the loader could
    // not validate that it lies in firmware runtime code. A version-2 handoff
    // ends at initrdSize; consumers must check version before reading this.
    uint64_t efiResetSystemPhysical;
    // Version 4: fingerprint of firmware's actual runtime page PAT indices
    // before changing CR3, plus the PAT MSR and indices used in that walk.
    uint64_t efiRuntimeCacheFingerprint;
    uint64_t efiRuntimePat;
    uint32_t efiRuntimePatSlots;
    uint32_t efiRuntimeCacheValid;
    // Version 5: fingerprint of the boot CPU's firmware MTRR registers.
    uint64_t efiRuntimeMtrrFingerprint;
    uint32_t efiRuntimeMtrrValid;
    uint32_t efiRuntimeMtrrReserved;
};

constexpr bool BootEfiRuntimeCodeContains(const MemoryDescriptor* map, uint32_t count, uint64_t address)
{
    if (!map || !address || count > 4096) return false;
    for (uint32_t i=0; i<count; ++i) {
        const auto& d=map[i];
        if ((d._reserved & (BootMemoryEfiRuntime|BootMemoryEfiRuntimeCode)) !=
            (BootMemoryEfiRuntime|BootMemoryEfiRuntimeCode)) continue;
        if (!d.pageCount || d.pageCount > (~uint64_t{0}-d.physicalStart)/4096) continue;
        if (address >= d.physicalStart && address-d.physicalStart < d.pageCount*4096) return true;
    }
    return false;
}

} // namespace brook
