#pragma once
#include <stdint.h>

namespace brook {

struct DebugLocalRequest {
    uint32_t tokenBytes = 0;
    bool allowed = false;
};

inline DebugLocalRequest DebugLocalRequestPolicy(const char* command, uint32_t bytes)
{
    DebugLocalRequest request;
    if (!command || !bytes) return request;

    uint32_t start = 0;
    if (command[0] == '@') {
        start = 1;
        while (start < bytes && command[start] >= '0' && command[start] <= '9') ++start;
        if (start == 1 || start > 24 || start == bytes || command[start] != ' ') return request;
        request.tokenBytes = start - 1;
        ++start;
    }

    // The recovery network self-check requires this one read-only command.
    // Match the actual byte span before the command dispatcher sees it.
    constexpr char caps[] = "bridge hardware caps";
    if (bytes - start != sizeof(caps) - 1) return request;
    for (uint32_t i = 0; i < sizeof(caps) - 1; ++i)
        if (command[start + i] != caps[i]) return request;
    request.allowed = true;
    return request;
}

}
