#include "config.h"
#include "fs.h"
#include "memory.h"

namespace brook {
namespace bootloader {

BootConfig g_bootConfig;

static bool KeyEquals(const uint8_t* line, size_t lineLen,
                      const char* key, size_t keyLen)
{
    if (lineLen != keyLen) return false;
    for (size_t i = 0; i < keyLen; i++) {
        if (line[i] != static_cast<uint8_t>(key[i])) return false;
    }
    return true;
}

static void TrimValue(const uint8_t* src, size_t len,
                      size_t* outStart, size_t* outLen)
{
    size_t start = 0;
    while (start < len && src[start] == ' ') start++;
    size_t end = len;
    while (end > start && src[end - 1] == ' ') end--;
    *outStart = start;
    *outLen   = end - start;
}

static bool ParseBool(const uint8_t* val, size_t len)
{
    return len > 0 && val[0] == '1';
}

static void AsciiToUtf16(const uint8_t* src, size_t len,
                          char16_t* dst, size_t maxDst)
{
    size_t count = (len < maxDst - 1) ? len : maxDst - 1;
    for (size_t i = 0; i < count; i++) {
        dst[i] = static_cast<char16_t>(src[i]);
    }
    dst[count] = u'\0';
}

void LoadConfig(EFI_HANDLE imageHandle, EFI_BOOT_SERVICES* bootServices)
{
    // Defaults
    const char16_t defaultTarget[] = u"KERNEL\\BROOK.ELF";
    size_t di = 0;
    while (defaultTarget[di] != u'\0' && di < 255) {
        g_bootConfig.target[di] = defaultTarget[di];
        di++;
    }
    g_bootConfig.target[di]     = u'\0';
    g_bootConfig.debugText      = false;
    g_bootConfig.logMemory      = false;
    g_bootConfig.logInterrupts  = false;

    UINTN fileSize = 0;
    uint8_t* data = ReadFile(imageHandle, bootServices, u"BROOK.CFG", &fileSize);
    if (data == nullptr) return;

    size_t pos = 0;
    while (pos < fileSize) {
        // Scan to end of line
        size_t lineStart = pos;
        while (pos < fileSize && data[pos] != '\n' && data[pos] != '\r') pos++;
        size_t lineEnd = pos;
        while (pos < fileSize && (data[pos] == '\n' || data[pos] == '\r')) pos++;

        // Trim leading spaces
        size_t start = lineStart;
        while (start < lineEnd && data[start] == ' ') start++;

        size_t lineLen = lineEnd - start;
        if (lineLen == 0) continue;
        if (data[start] == '#') continue;

        // Find '='
        size_t eq = start;
        while (eq < lineEnd && data[eq] != '=') eq++;
        if (eq >= lineEnd) continue;

        const uint8_t* keyPtr = data + start;
        size_t keyLen         = eq - start;
        const uint8_t* valPtr = data + eq + 1;
        size_t valLen         = lineEnd - (eq + 1);

        size_t valStart = 0, trimmedLen = 0;
        TrimValue(valPtr, valLen, &valStart, &trimmedLen);
        const uint8_t* trimmedVal = valPtr + valStart;

        if (KeyEquals(keyPtr, keyLen, "TARGET", 6)) {
            AsciiToUtf16(trimmedVal, trimmedLen, g_bootConfig.target, 256);
        } else if (KeyEquals(keyPtr, keyLen, "DEBUG_TEXT", 10)) {
            g_bootConfig.debugText = ParseBool(trimmedVal, trimmedLen);
        } else if (KeyEquals(keyPtr, keyLen, "LOG_MEMORY", 10)) {
            g_bootConfig.logMemory = ParseBool(trimmedVal, trimmedLen);
        } else if (KeyEquals(keyPtr, keyLen, "LOG_INTERRUPTS", 14)) {
            g_bootConfig.logInterrupts = ParseBool(trimmedVal, trimmedLen);
        }
        // Unrecognised keys silently ignored
    }

    FreePages(bootServices,
              reinterpret_cast<EFI_PHYSICAL_ADDRESS>(data),
              fileSize > 0 ? fileSize : 1);
}

} // namespace bootloader
} // namespace brook
