#pragma once
#include <stdint.h>

namespace brook {

// Human-readable exception description for panic/crash screens.
// Returns a static string describing the exception in plain language.
const char* ExceptionDescribe(uint8_t vector, uint64_t errorCode, uint64_t cr2,
                               uint64_t rip, bool fromUser);

// Short exception name (e.g., "#GP General Protection")
const char* ExceptionName(uint8_t vector);

} // namespace brook
