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
#include "vfs.h"

namespace brook {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static Window   g_windows[WM_MAX_WINDOWS] = {};
static bool     g_wmActive = false;
static int      g_focusedIdx = -1;
static uint8_t  g_nextZOrder = 1;  // 0 = backmost, higher = front

// App launcher state (implementation at bottom of file)
static LauncherItem g_launcherItems[WM_LAUNCHER_MAX_ITEMS] = {};
static uint32_t     g_launcherCount = 0;
static bool         g_launcherOpen  = false;
static bool         g_launcherLoaded = false;

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

int WmFindWindowForProcess(Process* proc)
{
    if (!proc) return -1;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
    {
        if (g_windows[i].proc == proc)
            return static_cast<int>(i);
    }
    return -1;
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

        // Check if mouse is within outer bounds (extended by grab zone for resize)
        int grab = static_cast<int>(WM_RESIZE_EDGE);
        if (mx < wx - grab || my < wy || mx >= wx + ww + grab || my >= wy + wh + grab)
            continue;

        // If click is outside the visual window but inside grab zone,
        // treat as edge resize
        bool outsideRight  = (mx >= wx + ww);
        bool outsideBottom = (my >= wy + wh);
        if (outsideRight || outsideBottom)
        {
            result.windowIndex = idx;
            if (outsideRight && outsideBottom)
                result.zone = WmHitZone::ResizeCorner;
            else if (outsideRight)
                result.zone = WmHitZone::ResizeRight;
            else
                result.zone = WmHitZone::ResizeBottom;
            return result;
        }

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

        // Resize edges (bottom and right, wider than visual border)
        if (relY >= wh - static_cast<int>(WM_RESIZE_EDGE))
        {
            result.zone = WmHitZone::ResizeBottom;
            return result;
        }
        if (relX >= ww - static_cast<int>(WM_RESIZE_EDGE))
        {
            result.zone = WmHitZone::ResizeRight;
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

    uint16_t newW, newH;

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
        newW = static_cast<uint16_t>(screenW - 2 * WM_BORDER_WIDTH);
        newH = static_cast<uint16_t>(desktopH - WM_TITLE_BAR_HEIGHT - 2 * WM_BORDER_WIDTH);
        w.state = WindowState::Maximized;
    }
    else
    {
        // Restore
        w.x = w.savedX;
        w.y = w.savedY;
        newW = w.savedW;
        newH = w.savedH;
        w.state = WindowState::Normal;
    }

    // Route through WmResizeWindow so terminal VFBs / SIGWINCH paths fire.
    // Without this, chrome grows but the inner VFB stays the old size.
    WmResizeWindow(idx, newW, newH);
}

void WmMoveWindow(int idx, int16_t newX, int16_t newY)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    Window& w = g_windows[idx];
    if (!w.proc) return;
    w.x = newX;
    w.y = newY;
}

