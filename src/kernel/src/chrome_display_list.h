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
    Fill,         // solid rect, replaces the destination
    Rows,         // one colour per row (title material gradient)
    CornerFill,   // solid rect clipped to a rounded top-right corner of clip
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

    void Fill(int x, int y, int w, int h, uint32_t color) { Push({ChromeOp::Fill, x, y, w, h, color, 0, 0, 0, {}, nullptr}); }
    void Rows(int x, int y, int w, int h, const uint32_t* colors) { Push({ChromeOp::Rows, x, y, w, h, 0, 0, 0, 0, {}, colors}); }
    void CornerFill(int x, int y, int w, int h, uint32_t color, const DesktopRect& block, unsigned radius) {
        Push({ChromeOp::CornerFill, x, y, w, h, color, radius, 0, 0, block, nullptr});
    }
    void MaterialRect(const DesktopRect& r, uint32_t color, unsigned radius) {
        Push({ChromeOp::MaterialRect, r.x, r.y, r.w, r.h, color, radius, 0, 0, {}, nullptr});
    }
    void Squares(int x, int y, int w, int h, int dx, int dy, int count, uint32_t color) {
        if (count > 0) Push({ChromeOp::Squares, x, y, w, h, color, static_cast<uint32_t>(count), dx, dy, {}, nullptr});
    }
    void Icon(int x, int y, int size, const uint32_t* pixels) {
        Push({ChromeOp::Icon, x, y, size, size, 0, 0, 0, 0, {}, pixels});
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

// Budgets: DESIGN.md allows 64 primitives per window frame.
using ChromeFrameStorage = ChromeListStorage<64, 128>;
using ChromeShellStorage = ChromeListStorage<1536, 4096>;

} // namespace brook
