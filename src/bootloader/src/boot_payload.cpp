#include "boot_payload.h"
#include "memory.h"

#if defined(BROOK_BOOT_COMPRESSION_KEY)
#include "monocypher-ed25519.h"

extern "C" int LZ4_decompress_safe(const char*, char*, int, int);

namespace {
constexpr uint8_t HexNibble(char ch)
{
    return ch <= '9' ? uint8_t(ch - '0') : uint8_t(ch - 'a' + 10);
}

struct BootPublicKey {
    uint8_t bytes[32];
    constexpr BootPublicKey(const char (&hex)[65]) : bytes{}
    {
        for (unsigned i = 0; i < 32; ++i)
            bytes[i] = uint8_t(HexNibble(hex[i * 2]) * 16 + HexNibble(hex[i * 2 + 1]));
    }
};
constexpr BootPublicKey PublicKey(BROOK_BOOT_COMPRESSION_KEY);
}
#endif

namespace brook { namespace bootloader {

bool ExpandBootPayload(EFI_BOOT_SERVICES* bs, uint8_t*& file, UINTN& bytes,
                       BootPayloadKind kind, UINTN maximum)
{
    if (!BootCompressed(file, bytes)) return true;
#if defined(BROOK_BOOT_COMPRESSION_KEY)
    const UINTN expanded = BootExpandedSize(file, bytes, kind, maximum);
    if (!expanded) return false;
    uint8_t message[sizeof(BootCompressionSignDomain) + BootCompressedHeaderBytes];
    for (unsigned i = 0; i < sizeof(BootCompressionSignDomain); ++i)
        message[i] = static_cast<uint8_t>(BootCompressionSignDomain[i]);
    for (unsigned i = 0; i < BootCompressedHeaderBytes; ++i)
        message[sizeof(BootCompressionSignDomain) + i] = file[i];
    if (crypto_ed25519_check_strict(file + BootCompressedHeaderBytes, PublicKey.bytes,
                                   message, sizeof(message)) != 0) return false;
    const uint8_t* body = file + BootCompressedPrefixBytes;
    const UINTN stored = bytes - BootCompressedPrefixBytes;
    uint8_t digest[32];
    BrookUpdateHash(body, stored, digest);
    if (!BrookUpdateEqual(digest, file + 32, sizeof(digest))) return false;

    EFI_PHYSICAL_ADDRESS decoded = 0;
    if (EFI_ERROR(bs->AllocatePages(AllocateAnyPages, EfiLoaderData, EFI_SIZE_TO_PAGES(expanded), &decoded)))
        return false;
    if (LZ4_decompress_safe(reinterpret_cast<const char*>(body), reinterpret_cast<char*>(decoded),
                            int(stored), int(expanded)) != int(expanded)) {
        FreePages(bs, decoded, expanded);
        return false;
    }
    FreePages(bs, reinterpret_cast<EFI_PHYSICAL_ADDRESS>(file), bytes);
    file = reinterpret_cast<uint8_t*>(decoded);
    bytes = expanded;
    return true;
#else
    (void)bs;
    (void)kind;
    (void)maximum;
    return false;
#endif
}

}} // namespace brook::bootloader
