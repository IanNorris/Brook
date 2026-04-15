// Brook window manager — z-ordered windows with chrome.
//
// Each process gets a Window that wraps its VFB with a title bar, border,
// and close/maximise buttons. The compositor calls WmRenderChrome() after
// blitting client areas to draw the window decorations.

#include "window.h"
#include "process.h"
#include "compositor.h"
#include "font_atlas.h"
#include "serial.h"
#include "rtc.h"
#include "terminal.h"

namespace brook {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static Window   g_windows[WM_MAX_WINDOWS] = {};
static bool     g_wmActive = false;
static int      g_focusedIdx = -1;
static uint8_t  g_nextZOrder = 1;  // 0 = backmost, higher = front

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void WmStrCopy(char* dst, const char* src, uint32_t maxLen)
{
    uint32_t i = 0;
    while (src[i] && i < maxLen - 1) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

// Pixel write into buffer with bounds check
static inline void WmPutPixel(uint32_t* buf, uint32_t stride,
                               uint32_t screenW, uint32_t screenH,
                               int x, int y, uint32_t color)
{
    if (x < 0 || y < 0) return;
    if (static_cast<uint32_t>(x) >= screenW) return;
    if (static_cast<uint32_t>(y) >= screenH) return;
    buf[y * stride + x] = color;
}

// Fill a rectangle
static void WmFillRect(uint32_t* buf, uint32_t stride,
                        uint32_t screenW, uint32_t screenH,
                        int x0, int y0, int w, int h, uint32_t color)
{
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x)
            WmPutPixel(buf, stride, screenW, screenH, x, y, color);
}

// Render a single glyph from g_fontAtlas at (penX, penY) into buffer.
// Returns advance width.
static int WmRenderGlyph(uint32_t* buf, uint32_t stride,
                          uint32_t screenW, uint32_t screenH,
                          int penX, int penY, int code,
                          uint32_t fg, uint32_t bg)
{
    const FontAtlas& fa = g_fontAtlas;
    if (code < static_cast<int>(fa.firstChar) ||
        code >= static_cast<int>(fa.firstChar + fa.glyphCount))
        return 0;

    const GlyphInfo& gi = fa.glyphs[code - static_cast<int>(fa.firstChar)];
    int gw = gi.atlasX1 - gi.atlasX0;
    int gh = gi.atlasY1 - gi.atlasY0;

    int drawX = penX + gi.bearingX;
    int drawY = penY + fa.ascent - gi.bearingY;

    for (int row = 0; row < gh; ++row)
    {
        for (int col = 0; col < gw; ++col)
        {
            uint8_t cov = fa.pixels[(gi.atlasY0 + row) * static_cast<int>(fa.atlasWidth)
                                     + (gi.atlasX0 + col)];
            if (cov == 0) continue;

            int px = drawX + col;
            int py = drawY + row;
            if (px < 0 || py < 0) continue;
            if (static_cast<uint32_t>(px) >= screenW) continue;
            if (static_cast<uint32_t>(py) >= screenH) continue;

            // Alpha blend
            uint32_t c = cov;
            uint32_t ic = 255 - c;
            uint32_t r = ((((fg >> 16) & 0xFF) * c + ((bg >> 16) & 0xFF) * ic) + 128) / 255;
            uint32_t g = ((((fg >> 8) & 0xFF) * c + ((bg >> 8) & 0xFF) * ic) + 128) / 255;
            uint32_t b = (((fg & 0xFF) * c + (bg & 0xFF) * ic) + 128) / 255;

            buf[py * stride + px] = (r << 16) | (g << 8) | b;
        }
    }

    return gi.advance;
}

// Render a string
static void WmRenderString(uint32_t* buf, uint32_t stride,
                            uint32_t screenW, uint32_t screenH,
                            int x, int y, const char* str,
                            uint32_t fg, uint32_t bg)
{
    int penX = x;
    while (*str)
    {
        penX += WmRenderGlyph(buf, stride, screenW, screenH,
                               penX, y, static_cast<uint8_t>(*str), fg, bg);
        ++str;
    }
}

// ---------------------------------------------------------------------------
// API implementation
// ---------------------------------------------------------------------------

void WmInit()
{
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
        g_windows[i].proc = nullptr;
    g_focusedIdx = -1;
    g_nextZOrder = 1;
    SerialPuts("WM: initialised\n");
}

bool WmIsActive()
{
    return g_wmActive;
}

void WmSetActive(bool active)
{
    g_wmActive = active;
    if (active)
        SerialPuts("WM: window manager mode enabled\n");
}

int WmCreateWindow(Process* proc, int16_t x, int16_t y,
                   uint16_t clientW, uint16_t clientH,
                   const char* title, uint8_t upscale)
{
    // Find free slot
    int idx = -1;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
    {
        if (g_windows[i].proc == nullptr)
        {
            idx = static_cast<int>(i);
            break;
        }
    }
    if (idx < 0) return -1;

    Window& w = g_windows[idx];
    w.proc = proc;
    w.x = x;
    w.y = y;
    w.clientW = clientW;
    w.clientH = clientH;
    w.zOrder = g_nextZOrder++;
    w.upscale = (upscale >= 1) ? upscale : 1;
    w.state = WindowState::Normal;
    w.focused = false;
    w.visible = true;
    w.minimized = false;
    w.savedX = x;
    w.savedY = y;
    w.savedW = clientW;
    w.savedH = clientH;
    WmStrCopy(w.title, title ? title : "Window", sizeof(w.title));

    SerialPrintf("WM: created window %d '%s' at (%d,%d) %ux%u scale=%u for pid %u\n",
                 idx, w.title, x, y, clientW, clientH, upscale, proc ? proc->pid : 0);

    // Auto-focus new window
    WmSetFocus(idx);

    return idx;
}

void WmDestroyWindow(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    Window& w = g_windows[idx];
    if (!w.proc) return;

    SerialPrintf("WM: destroyed window %d '%s'\n", idx, w.title);
    w.proc = nullptr;
    w.visible = false;

    if (g_focusedIdx == idx)
    {
        g_focusedIdx = -1;
        // Focus the next highest z-order window
        int bestIdx = -1;
        uint8_t bestZ = 0;
        for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
        {
            if (g_windows[i].proc && g_windows[i].visible && g_windows[i].zOrder >= bestZ)
            {
                bestZ = g_windows[i].zOrder;
                bestIdx = static_cast<int>(i);
            }
        }
        if (bestIdx >= 0) WmSetFocus(bestIdx);
    }
}

void WmDestroyWindowForProcess(Process* proc)
{
    if (!proc) return;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
    {
        if (g_windows[i].proc == proc)
        {
            WmDestroyWindow(static_cast<int>(i));
            return;
        }
    }
}

WmHitResult WmHitTest(int32_t mx, int32_t my)
{
    WmHitResult result = { -1, WmHitZone::None };

    // Collect sorted indices
    int sorted[WM_MAX_WINDOWS];
    uint32_t count = WmGetZOrder(sorted, WM_MAX_WINDOWS);

    // Walk front to back (last in z-order array = frontmost)
    for (int i = static_cast<int>(count) - 1; i >= 0; --i)
    {
        int idx = sorted[i];
        const Window& w = g_windows[idx];
        if (!w.proc || !w.visible || w.minimized) continue;

        int wx = w.x;
        int wy = w.y;
        int ww = w.outerWidth();
        int wh = w.outerHeight();

        // Check if mouse is within outer bounds
        if (mx < wx || my < wy || mx >= wx + ww || my >= wy + wh)
            continue;

        // We hit this window
        result.windowIndex = idx;

        int relX = mx - wx;
        int relY = my - wy;

        // Close button (top-right)
        int closeBtnX = ww - static_cast<int>(WM_BORDER_WIDTH) - static_cast<int>(WM_BUTTON_WIDTH);
        if (relY < static_cast<int>(WM_TITLE_BAR_HEIGHT) &&
            relX >= closeBtnX && relX < closeBtnX + static_cast<int>(WM_BUTTON_WIDTH))
        {
            result.zone = WmHitZone::CloseButton;
            return result;
        }

        // Maximize button (left of close)
        int maxBtnX = closeBtnX - static_cast<int>(WM_BUTTON_WIDTH);
        if (relY < static_cast<int>(WM_TITLE_BAR_HEIGHT) &&
            relX >= maxBtnX && relX < maxBtnX + static_cast<int>(WM_BUTTON_WIDTH))
        {
            result.zone = WmHitZone::MaximizeButton;
            return result;
        }

        // Minimize button (left of maximize)
        int minBtnX = maxBtnX - static_cast<int>(WM_BUTTON_WIDTH);
        if (relY < static_cast<int>(WM_TITLE_BAR_HEIGHT) &&
            relX >= minBtnX && relX < minBtnX + static_cast<int>(WM_BUTTON_WIDTH))
        {
            result.zone = WmHitZone::MinimizeButton;
            return result;
        }

        // Title bar (drag area)
        if (relY < static_cast<int>(WM_TITLE_BAR_HEIGHT))
        {
            result.zone = WmHitZone::TitleBar;
            return result;
        }

        // Resize corner (bottom-right grab zone)
        if (relX >= ww - static_cast<int>(WM_RESIZE_GRAB) &&
            relY >= wh - static_cast<int>(WM_RESIZE_GRAB))
        {
            result.zone = WmHitZone::ResizeCorner;
            return result;
        }

        // Border
        if (relX < static_cast<int>(WM_BORDER_WIDTH) ||
            relX >= ww - static_cast<int>(WM_BORDER_WIDTH) ||
            relY >= wh - static_cast<int>(WM_BORDER_WIDTH))
        {
            result.zone = WmHitZone::Border;
            return result;
        }

        // Client area
        result.zone = WmHitZone::ClientArea;
        return result;
    }

    return result;
}

void WmSetFocus(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    if (!g_windows[idx].proc) return;

    // Unfocus old
    if (g_focusedIdx >= 0 && g_focusedIdx < static_cast<int>(WM_MAX_WINDOWS))
        g_windows[g_focusedIdx].focused = false;

    // Focus and raise new
    g_windows[idx].focused = true;
    g_windows[idx].zOrder = g_nextZOrder++;
    g_focusedIdx = idx;
}

int WmGetFocusedWindow()
{
    return g_focusedIdx;
}

Window* WmGetWindow(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return nullptr;
    if (!g_windows[idx].proc) return nullptr;
    return &g_windows[idx];
}

uint32_t WmWindowCount()
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
        if (g_windows[i].proc) ++n;
    return n;
}

