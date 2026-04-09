#pragma once

#include <stdint.h>

// PS/2 keyboard driver.
//
// IRQ1 is routed through the I/O APIC at KBD_IRQ_VECTOR (33).
// Scan code set 1 → ASCII translation is done in the interrupt handler.
// A small ring buffer stores decoded characters.
//
// keyboard.cpp is compiled with -mgeneral-regs-only (ISR file).
//
// Lifecycle:
//   KbdInit() must be called after IoApicInit().
//   After KbdInit(), KbdGetChar()/KbdPeekChar() are safe from any non-ISR context.

namespace brook {

// IRQ vector used for keyboard (ISA IRQ 1, one above LAPIC timer vector 32).
static constexpr uint8_t KBD_IRQ_VECTOR = 33;

// Initialise the PS/2 keyboard: install the IRQ1 handler and unmask it
// in the I/O APIC.
void KbdInit();

// Non-blocking: return the next character from the ring buffer,
// or 0 if the buffer is empty.
char KbdPeekChar();

// Blocking: spin until a character is available, then return it.
char KbdGetChar();

} // namespace brook
