#include "../../kernel/src/fs/compressed_image.h"

// Optional host-tool acceleration; the on-disk checksum and implementation
// are exactly the same as the kernel reader's corruption check.
extern "C" uint64_t brook_image_checksum(const void* bytes, uint64_t length) {
    return brook::CompressedImageChecksum(bytes, length);
}

extern "C" uint64_t brook_image_checksum_update(uint64_t seed, const void* bytes, uint64_t length) {
    auto* data=static_cast<const uint8_t*>(bytes);
    for(uint64_t i=0;i<length;++i)seed=(seed^data[i])*1099511628211ULL;
    return seed;
}
