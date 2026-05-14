#pragma once

#include <stdint.h>

// Intel PCH SMBus (i801) driver — polling mode.
// Supports: Quick command, byte data read/write, word data read/write, bus scan.
// Target: Intel Comet Lake-H PCH (PCI 8086:06A3) at BDF 00:1f.4.

namespace brook {

// Initialize the SMBus controller. Finds the PCI device, reads BAR4,
// enables the host controller. Returns 0 on success, <0 on error.
int SmbusInit();

// Probe whether a device exists at the given 7-bit address (0x03–0x77).
// Returns 0 if device ACKed, -ENXIO if no response, <0 on other errors.
int SmbusProbe(uint8_t addr);

// Read a single byte from register 'reg' on device at 7-bit address 'addr'.
// Returns the byte value (0–255) on success, <0 on error.
int SmbusReadByte(uint8_t addr, uint8_t reg);

// Write a single byte 'val' to register 'reg' on device at 'addr'.
// Returns 0 on success, <0 on error.
int SmbusWriteByte(uint8_t addr, uint8_t reg, uint8_t val);

// Read a 16-bit word from register 'reg' on device at 'addr'.
// Returns the word value (0–65535) on success, <0 on error.
int SmbusReadWord(uint8_t addr, uint8_t reg);

// Scan the bus (addresses 0x03–0x77) and print discovered devices to serial.
// Returns the number of devices found.
int SmbusScan();

// Read SPD EEPROM info from a DIMM slot (0–7, maps to addresses 0x50–0x57).
// Prints DRAM type, module type, and part number to serial.
// Returns 0 on success, <0 if no DIMM in that slot.
int SmbusSpdRead(int slot);

} // namespace brook
