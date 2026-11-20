#pragma once
#include <stdint.h>

bool RdrandU64(uint64_t* out);

namespace brook {

inline bool StrongRandomBytes(uint8_t* out, uint32_t length)
{
    while (length) {
        uint64_t word;
        if (!RdrandU64(&word)) return false;
        const uint32_t chunk = length < sizeof(word) ? length : sizeof(word);
        for (uint32_t i = 0; i < chunk; ++i) out[i] = static_cast<uint8_t>(word >> (8 * i));
        out += chunk;
        length -= chunk;
    }
    return true;
}

}
