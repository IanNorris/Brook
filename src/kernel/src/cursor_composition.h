#pragma once
#include <stdint.h>
#include "../../shared/brook_theme.h"
#include "cursor_shapes.h"

namespace brook {
constexpr uint32_t CursorCapacity = 64;
struct CursorImage {
    uint32_t width = 12, height = 16;
    int32_t hotX = 0, hotY = 0;
    uint32_t pixels[CursorCapacity * CursorCapacity]{};
};

inline bool CursorPixelsHaveAlpha(const uint32_t* pixels, uint32_t width, uint32_t height)
{
    for (uint32_t row = 0; row < height; ++row)
        for (uint32_t col = 0; col < width; ++col)
            if (pixels[row * CursorCapacity + col] >> 24)
                return true;
    return false;
}

// Built-in shapes the compositor shows away from client areas: the arrow,
// and the resize arrows over WM frame edges and the side dock's grip.
enum class CursorShape : uint8_t { Arrow, ResizeH, ResizeV, ResizeNwse, ResizeNesw };

// The shape for resize edges (chrome_layout.h ChromeEdge* bits: 1 left,
// 2 top, 4 right, 8 bottom).
inline constexpr CursorShape CursorShapeForEdges(uint8_t edges)
{
    const bool l = edges & 1, t = edges & 2, r = edges & 4, b = edges & 8;
    if ((l || r) && (t || b)) return (l && t) || (r && b) ? CursorShape::ResizeNwse : CursorShape::ResizeNesw;
    if (l || r) return CursorShape::ResizeH;
    if (t || b) return CursorShape::ResizeV;
    return CursorShape::Arrow;
}

// Fill a resize shape (not the arrow, which has its own bitmap). False for Arrow.
inline bool CursorFillShape(CursorShape shape, CursorImage& image)
{
    const char* const* rows = shape == CursorShape::ResizeH ? CursorBitmapResizeH :
                              shape == CursorShape::ResizeV ? CursorBitmapResizeV :
                              shape == CursorShape::ResizeNwse ? CursorBitmapResizeNwse :
                              shape == CursorShape::ResizeNesw ? CursorBitmapResizeNesw : nullptr;
    if (!rows) return false;
    image.width = image.height = CursorShapeSize;
    image.hotX = image.hotY = static_cast<int32_t>(CursorShapeSize / 2);
    for (uint32_t i = 0; i < CursorCapacity * CursorCapacity; ++i) image.pixels[i] = 0;
    for (uint32_t y = 0; y < CursorShapeSize; ++y)
        for (uint32_t x = 0; x < CursorShapeSize; ++x)
            image.pixels[y * CursorCapacity + x] = rows[y][x] == 'o' ? 0xffffffffu : rows[y][x] == '#' ? 0xff000000u : 0;
    return true;
}

// Whether a client cursor image is shown. It belongs to the window it was
// set for (its creation identity) and is shown only while
// pointer input goes to that window's client area (`hover`, 0 over shell
// surfaces, WM chrome and the desktop); elsewhere the built-in arrow is
// shown. While a button is held the choice is kept, so a drag (a selection,
// a WM resize from the client's edge) keeps the cursor it started with.
// Owner 0 (no window: an image set while the pointer was over the desktop,
// the shell or a window of another process) is never shown. It used to mean
// "everywhere", which put Xwayland's root "X" cursor, set after
// wl_pointer.leave, over the taskbar, the desktop and kernel windows.
struct CursorOwnership {
    uint64_t owner = 0;
    bool showClient = false;
    // A new image from the same owner during a drag keeps the current choice.
    void Set(uint64_t window, uint64_t hover, bool buttonsHeld)
    {
        const bool same = owner == window;
        owner = window;
        if (!buttonsHeld || !same) showClient = owner && owner == hover;
    }
    bool Update(uint64_t hover, bool buttonsHeld)
    {
        if (!buttonsHeld) showClient = owner && owner == hover;
        return showClient;
    }
};

// Scale only compositor-owned logical cursors. Client images are already in
// physical pixels and must not pass through this a second time.
inline void ScaleCursor(const CursorImage& source,CursorImage& frame,uint32_t percent)
{
    if (!percent) percent=100;
    frame={};
    frame.width=(source.width*percent+99)/100;
    frame.height=(source.height*percent+99)/100;
    if(frame.width>CursorCapacity)frame.width=CursorCapacity;
    if(frame.height>CursorCapacity)frame.height=CursorCapacity;
    frame.hotX=static_cast<int32_t>(static_cast<int64_t>(source.hotX)*percent/100);
    frame.hotY=static_cast<int32_t>(static_cast<int64_t>(source.hotY)*percent/100);
    for(uint32_t y=0;y<frame.height;++y)
        for(uint32_t x=0;x<frame.width;++x)
            frame.pixels[y*CursorCapacity+x]=source.pixels[(y*100/percent)*CursorCapacity+x*100/percent];
}

// Compose into a separate frame: client cursor pixels/hotspot survive busy
// animation and are restored verbatim when the last pending launch resolves.
inline void ComposeCursor(const CursorImage& source, CursorImage& frame,
                          bool busy, uint64_t nowMs, uint32_t accent)
{
    frame = source;
    if (!busy) return;
    constexpr int dx[8] = {0, 3, 4, 3, 0, -3, -4, -3};
    constexpr int dy[8] = {-4, -3, 0, 3, 4, 3, 0, -3};
    const uint32_t cx = source.width + 4 < CursorCapacity - 6 ? source.width + 4 : CursorCapacity - 6;
    const uint32_t cy = source.height / 2 + 4 < CursorCapacity - 6 ? source.height / 2 + 4 : CursorCapacity - 6;
    if (frame.width < cx + 6) frame.width = cx + 6;
    if (frame.height < cy + 6) frame.height = cy + 6;
    const unsigned phase = (nowMs / 100) % 8;
    for (unsigned dot = 0; dot < 8; ++dot) {
        const uint32_t x = static_cast<uint32_t>(static_cast<int>(cx) + dx[dot]);
        const uint32_t y = static_cast<uint32_t>(static_cast<int>(cy) + dy[dot]);
        const uint32_t color = dot == phase ? (accent | 0xff000000u) : (BROOK_CURSOR_BUSY_IDLE | 0xff000000u);
        for (uint32_t yy = 0; yy < 2; ++yy)
            for (uint32_t xx = 0; xx < 2; ++xx)
                frame.pixels[(y + yy) * CursorCapacity + x + xx] = color;
    }
}
}