void WmToggleMaximize(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    Window& w = g_windows[idx];
    if (!w.proc) return;

    uint32_t screenW, screenH;
    CompositorGetPhysDims(&screenW, &screenH);

    if (w.state == WindowState::Normal)
    {
        // Save current geometry
        w.savedX = w.x;
        w.savedY = w.y;
        w.savedW = w.clientW;
        w.savedH = w.clientH;

        // Maximize: fill desktop area (screen minus taskbar) minus chrome
        w.x = 0;
        w.y = 0;
        uint32_t desktopH = WmDesktopHeight(screenH);
        w.clientW = static_cast<uint16_t>(screenW - 2 * WM_BORDER_WIDTH);
        w.clientH = static_cast<uint16_t>(desktopH - WM_TITLE_BAR_HEIGHT - 2 * WM_BORDER_WIDTH);
        w.state = WindowState::Maximized;
    }
    else
    {
        // Restore
        w.x = w.savedX;
        w.y = w.savedY;
        w.clientW = w.savedW;
        w.clientH = w.savedH;
        w.state = WindowState::Normal;
    }
}

void WmMoveWindow(int idx, int16_t newX, int16_t newY)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    Window& w = g_windows[idx];
    if (!w.proc) return;
    w.x = newX;
    w.y = newY;
}

