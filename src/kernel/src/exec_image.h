#pragma once
#include <stdint.h>
namespace brook {
struct Vnode;
// Temporary kernel mapping of the entire executable, independent of its user
// load span. No silent truncation; callers own the vnode and release the image.
constexpr uint64_t MAX_EXEC_IMAGE_BYTES = 512ULL * 1024 * 1024;
struct ExecutableImage {
    uint8_t* data = nullptr;
    uint64_t size = 0;
    uint64_t pages = 0;
};
struct ElfBinary;
struct AddressSpaceState;
// Caller retains the immutable, dedicated ReadExecutableImage buffer throughout
// loading and metadata extraction. User mappings acquire independent COW refs.
// Do not wrap arbitrary aligned buffers here: shape checks cannot establish
// allocation provenance or exclude unrelated kernel data on their pages.
bool ElfLoadExecutable(const ExecutableImage&, ElfBinary*, AddressSpaceState*, uint16_t);
int ReadExecutableImage(Vnode*, ExecutableImage*);
void FreeExecutableImage(ExecutableImage*);
bool ElfValidateMainImage(const uint8_t*, uint64_t);
}
