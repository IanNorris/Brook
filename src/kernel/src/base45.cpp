// Base45 encoder — alphanumeric encoding for QR code data.
// Based on wallabythree/base45 (MIT license), modified for freestanding use.
// Original: https://github.com/nicktab/base45 (kf03w5t5741l)

#include "base45.h"

namespace brook {

static const char g_charset[] = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8',
    '9', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
    'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q',
    'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    ' ', '$', '%', '*', '+', '-', '.', '/', ':'
};

static unsigned int PowInt(unsigned int base, unsigned int exp)
{
    unsigned int result = 1;
    for (unsigned int i = 0; i < exp; ++i) result *= base;
    return result;
}

static int EncodeChunk(char* dst, const uint8_t* src, int size, uint32_t* written)
{
    *written = 0;

    unsigned int sum = 0;
    for (int i = 0; i < size; ++i)
    {
        sum += static_cast<unsigned int>(src[size - i - 1]) * PowInt(256, i);
    }

    unsigned int i = 0;
    while (sum > 0)
    {
        dst[i] = g_charset[sum % 45];
        sum /= 45;
        ++(*written);
        ++i;
    }

    return BASE45_OK;
}

int Base45Encode(char* dest, uint32_t destLen, const uint8_t* input, uint32_t inputLen)
{
    uint32_t requiredLen = inputLen / 2 * 3 + inputLen % 2 * 2 + 1;
    if (destLen < requiredLen) return -BASE45_MEMORY_ERROR;

    uint32_t i = 0;
    uint32_t offset = 0;

    while (i < inputLen)
    {
        uint32_t chunkSize = (inputLen - i > 1) ? 2 : 1;
        uint32_t written = 0;

        int status = EncodeChunk(dest + offset, input + i, chunkSize, &written);
        if (status != BASE45_OK) return -status;

        // Pad with leading zeros if needed
        while (written < chunkSize + 1)
        {
            dest[offset + written] = g_charset[0];
            ++written;
        }
        offset += written;
        i += chunkSize;
    }

    dest[offset] = '\0';
    return static_cast<int>(offset);
}

} // namespace brook