void WmMinimizeWindow(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    Window& w = g_windows[idx];
    if (!w.proc || w.minimized) return;

    w.minimized = true;
    SerialPrintf("WM: minimized window %d '%s'\n", idx, w.title);

    // If this was focused, focus the next visible window
    if (g_focusedIdx == idx)
    {
        g_focusedIdx = -1;
        w.focused = false;
        int bestIdx = -1;
        uint8_t bestZ = 0;
        for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
        {
            if (g_windows[i].proc && g_windows[i].visible && !g_windows[i].minimized
                && g_windows[i].zOrder >= bestZ)
            {
                bestZ = g_windows[i].zOrder;
                bestIdx = static_cast<int>(i);
            }
        }
        if (bestIdx >= 0) WmSetFocus(bestIdx);
    }
}

void WmRestoreWindow(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    Window& w = g_windows[idx];
    if (!w.proc || !w.minimized) return;

    w.minimized = false;
    SerialPrintf("WM: restored window %d '%s'\n", idx, w.title);
    WmSetFocus(idx);
}

uint32_t WmGetZOrder(int* outIndices, uint32_t maxOut)
{
    // Collect visible, non-minimized windows
    uint32_t count = 0;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS && count < maxOut; ++i)
    {
        if (g_windows[i].proc && g_windows[i].visible && !g_windows[i].minimized)
            outIndices[count++] = static_cast<int>(i);
    }

    // Simple insertion sort by z-order (ascending = back to front)
    for (uint32_t i = 1; i < count; ++i)
    {
        int key = outIndices[i];
        uint8_t keyZ = g_windows[key].zOrder;
        int j = static_cast<int>(i) - 1;
        while (j >= 0 && g_windows[outIndices[j]].zOrder > keyZ)
        {
            outIndices[j + 1] = outIndices[j];
            --j;
        }
        outIndices[j + 1] = key;
    }

    return count;
}

