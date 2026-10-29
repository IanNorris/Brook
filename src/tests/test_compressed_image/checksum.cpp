#include "../../kernel/src/fs/compressed_image.h"

// Optional host-tool acceleration; the on-disk checksum and implementation
// are exactly the same as the kernel reader's corruption check.
extern "C" uint64_t brook_image_checksum(const void* bytes, uint64_t length) {
    return brook::CompressedImageChecksum(bytes, length);
}