void WmResizeWindow(int idx, uint16_t newClientW, uint16_t newClientH)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    Window& w = g_windows[idx];
    if (!w.proc) return;

    // Enforce minimum size
    if (newClientW < WM_MIN_WIDTH)  newClientW = WM_MIN_WIDTH;
    if (newClientH < WM_MIN_HEIGHT) newClientH = WM_MIN_HEIGHT;

    // Update window dimensions
    w.clientW = newClientW;
    w.clientH = newClientH;

    // Check if this is a terminal window — resize its VFB
    Terminal* t = TerminalFindByProcess(w.proc);
    if (t)
    {
        TerminalResize(t, newClientW, newClientH);
    }
    else
    {
        // Non-terminal windows: just update dimensions
        // (process VFB stays the same size — upscale will handle it)
    }
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
static constexpr uint32_t TASKBAR_NEW_BTN_W  = 28; // "+" button width
static constexpr uint32_t TASKBAR_APPS_BTN_W = 48; // "Apps" button width

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

    // "Apps" launcher button
    uint32_t appsBg = g_launcherOpen ? WM_TASKBAR_BTN_ACTIVE : 0x00334455;
    WmFillRect(backBuffer, stride, screenW, screenH,
               static_cast<int>(btnX), static_cast<int>(btnY),
               TASKBAR_APPS_BTN_W, TASKBAR_BTN_HEIGHT, appsBg);
    WmRenderString(backBuffer, stride, screenW, screenH,
                   static_cast<int>(btnX + 6),
                   static_cast<int>(btnY + textYOff),
                   "Apps", 0x0088CCFF, appsBg);
    btnX += TASKBAR_APPS_BTN_W + TASKBAR_PADDING;

    // "+" new terminal button
    WmFillRect(backBuffer, stride, screenW, screenH,
               static_cast<int>(btnX), static_cast<int>(btnY),
               TASKBAR_NEW_BTN_W, TASKBAR_BTN_HEIGHT, 0x00334455);
    WmRenderString(backBuffer, stride, screenW, screenH,
                   static_cast<int>(btnX + (TASKBAR_NEW_BTN_W - 8) / 2),
                   static_cast<int>(btnY + textYOff),
                   "+", 0x0088CCFF, 0x00334455);
    btnX += TASKBAR_NEW_BTN_W + TASKBAR_PADDING;

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

    // "Apps" launcher button
    {
        uint32_t btnY2 = tbY + (WM_TASKBAR_HEIGHT - TASKBAR_BTN_HEIGHT) / 2;
        if (mx >= static_cast<int32_t>(btnX) &&
            mx < static_cast<int32_t>(btnX + TASKBAR_APPS_BTN_W) &&
            my >= static_cast<int32_t>(btnY2) &&
            my < static_cast<int32_t>(btnY2 + TASKBAR_BTN_HEIGHT))
        {
            return -3; // special: apps launcher button
        }
        btnX += TASKBAR_APPS_BTN_W + TASKBAR_PADDING;
    }

    // "+" new terminal button
    {
        uint32_t btnY2 = tbY + (WM_TASKBAR_HEIGHT - TASKBAR_BTN_HEIGHT) / 2;
        if (mx >= static_cast<int32_t>(btnX) &&
            mx < static_cast<int32_t>(btnX + TASKBAR_NEW_BTN_W) &&
            my >= static_cast<int32_t>(btnY2) &&
            my < static_cast<int32_t>(btnY2 + TASKBAR_BTN_HEIGHT))
        {
            return -2; // special: new terminal button
        }
        btnX += TASKBAR_NEW_BTN_W + TASKBAR_PADDING;
    }

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

// ---------------------------------------------------------------------------
// App Launcher — popup panel with shortcut items
// ---------------------------------------------------------------------------

// Launcher visual constants
static constexpr uint32_t LAUNCHER_ICON_SIZE    = 20;
static constexpr uint32_t LAUNCHER_ICON_MARGIN  = 8;
static constexpr uint32_t LAUNCHER_ITEM_HEIGHT  = 32;
static constexpr uint32_t LAUNCHER_ITEM_WIDTH   = 230;
static constexpr uint32_t LAUNCHER_PADDING      = 6;
static constexpr uint32_t LAUNCHER_BG           = 0x00252535;
static constexpr uint32_t LAUNCHER_ITEM_BG      = 0x00303045;
static constexpr uint32_t LAUNCHER_ITEM_FG      = 0x00E0E0E0;
static constexpr uint32_t LAUNCHER_BORDER_CLR   = 0x00505060;
static constexpr uint32_t LAUNCHER_HEADER_FG    = 0x0090D0FF;

// Assign an icon color based on the shortcut title
static uint32_t LauncherIconColor(const char* title)
{
    // Known apps get distinctive colors
    for (const char* p = title; *p; ++p)
    {
        char c = (*p >= 'a' && *p <= 'z') ? (*p - 32) : *p;
        if (c == 'D') return 0x00B03030; // DOOM — dark red
        if (c == 'Q') return 0x00806020; // Quake — brown
        if (c == 'N') return 0x002060A0; // NetSurf — blue
        if (c == 'T') return 0x00307030; // Terminal — green
        if (c >= 'A') break; // use first alpha char
    }
    // Fallback: hash title to a muted color
    uint32_t h = 0x811c9dc5;
    for (const char* p = title; *p; ++p)
        h = (h ^ static_cast<uint8_t>(*p)) * 0x01000193;
    return 0x00404040 | ((h & 0x7F) << 16) | (((h >> 8) & 0x7F) << 8) | ((h >> 16) & 0x7F);
}

