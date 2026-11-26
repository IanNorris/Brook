#pragma once

#include <stdint.h>
#include "../../shared/update/boot_compression_magic.h"

namespace brook { namespace bootloader {

enum class BootPayloadKind : uint32_t { Kernel = 1, Initrd = 2 };
constexpr uint32_t BootCompressedHeaderBytes = 64;
constexpr uint32_t BootCompressedPrefixBytes = BootCompressedHeaderBytes + 64;
constexpr char BootCompressionSignDomain[] = "BROOK-BOOT-COMPRESSION-V1";

inline bool BootCompressed(const uint8_t* file, BrookUpdateSize bytes)
{
    return BrookBootCompressed(file, bytes);
}

// Header: magic, kind u32, codec u32, expanded u64, stored u64, SHA-256 of
// the LZ4 block. The Ed25519 message is the domain (including NUL) followed
// by all 64 header bytes.
inline uint64_t BootExpandedSize(const uint8_t* file, BrookUpdateSize bytes, BootPayloadKind kind, uint64_t maximum)
{
    if (!BootCompressed(file, bytes) || bytes <= BootCompressedPrefixBytes ||
        BrookUpdateGet32(file + 8) != uint32_t(kind) || BrookUpdateGet32(file + 12) != 1 ||
        BrookUpdateGet64(file + 24) != bytes - BootCompressedPrefixBytes) return 0;
    const uint64_t size = BrookUpdateGet64(file + 16);
    const uint64_t minimum = kind == BootPayloadKind::Kernel ? 64 : 512;
    // LZ4's freestanding decoder takes signed int lengths.
    return size >= minimum && size <= maximum && size <= 0x7fffffffu &&
        bytes - BootCompressedPrefixBytes <= 0x7fffffffu ? size : 0;
}

}} // namespace brook::bootloader
