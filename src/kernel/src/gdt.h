#pragma once
#include <stdint.h>

// GDT selector constants (byte offsets into the GDT table).
static constexpr uint16_t GDT_KERNEL_CODE = 0x08;
static constexpr uint16_t GDT_KERNEL_DATA = 0x10;
static constexpr uint16_t GDT_USER_CODE   = 0x18 | 3;
static constexpr uint16_t GDT_USER_DATA   = 0x20 | 3;

struct GdtEntry {
    uint16_t limitLow;
    uint16_t baseLow;
    uint8_t  baseMiddle;
    uint8_t  access;
    uint8_t  granularity;  // high nibble: flags, low nibble: limit high
    uint8_t  baseHigh;
} __attribute__((packed));

struct GdtDescriptor {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// Populate the GDT, load it via lgdt, and reload all segment registers.
void GdtInit();