// Draw a small colored icon with first letter
static void LauncherDrawIcon(uint32_t* backBuffer, uint32_t stride,
                             uint32_t screenW, uint32_t screenH,
                             int32_t x, int32_t y, uint32_t color, char letter)
{
    // Filled rounded-ish rectangle (just a solid square with slightly inset corners)
    for (uint32_t iy = 0; iy < LAUNCHER_ICON_SIZE; iy++)
    {
        for (uint32_t ix = 0; ix < LAUNCHER_ICON_SIZE; ix++)
        {
            // Skip corner pixels for rounded look
            bool corner = (ix == 0 && iy == 0) || (ix == LAUNCHER_ICON_SIZE-1 && iy == 0) ||
                          (ix == 0 && iy == LAUNCHER_ICON_SIZE-1) ||
                          (ix == LAUNCHER_ICON_SIZE-1 && iy == LAUNCHER_ICON_SIZE-1);
            if (corner) continue;

            int32_t px = x + static_cast<int32_t>(ix);
            int32_t py2 = y + static_cast<int32_t>(iy);
            if (px >= 0 && px < static_cast<int32_t>(screenW) &&
                py2 >= 0 && py2 < static_cast<int32_t>(screenH))
                backBuffer[py2 * stride + px] = color;
        }
    }

    // Render the letter centered in the icon
    if (letter >= 'a' && letter <= 'z') letter -= 32; // uppercase
    char str[2] = { letter, '\0' };
    int32_t lx = x + static_cast<int32_t>(LAUNCHER_ICON_SIZE / 2) - 4;
    int32_t ly = y + static_cast<int32_t>(LAUNCHER_ICON_SIZE / 2) -
                 static_cast<int32_t>(g_fontAtlas.lineHeight / 2);
    WmRenderString(backBuffer, stride, screenW, screenH, lx, ly, str, 0x00FFFFFF, color);
}

// Parse "# title: Something" from a script file's first few lines
static bool ParseShortcutTitle(const char* path, char* titleOut, uint32_t titleMax)
{
    Vnode* vn = VfsOpen(path, 0);
    if (!vn) return false;

    char buf[512];
    uint64_t offset = 0;
    int rd = VfsRead(vn, buf, sizeof(buf) - 1, &offset);
    VfsClose(vn);
    if (rd <= 0) return false;
    buf[rd] = '\0';

    // Search for "# title:" in the buffer
    const char* p = buf;
    while (*p)
    {
        // Skip to start of line
        while (*p && (*p == '\n' || *p == '\r')) ++p;
        if (*p != '#') break; // only check comment lines at the top

        // Look for "# title:"
        const char* line = p;
        while (*p && *p != '\n') ++p;

        // Check for "# title:" prefix
        const char* t = line + 1;
        while (*t == ' ') ++t;
        if (t[0] == 't' && t[1] == 'i' && t[2] == 't' && t[3] == 'l' &&
            t[4] == 'e' && t[5] == ':')
        {
            t += 6;
            while (*t == ' ') ++t;
            uint32_t i = 0;
            while (*t && *t != '\n' && *t != '\r' && i < titleMax - 1)
                titleOut[i++] = *t++;
            titleOut[i] = '\0';
            return true;
        }
    }
    return false;
}

