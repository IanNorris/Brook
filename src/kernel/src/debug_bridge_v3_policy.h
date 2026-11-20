#pragma once

#include <stdint.h>
#include "debug_local_policy.h"

namespace brook {

inline constexpr uint32_t DebugBridgeV3CommandBytes = 256; // includes the LF

inline bool DebugBridgeV3CommandAllowed(const char* command, uint32_t length, uint8_t operation)
{
    if (!command || length < 5 || length > DebugBridgeV3CommandBytes ||
        command[0] != '@' || command[length - 1] != '\n' || operation > 1) return false;
    for (uint32_t i = 0; i + 1 < length; ++i)
        if (static_cast<unsigned char>(command[i]) < ' ' ||
            static_cast<unsigned char>(command[i]) > '~') return false;

    uint32_t tokenEnd = 1;
    while (tokenEnd < length && command[tokenEnd] >= '0' && command[tokenEnd] <= '9') ++tokenEnd;
    if (tokenEnd == 1 || tokenEnd > 24 || tokenEnd + 2 >= length || command[tokenEnd] != ' ' ||
        command[tokenEnd + 1] == ' ' || command[tokenEnd + 1] == '@') return false;
    return operation == 1 || DebugLocalRequestPolicy(command, length - 1).allowed;
}

}