// ---------------------------------------------------------------------------
// Chrome rendering
// ---------------------------------------------------------------------------

static void RenderWindowChrome(uint32_t* buf, uint32_t stride,
                                uint32_t screenW, uint32_t screenH,
                                const Window& w)
{
    int wx = w.x;
    int wy = w.y;
    int ow = w.outerWidth();
    int oh = w.outerHeight();

    uint32_t borderCol = w.focused ? WM_BORDER_FOCUSED : WM_BORDER_UNFOCUSED;
    uint32_t titleBg   = w.focused ? WM_TITLE_BG_FOCUSED : WM_TITLE_BG_UNFOCUSED;

    // Top border
    WmFillRect(buf, stride, screenW, screenH, wx, wy, ow, WM_BORDER_WIDTH, borderCol);
    // Left border
    WmFillRect(buf, stride, screenW, screenH, wx, wy, WM_BORDER_WIDTH, oh, borderCol);
    // Right border
    WmFillRect(buf, stride, screenW, screenH, wx + ow - WM_BORDER_WIDTH, wy, WM_BORDER_WIDTH, oh, borderCol);
    // Bottom border
    WmFillRect(buf, stride, screenW, screenH, wx, wy + oh - WM_BORDER_WIDTH, ow, WM_BORDER_WIDTH, borderCol);

    // Title bar background (fills between top border and client area)
    int titleX = wx + WM_BORDER_WIDTH;
    int titleY = wy + WM_BORDER_WIDTH;
    int titleW = ow - 2 * WM_BORDER_WIDTH;
    int titleH = WM_TITLE_BAR_HEIGHT; // full height to meet client area
    WmFillRect(buf, stride, screenW, screenH, titleX, titleY, titleW, titleH, titleBg);

    // Title text (vertically centered in title bar)
    int textY = titleY + (titleH - g_fontAtlas.lineHeight) / 2;
    WmRenderString(buf, stride, screenW, screenH,
                   titleX + 6, textY, w.title,
                   WM_TITLE_FG, titleBg);

    // Close button — 'X' character, right-aligned
    int closeBtnX = wx + ow - WM_BORDER_WIDTH - WM_BUTTON_WIDTH;
    WmFillRect(buf, stride, screenW, screenH, closeBtnX, titleY,
               WM_BUTTON_WIDTH, titleH, 0x00C04040); // red tint
    // Center 'X' glyph in button
    int glyphW = 8; // default
    if ('X' >= g_fontAtlas.firstChar && 'X' < g_fontAtlas.firstChar + g_fontAtlas.glyphCount)
        glyphW = g_fontAtlas.glyphs['X' - g_fontAtlas.firstChar].advance;
    int closeGlyphX = closeBtnX + (WM_BUTTON_WIDTH - glyphW) / 2;
    int closeGlyphY = titleY + (titleH - g_fontAtlas.lineHeight) / 2;
    WmRenderGlyph(buf, stride, screenW, screenH,
                  closeGlyphX, closeGlyphY, 'X',
                  0x00FFFFFF, 0x00C04040);

    // Maximize button — box icon, left of close button
    int maxBtnX = closeBtnX - WM_BUTTON_WIDTH;
    uint32_t maxBtnBg = titleBg;
    WmFillRect(buf, stride, screenW, screenH, maxBtnX, titleY,
               WM_BUTTON_WIDTH, titleH, maxBtnBg);
    // Draw a centered square to represent maximize
    int sqS = 10;
    int sqX = maxBtnX + (WM_BUTTON_WIDTH - sqS) / 2;
    int sqY = titleY + (titleH - sqS) / 2;
    // Top edge (thicker)
    WmFillRect(buf, stride, screenW, screenH, sqX, sqY, sqS, 2, WM_TITLE_FG);
    // Bottom edge
    WmFillRect(buf, stride, screenW, screenH, sqX, sqY + sqS - 1, sqS, 1, WM_TITLE_FG);
    // Left edge
    WmFillRect(buf, stride, screenW, screenH, sqX, sqY, 1, sqS, WM_TITLE_FG);
    // Right edge
    WmFillRect(buf, stride, screenW, screenH, sqX + sqS - 1, sqY, 1, sqS, WM_TITLE_FG);

    // Minimize button — underscore icon, left of maximize button
    int minBtnX = maxBtnX - WM_BUTTON_WIDTH;
    WmFillRect(buf, stride, screenW, screenH, minBtnX, titleY,
               WM_BUTTON_WIDTH, titleH, titleBg);
    // Draw a horizontal line at the bottom of the button area
    int lineW = 10;
    int lineX = minBtnX + (WM_BUTTON_WIDTH - lineW) / 2;
    int lineY = titleY + titleH - 6;
    WmFillRect(buf, stride, screenW, screenH, lineX, lineY, lineW, 2, WM_TITLE_FG);
}