void WmLauncherLoad()
{
    if (g_launcherLoaded) return;
    g_launcherLoaded = true;
    g_launcherCount = 0;

    Vnode* dir = VfsOpen("/boot/SHORTCUTS", 0);
    if (!dir)
    {
        SerialPuts("WM: no /boot/SHORTCUTS directory\n");
        return;
    }

    DirEntry de;
    uint32_t cookie = 0;
    while (VfsReaddir(dir, &de, &cookie) == 1 && g_launcherCount < WM_LAUNCHER_MAX_ITEMS)
    {
        if (de.isDir) continue;

        // Build full path
        LauncherItem& item = g_launcherItems[g_launcherCount];
        uint32_t pi = 0;
        const char* prefix = "/boot/SHORTCUTS/";
        while (*prefix && pi < sizeof(item.scriptPath) - 1)
            item.scriptPath[pi++] = *prefix++;
        uint32_t ni = 0;
        while (de.name[ni] && pi < sizeof(item.scriptPath) - 1)
            item.scriptPath[pi++] = de.name[ni++];
        item.scriptPath[pi] = '\0';

        // Try to parse title from the file
        if (!ParseShortcutTitle(item.scriptPath, item.title, sizeof(item.title)))
        {
            // Fallback: use filename without extension
            uint32_t ti = 0;
            for (uint32_t j = 0; de.name[j] && de.name[j] != '.' && ti < sizeof(item.title) - 1; ++j)
                item.title[ti++] = de.name[j];
            item.title[ti] = '\0';
        }

        item.valid = true;
        item.iconColor = LauncherIconColor(item.title);
        SerialPrintf("WM: launcher[%u] = '%s' -> %s\n",
                     g_launcherCount, item.title, item.scriptPath);
        g_launcherCount++;
    }

    VfsClose(dir);
    SerialPrintf("WM: loaded %u launcher shortcuts\n", g_launcherCount);
}

void WmLauncherToggle()
{
    if (!g_launcherLoaded) WmLauncherLoad();
    g_launcherOpen = !g_launcherOpen;
}

bool WmLauncherVisible()
{
    return g_launcherOpen;
}

// Get the launcher panel geometry (anchored above the Apps button on the taskbar)
static void LauncherGetRect(uint32_t screenW, uint32_t screenH,
                            int32_t* outX, int32_t* outY,
                            uint32_t* outW, uint32_t* outH)
{
    uint32_t itemCount = g_launcherCount > 0 ? g_launcherCount : 1;
    uint32_t headerH = LAUNCHER_ITEM_HEIGHT; // "Apps" header row
    uint32_t panelW = LAUNCHER_ITEM_WIDTH + LAUNCHER_PADDING * 2;
    uint32_t panelH = headerH + itemCount * (LAUNCHER_ITEM_HEIGHT + 2) + LAUNCHER_PADDING * 2;

    *outX = static_cast<int32_t>(TASKBAR_PADDING);
    *outY = static_cast<int32_t>(screenH - WM_TASKBAR_HEIGHT - panelH - 2);
    *outW = panelW;
    *outH = panelH;
}

