#pragma once

#include "../../kernel/src/cursor_composition.h"

namespace brook::svga {

constexpr uint32_t CursorCommandWords = 8 + HardwareCursorCapacity * (HardwareCursorCapacity + HardwareCursorCapacity / 32);

enum class CursorPacket { Invalid, Hidden, Image };

inline uint32_t PremultiplyCursorPixel(uint32_t pixel)
{
    const uint32_t alpha = pixel >> 24;
    uint32_t result = alpha << 24;
    for (unsigned shift = 0; shift < 24; shift += 8)
        result |= ((((pixel >> shift) & 255) * alpha + 127) / 255) << shift;
    return result;
}

inline bool CursorImageMatches(const HardwareCursorImage& a, const HardwareCursorImage& b)
{
    if (a.width != b.width || a.height != b.height || a.hotX != b.hotX || a.hotY != b.hotY)
        return false;
    for (uint32_t y = 0; y < a.height; ++y)
        for (uint32_t x = 0; x < a.width; ++x)
            if (a.pixels[y * HardwareCursorCapacity + x] != b.pixels[y * HardwareCursorCapacity + x])
                return false;
    return true;
}

inline CursorPacket EncodeCursor(const HardwareCursorImage& image, bool alpha, uint32_t* words,
                                 uint32_t& count)
{
    if (!image.width || image.width > HardwareCursorCapacity || !image.height ||
        image.height > HardwareCursorCapacity || image.hotX < 0 || image.hotY < 0 ||
        uint32_t(image.hotX) >= image.width || uint32_t(image.hotY) >= image.height)
        return CursorPacket::Invalid;

    if (!CursorPixelsHaveAlpha(image.pixels, image.width, image.height))
        return CursorPacket::Hidden;

    // The legacy AND rows are dword-aligned. Padding with transparent pixels
    // also keeps the row stride identical for QEMU's byte-aligned cursor reader.
    const uint32_t width = alpha ? image.width : (image.width + 31) & ~31u;
    words[0] = alpha ? 22 : 19;
    words[1] = 0;
    words[2] = uint32_t(image.hotX);
    words[3] = uint32_t(image.hotY);
    words[4] = width;
    words[5] = image.height;
    count = 6;

    uint32_t* mask = nullptr;
    if (!alpha) {
        words[count++] = 1;
        words[count++] = 32;
        mask = words + count;
        count += width / 32 * image.height;
        for (uint32_t i = 0; i < width / 32 * image.height; ++i) mask[i] = 0;
    }

    for (uint32_t y = 0; y < image.height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t pixel = x < image.width ? image.pixels[y * HardwareCursorCapacity + x] : 0;
            if (alpha) {
                words[count++] = PremultiplyCursorPixel(pixel);
            } else {
                const uint32_t opacity = pixel >> 24;
                if (opacity && opacity != 255) return CursorPacket::Invalid;
                if (!opacity) {
                    const uint32_t bit = (x % 32 / 8) * 8 + 7 - x % 8;
                    mask[y * (width / 32) + x / 32] |= 1u << bit;
                }
                words[count++] = opacity ? pixel & 0x00ffffff : 0;
            }
        }
    }
    return CursorPacket::Image;
}

} // namespace brook::svga
