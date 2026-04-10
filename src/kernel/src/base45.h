#pragma once
// Base45 encoder — alphanumeric encoding for QR code data.
// Based on wallabythree/base45 (MIT license), modified for freestanding use.

#include <stdint.h>

namespace brook {

static constexpr int BASE45_OK = 0;
static constexpr int BASE45_MEMORY_ERROR = 1;

// Encode binary data to Base45.
// Returns number of characters written (excluding null terminator), or negative on error.
int Base45Encode(char* dest, uint32_t destLen, const uint8_t* input, uint32_t inputLen);

} // namespace brook