void WmRenderChrome(uint32_t* backBuffer, uint32_t stride,
                    uint32_t screenW, uint32_t screenH)
{
    if (!g_wmActive) return;

    int sorted[WM_MAX_WINDOWS];
    uint32_t count = WmGetZOrder(sorted, WM_MAX_WINDOWS);

    for (uint32_t i = 0; i < count; ++i)
    {
        const Window& w = g_windows[sorted[i]];
        RenderWindowChrome(backBuffer, stride, screenW, screenH, w);
    }
}

void WmRenderChromeForWindow(uint32_t* backBuffer, uint32_t stride,
                              uint32_t screenW, uint32_t screenH, int idx)
{
    if (!g_wmActive || idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    const Window& w = g_windows[idx];
    if (!w.proc || !w.visible) return;
    RenderWindowChrome(backBuffer, stride, screenW, screenH, w);
}

// ---------------------------------------------------------------------------
// Taskbar
// ---------------------------------------------------------------------------

static constexpr uint32_t TASKBAR_BTN_WIDTH  = 140;
static constexpr uint32_t TASKBAR_BTN_HEIGHT = 24;
static constexpr uint32_t TASKBAR_PADDING    = 4;
static constexpr uint32_t TASKBAR_SEPARATOR  = 1; // thin line between taskbar and desktop

void WmRenderTaskbar(uint32_t* backBuffer, uint32_t stride,
                     uint32_t screenW, uint32_t screenH,
                     uint64_t /*uptimeMs*/)
{
    if (!g_wmActive || !backBuffer) return;

    uint32_t tbY = screenH - WM_TASKBAR_HEIGHT;

    // Taskbar background
    WmFillRect(backBuffer, stride, screenW, screenH,
               0, static_cast<int>(tbY), static_cast<int>(screenW),
               static_cast<int>(WM_TASKBAR_HEIGHT), WM_TASKBAR_BG);

    // Top separator line
    WmFillRect(backBuffer, stride, screenW, screenH,
               0, static_cast<int>(tbY), static_cast<int>(screenW),
               TASKBAR_SEPARATOR, 0x00404050);

    // Collect all active windows (including minimized) for taskbar buttons
    uint32_t btnX = TASKBAR_PADDING;
    uint32_t btnY = tbY + (WM_TASKBAR_HEIGHT - TASKBAR_BTN_HEIGHT) / 2;
    uint32_t textYOff = (TASKBAR_BTN_HEIGHT - static_cast<uint32_t>(g_fontAtlas.lineHeight)) / 2;

    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
    {
        const Window& w = g_windows[i];
        if (!w.proc || !w.visible) continue;

        // Button background — highlight if focused
        uint32_t btnBg = (w.focused && !w.minimized) ? WM_TASKBAR_BTN_ACTIVE : WM_TASKBAR_BTN_BG;
        WmFillRect(backBuffer, stride, screenW, screenH,
                   static_cast<int>(btnX), static_cast<int>(btnY),
                   TASKBAR_BTN_WIDTH, TASKBAR_BTN_HEIGHT, btnBg);

        // If minimized, draw a subtle underline indicator
        if (w.minimized)
        {
            WmFillRect(backBuffer, stride, screenW, screenH,
                       static_cast<int>(btnX + 2),
                       static_cast<int>(btnY + TASKBAR_BTN_HEIGHT - 2),
                       TASKBAR_BTN_WIDTH - 4, 1, 0x00808080);
        }

        // Truncate title to fit button
        char label[20];
        uint32_t maxChars = (TASKBAR_BTN_WIDTH - 8) / 8; // rough estimate
        if (maxChars > sizeof(label) - 1) maxChars = sizeof(label) - 1;
        uint32_t ti = 0;
        while (ti < maxChars && w.title[ti]) { label[ti] = w.title[ti]; ti++; }
        label[ti] = '\0';

        // Render title text
        WmRenderString(backBuffer, stride, screenW, screenH,
                       static_cast<int>(btnX + 4),
                       static_cast<int>(btnY + textYOff),
                       label, WM_TASKBAR_BTN_FG, btnBg);

        btnX += TASKBAR_BTN_WIDTH + TASKBAR_PADDING;
    }

    // Real-time clock — right-aligned in taskbar
    uint64_t now = RtcNow();
    char clockBuf[32];
    RtcFormatTaskbar(clockBuf, now, false); // no seconds by default

    // Measure clock text width
    const FontAtlas& fa = g_fontAtlas;
    uint32_t clockW = 0;
    for (const char* p = clockBuf; *p; p++)
    {
        int code = static_cast<int>(static_cast<uint8_t>(*p));
        if (code >= static_cast<int>(fa.firstChar) &&
            code < static_cast<int>(fa.firstChar + fa.glyphCount))
            clockW += static_cast<uint32_t>(fa.glyphs[code - static_cast<int>(fa.firstChar)].advance);
    }

    uint32_t clockX = screenW - clockW - TASKBAR_PADDING * 2;
    WmRenderString(backBuffer, stride, screenW, screenH,
                   static_cast<int>(clockX),
                   static_cast<int>(btnY + textYOff),
                   clockBuf, WM_TASKBAR_CLOCK_FG, WM_TASKBAR_BG);
}

int WmTaskbarHitTest(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH)
{
    uint32_t tbY = screenH - WM_TASKBAR_HEIGHT;
    if (my < static_cast<int32_t>(tbY) || mx < 0 || mx >= static_cast<int32_t>(screenW))
        return -1;

    // Walk window buttons left to right
    uint32_t btnX = TASKBAR_PADDING;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
    {
        const Window& w = g_windows[i];
        if (!w.proc || !w.visible) continue;

        uint32_t btnY = tbY + (WM_TASKBAR_HEIGHT - TASKBAR_BTN_HEIGHT) / 2;
        if (mx >= static_cast<int32_t>(btnX) &&
            mx < static_cast<int32_t>(btnX + TASKBAR_BTN_WIDTH) &&
            my >= static_cast<int32_t>(btnY) &&
            my < static_cast<int32_t>(btnY + TASKBAR_BTN_HEIGHT))
        {
            return static_cast<int>(i);
        }
        btnX += TASKBAR_BTN_WIDTH + TASKBAR_PADDING;
    }

    return -1; // clicked taskbar background but not a button
}

uint32_t WmDesktopHeight(uint32_t screenH)
{
    return screenH > WM_TASKBAR_HEIGHT ? screenH - WM_TASKBAR_HEIGHT : screenH;
}

void WmSpawnTerminal()
{
    if (!g_wmActive) return;

    uint32_t clientW = 800;
    uint32_t clientH = 600;

    int termIdx = TerminalCreate(clientW, clientH);
    if (termIdx < 0)
    {
        SerialPuts("WM: failed to spawn new terminal\n");
        return;
    }

    Terminal* t = TerminalGet(termIdx);
    if (t && t->child)
    {
        // Stagger position based on terminal index
        int16_t winX = static_cast<int16_t>(60 + (termIdx % 6) * 40);
        int16_t winY = static_cast<int16_t>(60 + (termIdx % 6) * 40);

        t->child->fbDestX = winX + static_cast<int16_t>(WM_BORDER_WIDTH);
        t->child->fbDestY = winY + static_cast<int16_t>(WM_TITLE_BAR_HEIGHT + WM_BORDER_WIDTH);
        t->child->fbScale = 1;
        t->child->fbDirty = 1;

        WmCreateWindow(t->child, winX, winY,
                       static_cast<uint16_t>(clientW),
                       static_cast<uint16_t>(clientH), "Terminal");

        SerialPrintf("WM: spawned new terminal (bash pid %u)\n", t->child->pid);
    }
}

} // namespace brook
