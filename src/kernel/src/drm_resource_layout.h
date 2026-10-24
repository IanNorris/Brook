#pragma once
#include <stdint.h>

// VirGL's client size includes its padded rows, layers and mip chain. Some
// host-only textures use a size of one, so retain a conservative base-level
// minimum as well. Never narrow the client's real allocation to that minimum.
inline bool DrmLegacyResourceLayout(bool buffer, uint32_t width, uint32_t height,
                                    uint32_t clientStride, uint32_t clientSize,
                                    uint32_t* stride, uint32_t* size)
{
    const uint64_t row = clientStride ? clientStride :
        (buffer ? uint64_t{width} : uint64_t{width} * 4);
    const uint64_t minimum = buffer ? uint64_t{width} : row * (height ? height : 1);
    const uint64_t bytes = clientSize > minimum ? clientSize : minimum;
    if (!row || row > 0xffffffffULL || !bytes || bytes > 0xffffffffULL) return false;
    *stride = static_cast<uint32_t>(row);
    *size = static_cast<uint32_t>(bytes);
    return true;
}