void WmLauncherRender(uint32_t* backBuffer, uint32_t stride,
                      uint32_t screenW, uint32_t screenH)
{
    if (!g_launcherOpen || g_launcherCount == 0) return;

    int32_t px, py;
    uint32_t pw, ph;
    LauncherGetRect(screenW, screenH, &px, &py, &pw, &ph);

    // Panel background
    WmFillRect(backBuffer, stride, screenW, screenH,
               px, py, static_cast<int>(pw), static_cast<int>(ph), LAUNCHER_BG);

    // Border
    WmFillRect(backBuffer, stride, screenW, screenH, px, py, static_cast<int>(pw), 1, LAUNCHER_BORDER_CLR);
    WmFillRect(backBuffer, stride, screenW, screenH, px, py + static_cast<int32_t>(ph) - 1, static_cast<int>(pw), 1, LAUNCHER_BORDER_CLR);
    WmFillRect(backBuffer, stride, screenW, screenH, px, py, 1, static_cast<int>(ph), LAUNCHER_BORDER_CLR);
    WmFillRect(backBuffer, stride, screenW, screenH, px + static_cast<int32_t>(pw) - 1, py, 1, static_cast<int>(ph), LAUNCHER_BORDER_CLR);

    uint32_t textYOff = (LAUNCHER_ITEM_HEIGHT - static_cast<uint32_t>(g_fontAtlas.lineHeight)) / 2;

    // Header: "Apps"
    int32_t iy = py + static_cast<int32_t>(LAUNCHER_PADDING);
    WmRenderString(backBuffer, stride, screenW, screenH,
                   px + static_cast<int32_t>(LAUNCHER_PADDING) + 4,
                   iy + static_cast<int32_t>(textYOff),
                   "Applications", LAUNCHER_HEADER_FG, LAUNCHER_BG);
    iy += LAUNCHER_ITEM_HEIGHT;

    // Items
    for (uint32_t i = 0; i < g_launcherCount; i++)
    {
        if (!g_launcherItems[i].valid) continue;

        int32_t itemX = px + static_cast<int32_t>(LAUNCHER_PADDING);
        int32_t itemY = iy;

        WmFillRect(backBuffer, stride, screenW, screenH,
                   itemX, itemY,
                   static_cast<int>(LAUNCHER_ITEM_WIDTH),
                   static_cast<int>(LAUNCHER_ITEM_HEIGHT),
                   LAUNCHER_ITEM_BG);

        // Draw icon
        int32_t iconX = itemX + 6;
        int32_t iconY = itemY + static_cast<int32_t>((LAUNCHER_ITEM_HEIGHT - LAUNCHER_ICON_SIZE) / 2);
        char firstLetter = g_launcherItems[i].title[0];
        LauncherDrawIcon(backBuffer, stride, screenW, screenH,
                         iconX, iconY, g_launcherItems[i].iconColor, firstLetter);

        // Title text (offset to right of icon)
        int32_t textX = iconX + static_cast<int32_t>(LAUNCHER_ICON_SIZE) +
                        static_cast<int32_t>(LAUNCHER_ICON_MARGIN);
        WmRenderString(backBuffer, stride, screenW, screenH,
                       textX, itemY + static_cast<int32_t>(textYOff),
                       g_launcherItems[i].title,
                       LAUNCHER_ITEM_FG, LAUNCHER_ITEM_BG);

        iy += LAUNCHER_ITEM_HEIGHT + 2;
    }
}

int WmLauncherHitTest(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH)
{
    if (!g_launcherOpen || g_launcherCount == 0) return -1;

    int32_t px, py;
    uint32_t pw, ph;
    LauncherGetRect(screenW, screenH, &px, &py, &pw, &ph);

    // Outside panel?
    if (mx < px || mx >= px + static_cast<int32_t>(pw) ||
        my < py || my >= py + static_cast<int32_t>(ph))
        return -1;

    // Skip header
    int32_t itemStartY = py + static_cast<int32_t>(LAUNCHER_PADDING + LAUNCHER_ITEM_HEIGHT);
    int32_t itemX = px + static_cast<int32_t>(LAUNCHER_PADDING);

    for (uint32_t i = 0; i < g_launcherCount; i++)
    {
        int32_t iy = itemStartY + static_cast<int32_t>(i * (LAUNCHER_ITEM_HEIGHT + 2));
        if (mx >= itemX && mx < itemX + static_cast<int32_t>(LAUNCHER_ITEM_WIDTH) &&
            my >= iy && my < iy + static_cast<int32_t>(LAUNCHER_ITEM_HEIGHT))
        {
            return static_cast<int>(i);
        }
    }

    return -1; // clicked in panel but not on an item (header or padding)
}

void WmLauncherExec(int itemIdx)
{
    if (itemIdx < 0 || itemIdx >= static_cast<int>(g_launcherCount)) return;
    if (!g_launcherItems[itemIdx].valid) return;

    SerialPrintf("WM: launching '%s' via %s\n",
                 g_launcherItems[itemIdx].title,
                 g_launcherItems[itemIdx].scriptPath);

    g_launcherOpen = false;

    // Execute the shortcut script (uses existing shell infrastructure)
    extern int ShellExecScript(const char* path);
    ShellExecScript(g_launcherItems[itemIdx].scriptPath);
}

} // namespace brook
