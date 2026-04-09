#pragma once

#include <stdint.h>

namespace brook
{

constexpr uint32_t BootProtocolMagic   = 0xB007B007u;
constexpr uint32_t BootProtocolVersion = 1u;

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
};

struct MemoryDescriptor
{
    uint64_t   physicalStart;  // Physical base address, 4KB aligned
    uint64_t   pageCount;      // Size in 4KB pages
    MemoryType type;
    uint32_t   _reserved;
};

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
};

} // namespace brook
