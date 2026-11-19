#pragma once
// Chrome display lists (compositor refactor stage 1.2). The frame and shell
// builders describe a surface as a short list of primitives; one executor
// (chrome_raster.cpp) rasterises a list on the CPU. Building and executing
// are separate, so the same list can later go to another executor or across
// a process boundary, and host tests render it into golden images.
//
// The primitives are exactly the operations today's painters perform, so the
// CPU executor reproduces their pixels. Coordinates are in the caller's
// surface space (normally the screen); text is copied into the list. Icon
// and row-colour pointers are borrowed: the caller keeps them alive until the
// list has been executed.
//
// Freestanding: no kernel dependencies.
#include <stdint.h>
#include "desktop_metrics.h"

namespace brook {

enum class ChromeOp : uint8_t {
    FadeX,        // source-over horizontal stroke, from solid to clear
    Fill,         // solid rect, replaces the destination
    Rows,         // one colour per row (title material gradient)
    CornerFill,   // solid rect clipped to a rounded top-right corner of clip
    MaterialOutline, // one-pixel stroke with rounded corners
    MaterialRect, // solid rect with rounded corners, top and bottom
    Text,         // glyph run, source-over by coverage
    Icon,         // straight-alpha ARGB icon, source-over, scaled
    Squares,      // a run of equal rects stepping by (dx, dy): drawn strokes
};

struct ChromeCmd {
    ChromeOp op;
    int32_t x, y, w, h; // Text: pen origin, w = maximum width (0: none); Icon: w = size
    uint32_t color;
    uint32_t arg;       // Text: arena offset; CornerFill/MaterialRect: radius; Squares: count
    int32_t dx, dy;     // Squares: step
    DesktopRect clip;   // Text: glyph clip when dx != 0; CornerFill: rounded block
    const void* data;   // Rows: uint32_t[h] colours; Icon: pixels
};

class ChromeDisplayList {
public:
    ChromeDisplayList(ChromeCmd* cmds, uint32_t capacity, char* text, uint32_t textCapacity)
        : cmds_(cmds), capacity_(capacity), text_(text), textCapacity_(textCapacity) {}
    ChromeDisplayList(const ChromeDisplayList&) = delete;
    ChromeDisplayList& operator=(const ChromeDisplayList&) = delete;

    uint32_t Count() const { return count_; }
    const ChromeCmd& At(uint32_t i) const { return cmds_[i]; }
    const char* TextAt(uint32_t offset) const { return text_ + offset; }
    // A builder asked for more than the list holds; the excess was dropped.
    bool Overflowed() const { return overflow_; }
    void Clear() { count_ = textUsed_ = 0; overflow_ = false; }

