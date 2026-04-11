#pragma once

#include <stdint.h>

// APIC subsystem — Local APIC init, PIC disable, LAPIC timer.
//
// Sequence:
//   1. PIC disabled (8259 masked): legacy IRQs no longer fire.
//   2. LAPIC enabled via IA32_APIC_BASE MSR (or MMIO-mapped base from MADT).
//   3. LAPIC timer calibrated against PIT channel 2 (~10ms window).
//   4. LAPIC timer set to periodic mode, vector 32 (IRQ0 equivalent).
//
// All LAPIC MMIO access goes through a virtual mapping set up during ApicInit().
// Must be called after VmmInit(), HeapInit(), PmmEnableTracking(), AcpiInit().

namespace brook {

// LAPIC register offsets (byte offsets from LAPIC base).
namespace LapicReg {
    static constexpr uint32_t ID             = 0x020;
    static constexpr uint32_t VERSION        = 0x030;
    static constexpr uint32_t TPR            = 0x080;  // Task Priority
    static constexpr uint32_t EOI            = 0x0B0;  // End of Interrupt
    static constexpr uint32_t SVR            = 0x0F0;  // Spurious Interrupt Vector
    static constexpr uint32_t ICR_LO         = 0x300;  // Interrupt Command (low)
    static constexpr uint32_t ICR_HI         = 0x310;  // Interrupt Command (high)
    static constexpr uint32_t LVT_TIMER      = 0x320;  // LVT Timer entry
    static constexpr uint32_t TIMER_INIT_CNT = 0x380;  // Timer Initial Count
    static constexpr uint32_t TIMER_CUR_CNT  = 0x390;  // Timer Current Count
    static constexpr uint32_t TIMER_DIVIDE   = 0x3E0;  // Timer Divide Config
}  // namespace LapicReg

// SVR bit: LAPIC software enable.
static constexpr uint32_t LAPIC_SVR_ENABLE       = (1u << 8);
// SVR spurious vector (bottom byte).
static constexpr uint32_t LAPIC_SPURIOUS_VECTOR  = 0xFF;

// LVT Timer mode bits.
static constexpr uint32_t LAPIC_TIMER_PERIODIC   = (1u << 17);
static constexpr uint32_t LAPIC_TIMER_MASKED      = (1u << 16);

// LAPIC timer IRQ vector.
static constexpr uint8_t  LAPIC_TIMER_VECTOR     = 32;

// Initialise the APIC subsystem.
// localApicPhysical: physical address from AcpiGetMadt().localApicPhysical.
// Returns false if the LAPIC is absent or init fails.
bool ApicInit(uint64_t localApicPhysical);

// Send End-of-Interrupt to the LAPIC.  Must be called at the end of every
// LAPIC-sourced interrupt handler.
void ApicSendEoi();

// Read the LAPIC ID of the current CPU.
uint8_t ApicGetId();

// Return the calibrated LAPIC timer ticks per millisecond.
// Valid only after ApicInit().
uint32_t ApicGetTimerTicksPerMs();

// Return the virtual base address used to access LAPIC registers.
uint64_t ApicGetLapicVirtBase();

// ---------------------------------------------------------------------------
// I/O APIC
// ---------------------------------------------------------------------------

// Map the I/O APIC MMIO and store internal state.
// Must be called before IoApicUnmaskIrq/IoApicMaskIrq.
bool IoApicInit(uint64_t ioApicPhysical, uint32_t gsiBase);

// Program an I/O APIC redirection table entry to deliver irq at vector,
// edge-triggered, active-high, to the BSP (LAPIC ID from ApicGetId()).
// irq is the ISA IRQ number (0-based from GSI base).
void IoApicUnmaskIrq(uint8_t irq, uint8_t vector);

// Mask (disable) an I/O APIC IRQ line.
void IoApicMaskIrq(uint8_t irq);

// Start the LAPIC timer on the calling AP. Uses the BSP's calibrated
// ticks-per-ms value. Must be called after the AP has enabled its LAPIC.
void ApicInitTimerOnAp();

} // namespace brook
