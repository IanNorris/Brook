#pragma once
#include <stdint.h>

// GDT selector constants (byte offsets into the GDT table).
// Layout is chosen so that SYSCALL/SYSRET work with the STAR MSR:
//   STAR[47:32] = 0x08 → SYSCALL loads CS=0x08 (kernel code), SS=0x10 (kernel data)
//   STAR[63:48] = 0x18 → SYSRET  loads SS=0x20 (user data),   CS=0x28 (user code)
// Index 0: null, 1: kernel code, 2: kernel data,
//       3: null (SYSRET anchor), 4: user data, 5: user code, 6-7: TSS
static constexpr uint16_t GDT_KERNEL_CODE = 0x08;
static constexpr uint16_t GDT_KERNEL_DATA = 0x10;
static constexpr uint16_t GDT_USER_DATA   = 0x20 | 3;
static constexpr uint16_t GDT_USER_CODE   = 0x28 | 3;
static constexpr uint16_t GDT_TSS         = 0x30;   // TSS selector (RPL=0)

// STAR MSR fields.
static constexpr uint16_t GDT_STAR_KERNEL = 0x08;  // STAR[47:32]
static constexpr uint16_t GDT_STAR_USER   = 0x18;  // STAR[63:48]

// IST slots in the TSS (1-indexed; 0 = no IST).
static constexpr uint8_t IST_DOUBLE_FAULT = 1;  // IST1 → dedicated double-fault stack
static constexpr uint8_t IST_NMI         = 2;  // IST2 → dedicated NMI stack

struct GdtEntry {
    uint16_t limitLow;
    uint16_t baseLow;
    uint8_t  baseMiddle;
    uint8_t  access;
    uint8_t  granularity;  // high nibble: flags, low nibble: limit high
    uint8_t  baseHigh;
} __attribute__((packed));

// A 64-bit TSS descriptor occupies TWO consecutive 8-byte GDT slots.
struct TssDescriptor {
    uint16_t limitLow;
    uint16_t baseLow;
    uint8_t  baseMid;
    uint8_t  access;       // 0x89 = Present | Type=TSS available
    uint8_t  limitHighFlags;
    uint8_t  baseHigh;
    uint32_t baseUpper;
    uint32_t _reserved;
} __attribute__((packed));

struct GdtDescriptor {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// 64-bit Task State Segment.
struct Tss64 {
    uint32_t _reserved0;
    uint64_t rsp[3];         // RSP0–RSP2: ring-transition stacks
    uint64_t _reserved1;
    uint64_t ist[7];         // IST1–IST7: interrupt stack table
    uint64_t _reserved2;
    uint16_t _reserved3;
    uint16_t ioBitmapOffset; // set to sizeof(Tss64) to disable I/O bitmap
} __attribute__((packed));

// Maximum CPUs supported.
static constexpr uint32_t GDT_MAX_CPUS = 64;

// Populate the GDT (with TSS), load it via lgdt, reload segment registers,
// and load the TSS via ltr.  Must be called before IdtInit.
void GdtInit();

// Initialise per-CPU GDT/TSS for an AP. Allocates a new TSS, patches the
// BSP's GDT table (adding a new TSS descriptor), and returns the TSS selector.
// The AP must then lgdt + ltr on its own core.
uint16_t GdtInitAp(uint32_t cpuIndex);

// Load the GDT and TSS selector on the current CPU (for AP use after GdtInitAp).
void GdtLoadOnAp(uint16_t tssSelector);

// Set TSS.RSP0 — the kernel stack loaded by the CPU on ring 3 → ring 0
// transitions (interrupts, exceptions, but NOT SYSCALL).
void GdtSetTssRsp0(uint64_t stackTop);

// Per-CPU version: set RSP0 on a specific CPU's TSS.
void GdtSetTssRsp0ForCpu(uint32_t cpuIndex, uint64_t stackTop);

// Get the TSS for a specific CPU.
Tss64* GdtGetTss(uint32_t cpuIndex);