    void FadeX(int x, int y, int w, int h, uint32_t color) {
        Push({ChromeOp::FadeX, x, y, w, h, color, 0, 0, 0, {}, nullptr});
    }
    void Fill(int x, int y, int w, int h, uint32_t color) { Push({ChromeOp::Fill, x, y, w, h, color, 0, 0, 0, {}, nullptr}); }
    void Rows(int x, int y, int w, int h, const uint32_t* colors) { Push({ChromeOp::Rows, x, y, w, h, 0, 0, 0, 0, {}, colors}); }
    void CornerFill(int x, int y, int w, int h, uint32_t color, const DesktopRect& block, unsigned radius) {
        Push({ChromeOp::CornerFill, x, y, w, h, color, radius, 0, 0, block, nullptr});
    }
    void MaterialOutline(const DesktopRect& r, uint32_t color, unsigned radius) {
        Push({ChromeOp::MaterialOutline, r.x, r.y, r.w, r.h, color, radius, 0, 0, {}, nullptr});
    }
    void MaterialRect(const DesktopRect& r, uint32_t color, unsigned radius) {
        Push({ChromeOp::MaterialRect, r.x, r.y, r.w, r.h, color, radius, 0, 0, {}, nullptr});
    }
    void Squares(int x, int y, int w, int h, int dx, int dy, int count, uint32_t color) {
        if (count > 0) Push({ChromeOp::Squares, x, y, w, h, color, static_cast<uint32_t>(count), dx, dy, {}, nullptr});
    }
    void Icon(int x, int y, int size, const uint32_t* pixels, unsigned opacity = 255) {
        Push({ChromeOp::Icon, x, y, size, size, 0, opacity, 0, 0, {}, pixels});
    }
    // maxWidth <= 0: unbounded. clip: glyph pixels outside it are skipped.
    void Text(int x, int y, const char* text, uint32_t fg, int maxWidth = 0, const DesktopRect* clip = nullptr) {
        uint32_t length = 0;
        while (text[length]) ++length;
        if (!length) return;
        if (textUsed_ + length + 1 > textCapacity_ || count_ >= capacity_) { overflow_ = true; return; }
        const uint32_t offset = textUsed_;
        for (uint32_t i = 0; i < length; ++i) text_[offset + i] = text[i];
        text_[offset + length] = 0;
        textUsed_ += length + 1;
        Push({ChromeOp::Text, x, y, maxWidth, 0, fg, offset, clip ? 1 : 0, 0, clip ? *clip : DesktopRect{}, nullptr});
    }

private:
    void Push(const ChromeCmd& cmd) {
        if (count_ >= capacity_) { overflow_ = true; return; }
        cmds_[count_++] = cmd;
    }
    ChromeCmd* cmds_;
    uint32_t capacity_, count_ = 0;
    char* text_;
    uint32_t textCapacity_, textUsed_ = 0;
    bool overflow_ = false;
};

// Storage for a list: trivially constructible, so it can live in static
// storage (no guarded initialisation) or on the stack.
template <uint32_t Commands, uint32_t TextBytes>
struct ChromeListStorage {
    ChromeCmd cmds[Commands];
    char text[TextBytes];
};
template <uint32_t Commands, uint32_t TextBytes>
inline ChromeDisplayList ChromeListOn(ChromeListStorage<Commands, TextBytes>& storage) {
    return ChromeDisplayList(storage.cmds, Commands, storage.text, TextBytes);
}

// Content key of a list (compositor refactor stage 1.4): a nonzero FNV-1a
// hash of every command, the text it draws and the colours and icon pixels it
// borrows. Executed by the same executor with the same font onto the same
// (cleared) target, two lists with equal keys produce identical pixels, so a
// surface whose key is unchanged need not be rasterised again. seed folds in
// what the list does not carry: the surface kind and rectangle, the target
// size and the font.
inline uint64_t ChromeListKey(const ChromeDisplayList& list, uint64_t seed) {
    constexpr uint64_t prime = 0x100000001b3ull;
    uint64_t h = 0xcbf29ce484222325ull ^ seed;
    auto word = [&](uint64_t v) { h = (h ^ v) * prime; };
    auto pair = [&](int32_t a, int32_t b) { word(uint64_t(uint32_t(a)) << 32 | uint32_t(b)); };
    for (uint32_t i = 0; i < list.Count(); ++i) {
        const ChromeCmd& c = list.At(i);
        word(uint64_t(c.op) << 32 | c.color);
        pair(c.x, c.y); pair(c.w, c.h); pair(c.dx, c.dy);
        pair(c.clip.x, c.clip.y); pair(c.clip.w, c.clip.h);
        if (c.op == ChromeOp::Text) {
            // The arena offset depends on earlier texts; hash the string.
            const char* text = list.TextAt(c.arg);
            uint32_t n = 0;
            for (; text[n]; ++n) word(uint8_t(text[n]));
            word(n);
        } else word(c.arg);
        if (c.op == ChromeOp::Rows && c.data) {
            const uint32_t* rows = static_cast<const uint32_t*>(c.data);
            for (int32_t r = 0; r < c.h; ++r) word(rows[r]);
        } else if (c.op == ChromeOp::Icon) {
            // Icons are 24x24 straight-alpha sources (LAUNCHER_ICON_PX).
            const uint32_t* icon = static_cast<const uint32_t*>(c.data);
            word(icon != nullptr);
            if (icon) for (uint32_t p = 0; p < 24 * 24; p += 2) word(uint64_t(icon[p]) << 32 | icon[p + 1]);
        }
    }
    word(list.Count()); word(list.Overflowed());
    return h ? h : 1;
}

// Budgets: DESIGN.md allows 64 primitives per window frame.
using ChromeFrameStorage = ChromeListStorage<64, 128>;
using ChromeShellStorage = ChromeListStorage<1536, 4096>;

} // namespace brook
