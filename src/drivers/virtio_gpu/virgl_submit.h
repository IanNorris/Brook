#pragma once
#include <stdint.h>

// Validate before submitting any part of a userspace command stream.
inline bool VirglStreamValid(const uint32_t* words, uint32_t count, uint32_t cap)
{
    for (uint32_t pos = 0; pos < count;) {
        uint32_t length = (words[pos] >> 16) + 1;
        if (length > cap || length > count - pos) return false;
        pos += length;
    }
    return count != 0;
}

// Whole-command split, rewound to the last completed DRAW_VBO if necessary.
// Keep SET_VERTEX_BUFFERS and its consuming draw in the same host submission.
inline uint32_t VirglChunkEnd(const uint32_t* words, uint32_t count,
                             uint32_t start, uint32_t cap)
{
    uint32_t pos = start, lastDrawEnd = start;
    while (pos < count) {
        uint32_t length = (words[pos] >> 16) + 1;
        if (length > cap - (pos - start))
            return lastDrawEnd > start ? lastDrawEnd : pos;
        if ((words[pos] & 0xff) == 8) lastDrawEnd = pos + length;
        pos += length;
    }
    return pos;
}
