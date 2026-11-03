#include "config.h"
#include "fs.h"
#include "memory.h"

namespace brook {
namespace bootloader {

BootConfig g_bootConfig;

static bool KeyEquals(const uint8_t* line, UINTN lineLen,
                      const char* key, UINTN keyLen)
{
    if (lineLen != keyLen) return false;
    for (UINTN i = 0; i < keyLen; i++) {
        if (line[i] != static_cast<uint8_t>(key[i])) return false;
    }
    return true;
}

static void TrimValue(const uint8_t* src, UINTN len,
                      UINTN* outStart, UINTN* outLen)
{
    UINTN start = 0;
    while (start < len && src[start] == ' ') start++;
    UINTN end = len;
    while (end > start && src[end - 1] == ' ') end--;
    *outStart = start;
    *outLen   = end - start;
}

static uint32_t ParseDimension(const uint8_t* val, UINTN len)
{
    if (!len || len > 5) return 0;
    uint32_t result = 0;
    for (UINTN i=0;i<len;++i) {
        if (val[i]<'0' || val[i]>'9') return 0;
        result=result*10+(val[i]-'0');
    }
    return result <= 16384 ? result : 0;
}

static bool ParseBool(const uint8_t* val, UINTN len)
{
    return len > 0 && val[0] == '1';
}

static void AsciiToUtf16(const uint8_t* src, UINTN len,
                          char16_t* dst, UINTN maxDst)
{
    UINTN count = (len < maxDst - 1) ? len : maxDst - 1;
    for (UINTN i = 0; i < count; i++) {
        dst[i] = static_cast<char16_t>(src[i]);
    }
    dst[count] = u'\0';
}

void LoadConfig(EFI_HANDLE imageHandle, EFI_BOOT_SERVICES* bootServices)
{
    // Defaults
    const char16_t defaultTarget[] = u"KERNEL\\BROOK.ELF";
    UINTN di = 0;
    while (defaultTarget[di] != u'\0' && di < 255) {
        g_bootConfig.target[di] = defaultTarget[di];
        di++;
    }
    g_bootConfig.target[di]     = u'\0';
    g_bootConfig.displayWidth   = 0;
    g_bootConfig.displayHeight  = 0;
    g_bootConfig.debugText      = false;
    g_bootConfig.updateSlots    = false;
    g_bootConfig.recoveryMenu   = false;
    g_bootConfig.logMemory      = false;
    g_bootConfig.logInterrupts  = false;

    UINTN fileSize = 0;
    uint8_t* data = ReadFile(imageHandle, bootServices, u"BROOK.CFG", &fileSize);
    if (data == nullptr) return;

    UINTN pos = 0;
    while (pos < fileSize) {
        // Scan to end of line
        UINTN lineStart = pos;
        while (pos < fileSize && data[pos] != '\n' && data[pos] != '\r') pos++;
        UINTN lineEnd = pos;
        while (pos < fileSize && (data[pos] == '\n' || data[pos] == '\r')) pos++;

        // Trim leading spaces
        UINTN start = lineStart;
        while (start < lineEnd && data[start] == ' ') start++;

        UINTN lineLen = lineEnd - start;
        if (lineLen == 0) continue;
        if (data[start] == '#') continue;

        // Find '='
        UINTN eq = start;
        while (eq < lineEnd && data[eq] != '=') eq++;
        if (eq >= lineEnd) continue;

        const uint8_t* keyPtr = data + start;
        UINTN keyLen         = eq - start;
        const uint8_t* valPtr = data + eq + 1;
        UINTN valLen         = lineEnd - (eq + 1);

        UINTN valStart = 0, trimmedLen = 0;
        TrimValue(valPtr, valLen, &valStart, &trimmedLen);
        const uint8_t* trimmedVal = valPtr + valStart;

        if (KeyEquals(keyPtr, keyLen, "TARGET", 6)) {
            AsciiToUtf16(trimmedVal, trimmedLen, g_bootConfig.target, 256);
        } else if (KeyEquals(keyPtr, keyLen, "UPDATE_SLOTS", 12)) {
            g_bootConfig.updateSlots = ParseBool(trimmedVal, trimmedLen);
        } else if (KeyEquals(keyPtr, keyLen, "RECOVERY_MENU", 13)) {
            g_bootConfig.recoveryMenu = ParseBool(trimmedVal, trimmedLen);
        } else if (KeyEquals(keyPtr, keyLen, "DEBUG_TEXT", 10)) {
            g_bootConfig.debugText = ParseBool(trimmedVal, trimmedLen);
        } else if (KeyEquals(keyPtr, keyLen, "LOG_MEMORY", 10)) {
            g_bootConfig.logMemory = ParseBool(trimmedVal, trimmedLen);
        } else if (KeyEquals(keyPtr, keyLen, "LOG_INTERRUPTS", 14)) {
            g_bootConfig.logInterrupts = ParseBool(trimmedVal, trimmedLen);
        } else if (KeyEquals(keyPtr, keyLen, "DISPLAY_WIDTH", 13)) {
            g_bootConfig.displayWidth=ParseDimension(trimmedVal,trimmedLen);
        } else if (KeyEquals(keyPtr, keyLen, "DISPLAY_HEIGHT", 14)) {
            g_bootConfig.displayHeight=ParseDimension(trimmedVal,trimmedLen);
        }
        // Unrecognised keys silently ignored
    }

    FreePages(bootServices,
              reinterpret_cast<EFI_PHYSICAL_ADDRESS>(data),
              fileSize > 0 ? fileSize : 1);
}

} // namespace bootloader
} // namespace brook
