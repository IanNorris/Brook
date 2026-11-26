#include "../../bootloader/src/boot_compression.h"
#include <cassert>
#include <cstring>

using namespace brook::bootloader;

int main()
{
    uint8_t file[BootCompressedPrefixBytes + 16]{};
    memcpy(file, BootCompressedMagic, sizeof(BootCompressedMagic));
    BrookUpdatePut32(file + 8, uint32_t(BootPayloadKind::Kernel));
    BrookUpdatePut32(file + 12, 1);
    BrookUpdatePut64(file + 16, 8u << 20);
    BrookUpdatePut64(file + 24, sizeof(file) - BootCompressedPrefixBytes);

    constexpr uint64_t Limit = 8u << 20;
    assert(BootExpandedSize(file, sizeof(file), BootPayloadKind::Kernel, Limit) == Limit);
    assert(!BootExpandedSize(file, sizeof(file), BootPayloadKind::Initrd, Limit));
    assert(!BootExpandedSize(file, sizeof(file) - 1, BootPayloadKind::Kernel, Limit));
    assert(!BootExpandedSize(file, BootCompressedPrefixBytes, BootPayloadKind::Kernel, Limit));
    assert(!BootExpandedSize(file, 7, BootPayloadKind::Kernel, Limit));
    assert(!BootExpandedSize(file, sizeof(file), BootPayloadKind::Kernel, Limit - 1));

    BrookUpdatePut64(file + 16, 0xffffffffffffffffull);
    assert(!BootExpandedSize(file, sizeof(file), BootPayloadKind::Kernel, 0xffffffffffffffffull));
    BrookUpdatePut64(file + 16, 63);
    assert(!BootExpandedSize(file, sizeof(file), BootPayloadKind::Kernel, Limit));
    BrookUpdatePut64(file + 16, 64);
    BrookUpdatePut32(file + 12, 0);
    assert(!BootExpandedSize(file, sizeof(file), BootPayloadKind::Kernel, Limit));
    file[0] = 0x7f;
    assert(!BootCompressed(file, sizeof(file)));
}
