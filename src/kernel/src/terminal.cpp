#include "terminal.h"
#include "pipe.h"
#include "process.h"
#include "shell.h"
#include "scheduler.h"
#include "compositor.h"
#include "window.h"
#include "font_atlas.h"
#include "input.h"
#include "memory/heap.h"
#include "memory/virtual_memory.h"
#include "serial.h"
#include "serial_writer.h"
#include "vfs.h"

namespace brook {

extern volatile uint64_t g_lapicTickCount;

// ---------------------------------------------------------------------------
// Terminal instances
// ---------------------------------------------------------------------------

static Terminal g_terminals[MAX_TERMINALS] = {};
static uint32_t g_terminalCount = 0;

// ---------------------------------------------------------------------------
// ANSI escape-sequence parser states
// ---------------------------------------------------------------------------

enum class AnsiState : uint8_t
{
    Normal,
    Escape,     // got ESC
    Bracket,    // got ESC[
};

// ---------------------------------------------------------------------------
// Cell grid helpers (scrollback + logical state)
// ---------------------------------------------------------------------------

static void CellFillBlank(Terminal* t, TermCell* row, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
    {
        row[i].ch   = 0;
        row[i].attr = 0;
        row[i].pad  = 0;
        row[i].fg   = t->fgColor;
        row[i].bg   = t->bgColor;
    }
}

// Push the current top cell row into the scrollback ring and shift
// the remaining rows up by one.  Clears the new bottom row.
static void CellShiftUp(Terminal* t)
{
    if (!t->cells || t->rows == 0 || t->cols == 0) return;

    // Only archive to scrollback when on the main screen.  Alt-screen
    // applications (vi, less, htop) own the viewport; we shouldn't
    // capture their redraws as scrollback noise.
    if (t->scrollback && !t->inAltScreen && t->scrollbackRows > 0)
    {
        TermCell* dst = t->scrollback + (t->scrollbackHead * t->cols);
        for (uint32_t i = 0; i < t->cols; i++) dst[i] = t->cells[i];
        t->scrollbackHead = (t->scrollbackHead + 1) % t->scrollbackRows;
        if (t->scrollbackUsed < t->scrollbackRows) t->scrollbackUsed++;
    }

    for (uint32_t y = 0; y + 1 < t->rows; y++)
    {
        for (uint32_t x = 0; x < t->cols; x++)
            t->cells[y * t->cols + x] = t->cells[(y + 1) * t->cols + x];
    }
    CellFillBlank(t, t->cells + (t->rows - 1) * t->cols, t->cols);
}

// Record a printable character into the live cell grid at the cursor.
static void CellPut(Terminal* t, char ch)
{
    if (!t->cells || t->curX >= t->cols || t->curY >= t->rows) return;
    TermCell& c = t->cells[t->curY * t->cols + t->curX];
    c.ch   = ch;
    c.attr = 0;
    c.fg   = t->fgColor;
    c.bg   = t->bgColor;
}

// Blit a single cell (char + fg/bg) at logical row/col into the VFB.
// Used only by scrollback redraw — live rendering still goes through
// the existing TermRenderGlyph fast path.
static void CellRender(Terminal* t, uint32_t row, uint32_t col,
                       const TermCell& c)
{
    if (!t->vfb || !g_fontAtlas.pixels) return;
    const auto advance = g_fontAtlas.glyphs[0].advance;
    int pixX = static_cast<int>(col) * advance;
    int pixY = static_cast<int>(row) * g_fontAtlas.lineHeight;

    // Fill cell background.
    for (int cy = 0; cy < g_fontAtlas.lineHeight; cy++)
    {
        int dy = pixY + cy;
        if (dy < 0 || dy >= static_cast<int>(t->vfbH)) continue;
        for (int cx = 0; cx < advance; cx++)
        {
            int dx = pixX + cx;
            if (dx < 0 || dx >= static_cast<int>(t->vfbW)) continue;
            t->vfb[dy * t->vfbW + dx] = c.bg;
        }
    }

    if (c.ch < 32 || c.ch > 126) return;

    const auto& gi = g_fontAtlas.glyphs[c.ch - 32];
    int glyphW = gi.atlasX1 - gi.atlasX0;
    int glyphH = gi.atlasY1 - gi.atlasY0;
    int baseY  = pixY + g_fontAtlas.ascent;
    int drawX  = pixX + gi.bearingX;
    int drawY  = baseY - gi.bearingY;

    for (int gy = 0; gy < glyphH; gy++)
    {
        int dy = drawY + gy;
        if (dy < 0 || dy >= static_cast<int>(t->vfbH)) continue;
        for (int gx = 0; gx < glyphW; gx++)
        {
            int dx = drawX + gx;
            if (dx < 0 || dx >= static_cast<int>(t->vfbW)) continue;
            uint8_t alpha = g_fontAtlas.pixels[
                (gi.atlasY0 + gy) * g_fontAtlas.atlasWidth + gi.atlasX0 + gx];
            if (alpha == 0) continue;
            uint32_t fg = c.fg;
            if (alpha == 255)
                t->vfb[dy * t->vfbW + dx] = fg;
            else
            {
                uint32_t bg = t->vfb[dy * t->vfbW + dx];
                uint32_t rr = ((fg >> 16) & 0xFF) * alpha + ((bg >> 16) & 0xFF) * (255 - alpha);
                uint32_t gg = ((fg >>  8) & 0xFF) * alpha + ((bg >>  8) & 0xFF) * (255 - alpha);
                uint32_t bb = ((fg      ) & 0xFF) * alpha + ((bg      ) & 0xFF) * (255 - alpha);
                t->vfb[dy * t->vfbW + dx] = ((rr / 255) << 16) | ((gg / 255) << 8) | (bb / 255);
            }
        }
    }
}

// Redraw the entire VFB from the cell grid + scrollback according to
// the current viewOffset.  Called after wheel events and when returning
// to the live viewport after having scrolled back.
static void CellRedrawAll(Terminal* t)
{
    if (!t->vfb || !t->cells) return;

    // Clear VFB to background.
    for (uint32_t i = 0; i < t->vfbW * t->vfbH; i++)
        t->vfb[i] = t->bgColor;

    uint32_t off = t->viewOffset;
    if (off > t->scrollbackUsed) off = t->scrollbackUsed;

    for (uint32_t r = 0; r < t->rows; r++)
    {
        if (r < off)
        {
            // Scrollback rows fill the top.  The on-screen row r shows the
            // scrollback row at index (scrollbackUsed - off + r).
            uint32_t sbIdx = t->scrollbackUsed - off + r;
            uint32_t ringIdx = (t->scrollbackHead + t->scrollbackRows
                              - t->scrollbackUsed + sbIdx) % t->scrollbackRows;
            const TermCell* sbRow = t->scrollback + ringIdx * t->cols;
            for (uint32_t c = 0; c < t->cols; c++)
                CellRender(t, r, c, sbRow[c]);
        }
        else
        {
            uint32_t liveRow = r - off;
            const TermCell* row = t->cells + liveRow * t->cols;
            for (uint32_t c = 0; c < t->cols; c++)
                CellRender(t, r, c, row[c]);
        }
    }
    t->dirty = true;
}

// ---------------------------------------------------------------------------
// Glyph rendering into the terminal VFB
// ---------------------------------------------------------------------------

static void TermRenderGlyph(Terminal* t, char ch)
{
    if (!t->vfb || !g_fontAtlas.pixels) return;
    // While the user is viewing scrollback, we mustn't clobber the VFB
    // with live output — but we must still update the logical cell grid
    // so that snapping back to live shows the new content.
    const bool frozen = (t->viewOffset != 0);

    // Handle control characters
    if (ch == '\n')
    {
        t->curX = 0;
        t->curY++;
        if (t->curY >= t->rows)
        {
            CellShiftUp(t);
            if (!frozen)
            {
                uint32_t lineH = static_cast<uint32_t>(g_fontAtlas.lineHeight);
                uint32_t scrollBytes = t->vfbW * (t->vfbH - lineH);
                auto* dst = reinterpret_cast<uint8_t*>(t->vfb);
                auto* src = dst + t->vfbW * lineH * 4;
                for (uint32_t i = 0; i < scrollBytes * 4; i++)
                    dst[i] = src[i];
                uint32_t clearStart = t->vfbW * (t->vfbH - lineH);
                for (uint32_t i = clearStart; i < t->vfbW * t->vfbH; i++)
                    t->vfb[i] = t->bgColor;
            }
            t->curY = t->rows - 1;
        }
        t->dirty = true;
        return;
    }
    if (ch == '\r')
    {
        t->curX = 0;
        return;
    }
    if (ch == '\t')
    {
        t->curX = (t->curX + 8) & ~7;
        if (t->curX >= t->cols) { t->curX = 0; t->curY++; }
        return;
    }
    if (ch == '\b')
    {
        if (t->curX > 0) t->curX--;
        return;
    }

    // Printable character — render glyph
    if (ch < 32 || ch > 126) return;

    // Update logical cell grid (authoritative for scrollback redraw).
    CellPut(t, ch);

    if (!frozen)
    {
    const auto& gi = g_fontAtlas.glyphs[ch - 32];
    int glyphW = gi.atlasX1 - gi.atlasX0;
    int glyphH = gi.atlasY1 - gi.atlasY0;

    int pixX = static_cast<int>(t->curX) * g_fontAtlas.glyphs[0].advance;
    int pixY = static_cast<int>(t->curY) * g_fontAtlas.lineHeight;
    int baseY = pixY + g_fontAtlas.ascent;

    // Clear cell background
    for (int cy = 0; cy < g_fontAtlas.lineHeight; cy++)
    {
        int dy = pixY + cy;
        if (dy < 0 || dy >= static_cast<int>(t->vfbH)) continue;
        for (int cx = 0; cx < g_fontAtlas.glyphs[0].advance; cx++)
        {
            int dx = pixX + cx;
            if (dx < 0 || dx >= static_cast<int>(t->vfbW)) continue;
            t->vfb[dy * t->vfbW + dx] = t->bgColor;
        }
    }

    // Render glyph (alpha-blended)
    int drawX = pixX + gi.bearingX;
    int drawY = baseY - gi.bearingY;

    for (int gy = 0; gy < glyphH; gy++)
    {
        int dy = drawY + gy;
        if (dy < 0 || dy >= static_cast<int>(t->vfbH)) continue;
        for (int gx = 0; gx < glyphW; gx++)
        {
            int dx = drawX + gx;
            if (dx < 0 || dx >= static_cast<int>(t->vfbW)) continue;

            uint8_t alpha = g_fontAtlas.pixels[
                (gi.atlasY0 + gy) * g_fontAtlas.atlasWidth + gi.atlasX0 + gx];
            if (alpha == 0) continue;

            uint32_t fg = t->fgColor;
            if (alpha == 255)
            {
                t->vfb[dy * t->vfbW + dx] = fg;
            }
            else
            {
                uint32_t bg = t->vfb[dy * t->vfbW + dx];
                uint32_t rr = ((fg >> 16) & 0xFF) * alpha + ((bg >> 16) & 0xFF) * (255 - alpha);
                uint32_t gg = ((fg >>  8) & 0xFF) * alpha + ((bg >>  8) & 0xFF) * (255 - alpha);
                uint32_t bb = ((fg      ) & 0xFF) * alpha + ((bg      ) & 0xFF) * (255 - alpha);
                t->vfb[dy * t->vfbW + dx] = ((rr / 255) << 16) | ((gg / 255) << 8) | (bb / 255);
            }
        }
    }
    } // end !frozen

    t->curX++;
    if (t->curX >= t->cols)
    {
        t->curX = 0;
        t->curY++;
        if (t->curY >= t->rows)
        {
            CellShiftUp(t);
            if (!frozen)
            {
                uint32_t lineH = static_cast<uint32_t>(g_fontAtlas.lineHeight);
                uint32_t scrollBytes = t->vfbW * (t->vfbH - lineH);
                auto* dst = reinterpret_cast<uint8_t*>(t->vfb);
                auto* src = dst + t->vfbW * lineH * 4;
                for (uint32_t i = 0; i < scrollBytes * 4; i++)
                    dst[i] = src[i];
                uint32_t clearStart = t->vfbW * (t->vfbH - lineH);
                for (uint32_t i = clearStart; i < t->vfbW * t->vfbH; i++)
                    t->vfb[i] = t->bgColor;
            }
            t->curY = t->rows - 1;
        }
    }
    t->dirty = true;
}

// ---------------------------------------------------------------------------
// ANSI CSI parameter parsing
// ---------------------------------------------------------------------------

static void TermHandleCSI(Terminal* t, const char* params, int paramLen, char finalChar)
{
    // Parse up to 8 numeric parameters separated by ';'
    int args[8] = {};
    int argCount = 0;
    int val = 0;
    bool hasVal = false;

    for (int i = 0; i < paramLen && argCount < 8; i++)
    {
        if (params[i] >= '0' && params[i] <= '9')
        {
            val = val * 10 + (params[i] - '0');
            hasVal = true;
        }
        else if (params[i] == ';')
        {
            args[argCount++] = hasVal ? val : 0;
            val = 0;
            hasVal = false;
        }
    }
    if (hasVal && argCount < 8) args[argCount++] = val;

    switch (finalChar)
    {
    case 'H': // Cursor position
    case 'f':
    {
        int row = (argCount >= 1 && args[0] > 0) ? args[0] - 1 : 0;
        int col = (argCount >= 2 && args[1] > 0) ? args[1] - 1 : 0;
        if (row >= 0) t->curY = static_cast<uint32_t>(row);
        if (col >= 0) t->curX = static_cast<uint32_t>(col);
        break;
    }
    case 'J': // Erase in display
    {
        int mode = (argCount >= 1) ? args[0] : 0;
        uint32_t lineH = static_cast<uint32_t>(g_fontAtlas.lineHeight);
        uint32_t glyphW = static_cast<uint32_t>(g_fontAtlas.glyphs[0].advance);

        if (mode == 0)
        {
            // Clear from cursor to end of screen
            uint32_t pixY = t->curY * lineH;
            uint32_t pixX = t->curX * glyphW;
            // Clear rest of current line
            for (uint32_t y = pixY; y < pixY + lineH && y < t->vfbH; y++)
                for (uint32_t x = pixX; x < t->vfbW; x++)
                    t->vfb[y * t->vfbW + x] = t->bgColor;
            // Clear all lines below
            uint32_t startRow = (pixY + lineH < t->vfbH) ? pixY + lineH : t->vfbH;
            for (uint32_t i = startRow * t->vfbW; i < t->vfbW * t->vfbH; i++)
                t->vfb[i] = t->bgColor;
            // Mirror to cells.
            if (t->cells)
            {
                for (uint32_t x = t->curX; x < t->cols; x++)
                    CellFillBlank(t, &t->cells[t->curY * t->cols + x], 1);
                for (uint32_t y = t->curY + 1; y < t->rows; y++)
                    CellFillBlank(t, &t->cells[y * t->cols], t->cols);
            }
            t->dirty = true;
        }
        else if (mode == 1)
        {
            // Clear from start to cursor
            uint32_t pixY = t->curY * lineH;
            uint32_t pixX = t->curX * glyphW;
            // Clear all lines above
            for (uint32_t i = 0; i < pixY * t->vfbW; i++)
                t->vfb[i] = t->bgColor;
            // Clear current line up to cursor
            for (uint32_t y = pixY; y < pixY + lineH && y < t->vfbH; y++)
                for (uint32_t x = 0; x <= pixX && x < t->vfbW; x++)
                    t->vfb[y * t->vfbW + x] = t->bgColor;
            if (t->cells)
            {
                for (uint32_t y = 0; y < t->curY; y++)
                    CellFillBlank(t, &t->cells[y * t->cols], t->cols);
                for (uint32_t x = 0; x <= t->curX && x < t->cols; x++)
                    CellFillBlank(t, &t->cells[t->curY * t->cols + x], 1);
            }
            t->dirty = true;
        }
        else if (mode == 2 || mode == 3)
        {
            // Clear entire screen
            for (uint32_t i = 0; i < t->vfbW * t->vfbH; i++)
                t->vfb[i] = t->bgColor;
            if (t->cells)
                CellFillBlank(t, t->cells, t->cols * t->rows);
            t->curX = 0;
            t->curY = 0;
            t->dirty = true;
        }
        break;
    }
    case 'K': // Erase in line
    {
        int mode = (argCount >= 1) ? args[0] : 0;
        uint32_t lineH = static_cast<uint32_t>(g_fontAtlas.lineHeight);
        uint32_t pixY = t->curY * lineH;
        uint32_t pixX = t->curX * static_cast<uint32_t>(g_fontAtlas.glyphs[0].advance);
        if (mode == 0) // clear from cursor to end of line
        {
            for (uint32_t y = pixY; y < pixY + lineH && y < t->vfbH; y++)
                for (uint32_t x = pixX; x < t->vfbW; x++)
                    t->vfb[y * t->vfbW + x] = t->bgColor;
            if (t->cells)
                for (uint32_t x = t->curX; x < t->cols; x++)
                    CellFillBlank(t, &t->cells[t->curY * t->cols + x], 1);
        }
        else if (mode == 1) // clear from start to cursor
        {
            for (uint32_t y = pixY; y < pixY + lineH && y < t->vfbH; y++)
                for (uint32_t x = 0; x <= pixX && x < t->vfbW; x++)
                    t->vfb[y * t->vfbW + x] = t->bgColor;
            if (t->cells)
                for (uint32_t x = 0; x <= t->curX && x < t->cols; x++)
                    CellFillBlank(t, &t->cells[t->curY * t->cols + x], 1);
        }
        else if (mode == 2) // clear entire line
        {
            for (uint32_t y = pixY; y < pixY + lineH && y < t->vfbH; y++)
                for (uint32_t x = 0; x < t->vfbW; x++)
                    t->vfb[y * t->vfbW + x] = t->bgColor;
            if (t->cells)
                CellFillBlank(t, &t->cells[t->curY * t->cols], t->cols);
        }
        t->dirty = true;
        break;
    }
    case 'A': // Cursor up
    {
        int n = (argCount >= 1 && args[0] > 0) ? args[0] : 1;
        if (t->curY >= static_cast<uint32_t>(n)) t->curY -= n;
        else t->curY = 0;
        break;
    }
    case 'B': // Cursor down
    {
        int n = (argCount >= 1 && args[0] > 0) ? args[0] : 1;
        t->curY += n;
        if (t->curY >= t->rows) t->curY = t->rows - 1;
        break;
    }
    case 'C': // Cursor forward
    {
        int n = (argCount >= 1 && args[0] > 0) ? args[0] : 1;
        t->curX += n;
        if (t->curX >= t->cols) t->curX = t->cols - 1;
        break;
    }
    case 'D': // Cursor back
    {
        int n = (argCount >= 1 && args[0] > 0) ? args[0] : 1;
        if (t->curX >= static_cast<uint32_t>(n)) t->curX -= n;
        else t->curX = 0;
        break;
    }
    case 'G': // Cursor horizontal absolute
    {
        int col = (argCount >= 1 && args[0] > 0) ? args[0] - 1 : 0;
        t->curX = static_cast<uint32_t>(col);
        if (t->curX >= t->cols) t->curX = t->cols - 1;
        break;
    }
    case 'd': // Cursor vertical absolute
    {
        int row = (argCount >= 1 && args[0] > 0) ? args[0] - 1 : 0;
        t->curY = static_cast<uint32_t>(row);
        if (t->curY >= t->rows) t->curY = t->rows - 1;
        break;
    }
    case 'L': // Insert lines
    {
        int n = (argCount >= 1 && args[0] > 0) ? args[0] : 1;
        uint32_t lineH = static_cast<uint32_t>(g_fontAtlas.lineHeight);
        uint32_t pixY = t->curY * lineH;
        uint32_t shiftPx = static_cast<uint32_t>(n) * lineH;
        // Shift lines down
        if (pixY + shiftPx < t->vfbH)
        {
            for (int y = static_cast<int>(t->vfbH) - 1; y >= static_cast<int>(pixY + shiftPx); y--)
                for (uint32_t x = 0; x < t->vfbW; x++)
                    t->vfb[y * t->vfbW + x] = t->vfb[(y - shiftPx) * t->vfbW + x];
        }
        // Clear inserted lines
        uint32_t clearEnd = pixY + shiftPx;
        if (clearEnd > t->vfbH) clearEnd = t->vfbH;
        for (uint32_t y = pixY; y < clearEnd; y++)
            for (uint32_t x = 0; x < t->vfbW; x++)
                t->vfb[y * t->vfbW + x] = t->bgColor;
        t->dirty = true;
        break;
    }
    case 'M': // Delete lines
    {
        int n = (argCount >= 1 && args[0] > 0) ? args[0] : 1;
        uint32_t lineH = static_cast<uint32_t>(g_fontAtlas.lineHeight);
        uint32_t pixY = t->curY * lineH;
        uint32_t shiftPx = static_cast<uint32_t>(n) * lineH;
        // Shift lines up
        for (uint32_t y = pixY; y + shiftPx < t->vfbH; y++)
            for (uint32_t x = 0; x < t->vfbW; x++)
                t->vfb[y * t->vfbW + x] = t->vfb[(y + shiftPx) * t->vfbW + x];
        // Clear vacated lines at bottom
        uint32_t clearStart = t->vfbH > shiftPx ? t->vfbH - shiftPx : 0;
        for (uint32_t y = clearStart; y < t->vfbH; y++)
            for (uint32_t x = 0; x < t->vfbW; x++)
                t->vfb[y * t->vfbW + x] = t->bgColor;
        t->dirty = true;
        break;
    }
    case 'P': // Delete characters
    {
        int n = (argCount >= 1 && args[0] > 0) ? args[0] : 1;
        uint32_t lineH = static_cast<uint32_t>(g_fontAtlas.lineHeight);
        uint32_t glyphW = static_cast<uint32_t>(g_fontAtlas.glyphs[0].advance);
        uint32_t pixY = t->curY * lineH;
        uint32_t pixX = t->curX * glyphW;
        uint32_t shiftPx = static_cast<uint32_t>(n) * glyphW;
        for (uint32_t y = pixY; y < pixY + lineH && y < t->vfbH; y++)
        {
            for (uint32_t x = pixX; x + shiftPx < t->vfbW; x++)
                t->vfb[y * t->vfbW + x] = t->vfb[y * t->vfbW + x + shiftPx];
            for (uint32_t x = (t->vfbW > shiftPx ? t->vfbW - shiftPx : 0); x < t->vfbW; x++)
                t->vfb[y * t->vfbW + x] = t->bgColor;
        }
        t->dirty = true;
        break;
    }
    case '@': // Insert characters
    {
        int n = (argCount >= 1 && args[0] > 0) ? args[0] : 1;
        uint32_t lineH = static_cast<uint32_t>(g_fontAtlas.lineHeight);
        uint32_t glyphW = static_cast<uint32_t>(g_fontAtlas.glyphs[0].advance);
        uint32_t pixY = t->curY * lineH;
        uint32_t pixX = t->curX * glyphW;
        uint32_t shiftPx = static_cast<uint32_t>(n) * glyphW;
        for (uint32_t y = pixY; y < pixY + lineH && y < t->vfbH; y++)
        {
            for (int x = static_cast<int>(t->vfbW) - 1; x >= static_cast<int>(pixX + shiftPx); x--)
                t->vfb[y * t->vfbW + x] = t->vfb[y * t->vfbW + x - shiftPx];
            for (uint32_t x = pixX; x < pixX + shiftPx && x < t->vfbW; x++)
                t->vfb[y * t->vfbW + x] = t->bgColor;
        }
        t->dirty = true;
        break;
    }
    case 'r': // Set scrolling region (DECSTBM) — accept but don't implement scroll region
        break;
    case 'm': // SGR — Select Graphic Rendition
    {
        if (argCount == 0) { t->fgColor = 0x00CCCCCC; t->bgColor = 0x00001A2A; break; }
        for (int i = 0; i < argCount; i++)
        {
            int code = args[i];
            if (code == 0) { t->fgColor = 0x00CCCCCC; t->bgColor = 0x00001A2A; }
            else if (code == 1) { /* bold — brighten fg */ t->fgColor |= 0x00404040; }
            else if (code == 30) t->fgColor = 0x00000000;
            else if (code == 31) t->fgColor = 0x00CC0000;
            else if (code == 32) t->fgColor = 0x0000CC00;
            else if (code == 33) t->fgColor = 0x00CCCC00;
            else if (code == 34) t->fgColor = 0x000000CC;
            else if (code == 35) t->fgColor = 0x00CC00CC;
            else if (code == 36) t->fgColor = 0x0000CCCC;
            else if (code == 37) t->fgColor = 0x00CCCCCC;
            else if (code == 39) t->fgColor = 0x00CCCCCC; // default fg
            else if (code == 40) t->bgColor = 0x00000000;
            else if (code == 41) t->bgColor = 0x00CC0000;
            else if (code == 42) t->bgColor = 0x0000CC00;
            else if (code == 43) t->bgColor = 0x00CCCC00;
            else if (code == 44) t->bgColor = 0x000000CC;
            else if (code == 45) t->bgColor = 0x00CC00CC;
            else if (code == 46) t->bgColor = 0x0000CCCC;
            else if (code == 47) t->bgColor = 0x00CCCCCC;
            else if (code == 49) t->bgColor = 0x00001A2A; // default bg
            // Bright colors (90-97, 100-107)
            else if (code >= 90 && code <= 97)
            {
                static const uint32_t bright[] = {
                    0x00555555, 0x00FF5555, 0x0055FF55, 0x00FFFF55,
                    0x005555FF, 0x00FF55FF, 0x0055FFFF, 0x00FFFFFF
                };
                t->fgColor = bright[code - 90];
            }
        }
        break;
    }
    case 'h': // Set mode (DEC private if params start with '?')
    case 'l': // Reset mode
    {
        bool isSet = (finalChar == 'h');
        bool isPrivate = (paramLen > 0 && params[0] == '?');
        // Re-parse skipping leading '?'
        int pStart = isPrivate ? 1 : 0;
        int mode = 0;
        for (int i = pStart; i < paramLen; i++)
        {
            if (params[i] >= '0' && params[i] <= '9')
                mode = mode * 10 + (params[i] - '0');
        }
        if (isPrivate)
        {
            if (mode == 1049)
            {
                // Alternate screen buffer
                if (isSet && !t->inAltScreen)
                {
                    // Save main screen and cursor, allocate alt buffer
                    uint32_t sz = t->vfbW * t->vfbH;
                    t->altVfb = static_cast<uint32_t*>(kmalloc(sz * 4));
                    if (t->altVfb)
                    {
                        for (uint32_t i = 0; i < sz; i++)
                            t->altVfb[i] = t->vfb[i];
                        t->savedCurX = t->curX;
                        t->savedCurY = t->curY;
                        // Clear screen for alt buffer
                        for (uint32_t i = 0; i < sz; i++)
                            t->vfb[i] = t->bgColor;
                        t->curX = 0;
                        t->curY = 0;
                        t->inAltScreen = true;
                        t->dirty = true;
                    }
                }
                else if (!isSet && t->inAltScreen)
                {
                    // Restore main screen
                    if (t->altVfb)
                    {
                        uint32_t sz = t->vfbW * t->vfbH;
                        for (uint32_t i = 0; i < sz; i++)
                            t->vfb[i] = t->altVfb[i];
                        kfree(t->altVfb);
                        t->altVfb = nullptr;
                    }
                    t->curX = t->savedCurX;
                    t->curY = t->savedCurY;
                    t->inAltScreen = false;
                    t->dirty = true;
                }
            }
            // mode 25: cursor visibility — silently accept
            // mode 7: auto-wrap — silently accept
        }
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Terminal thread function — reads from stdout pipe and renders
// ---------------------------------------------------------------------------

static void TerminalThreadFn(void* arg)
{
    auto* t = static_cast<Terminal*>(arg);
    auto* outPipe = static_cast<PipeBuffer*>(t->stdoutPipe);

    Process* self = ProcessCurrent();
    AnsiState ansiState = AnsiState::Normal;
    char csiBuf[32];
    int csiLen = 0;

    // Clear VFB to background color
    for (uint32_t i = 0; i < t->vfbW * t->vfbH; i++)
        t->vfb[i] = t->bgColor;
    t->dirty = true;

    SerialPrintf("TERMINAL: thread started for pid %u, reading from pipe\n", self->pid);

    while (t->active)
    {
        char buf[256];

        // Register as reader waiter so pipe writers wake us immediately
        outPipe->readerWaiter = self;
        uint32_t n = outPipe->read(buf, sizeof(buf));

        if (n == 0)
        {
            // Check if child still alive — use a brief delay to avoid
            // racing with transient pipe-empty conditions (e.g. child
            // forked a subprocess that just closed its write end).
            self->wakeupTick = g_lapicTickCount + 50;
            SchedulerBlock(self);

            // Re-check after waking — only close if bash is truly gone
            if (t->child && t->child->state == ProcessState::Terminated)
            {
                SerialPrintf("TERMINAL: child exited (pid=%u status=%d), closing\n",
                             t->child->pid, t->child->exitStatus);
                break;
            }
            continue;
        }

        outPipe->readerWaiter = nullptr;

        // Mirror raw terminal output (bash stdout/stderr) to the serial log
        // so post-mortem debugging can see what the user saw on screen,
        // including ANSI escape sequences. The async writer is non-blocking
        // and drops excess if the ring fills, which is fine for a mirror.
        brook::SerialWriterEnqueue(buf, n);

        // Process each byte
        for (uint32_t i = 0; i < n; i++)
        {
            char ch = buf[i];

            switch (ansiState)
            {
            case AnsiState::Normal:
                if (ch == '\033') // ESC
                    ansiState = AnsiState::Escape;
                else
                    TermRenderGlyph(t, ch);
                break;

            case AnsiState::Escape:
                if (ch == '[')
                {
                    ansiState = AnsiState::Bracket;
                    csiLen = 0;
                }
                else
                {
                    // Unknown escape — ignore and return to normal
                    ansiState = AnsiState::Normal;
                }
                break;

            case AnsiState::Bracket:
                if ((ch >= '0' && ch <= '9') || ch == ';' || ch == '?')
                {
                    if (csiLen < static_cast<int>(sizeof(csiBuf) - 1))
                        csiBuf[csiLen++] = ch;
                }
                else
                {
                    // Final character
                    TermHandleCSI(t, csiBuf, csiLen, ch);
                    ansiState = AnsiState::Normal;
                }
                break;
            }
        }

        if (t->dirty)
        {
            Process* child = t->child;
            if (child && child->state != ProcessState::Terminated)
                child->fbDirty = 1;
            t->dirty = false;
            CompositorWake();
        }
    }

    // Terminal thread is exiting — detach from compositor.
    // Clear child's VFB pointer so compositor won't blit stale memory.
    Process* child = t->child;
    if (child)
    {
        __atomic_store_n(&child->fbVirtual, static_cast<uint32_t*>(nullptr), __ATOMIC_RELEASE);
        child->fbVfbWidth = 0;
    }

    t->active = false;
    DbgPrintf("TERMINAL: thread exiting\n");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int TerminalCreate(uint32_t clientW, uint32_t clientH)
{
    SerialPrintf("TERMINAL: creating %ux%u\n", clientW, clientH);

    // Find a free slot (reuse closed terminals)
    int idx = -1;
    for (uint32_t i = 0; i < MAX_TERMINALS; i++)
    {
        if (!g_terminals[i].active && !g_terminals[i].vfb)
        {
            idx = static_cast<int>(i);
            break;
        }
    }
    if (idx < 0) { SerialPrintf("TERMINAL: max terminals reached\n"); return -1; }
    Terminal* t = &g_terminals[idx];

    // Allocate VFB
    uint32_t vfbSize = clientW * clientH * 4;
    t->vfb = static_cast<uint32_t*>(kmalloc(vfbSize));
    if (!t->vfb) { SerialPrintf("TERMINAL: vfb alloc failed (%u bytes)\n", vfbSize); return -1; }

    t->vfbW = clientW;
    t->vfbH = clientH;
    t->cols = clientW / static_cast<uint32_t>(g_fontAtlas.glyphs[0].advance);
    t->rows = clientH / static_cast<uint32_t>(g_fontAtlas.lineHeight);
    t->curX = 0;
    t->curY = 0;
    t->fgColor = 0x00CCCCCC;
    t->bgColor = 0x001A1A2E;
    t->active = true;
    t->dirty = false;

    // Allocate cell grid + scrollback ring.
    t->cells = nullptr;
    t->scrollback = nullptr;
    t->scrollbackRows = 0;
    t->scrollbackHead = 0;
    t->scrollbackUsed = 0;
    t->viewOffset = 0;
    if (t->cols > 0 && t->rows > 0)
    {
        t->cells = static_cast<TermCell*>(
            kmalloc(t->cols * t->rows * sizeof(TermCell)));
        if (t->cells) CellFillBlank(t, t->cells, t->cols * t->rows);

        t->scrollbackRows = TERM_SCROLLBACK_ROWS;
        t->scrollback = static_cast<TermCell*>(
            kmalloc(t->scrollbackRows * t->cols * sizeof(TermCell)));
        if (!t->scrollback)
        {
            t->scrollbackRows = 0;
        }
    }

    // Create pipes
    auto* stdinPipe = PipeBufferCreate(PIPE_BUF_DEFAULT_SIZE);
    auto* stdoutPipe = PipeBufferCreate(PIPE_BUF_DEFAULT_SIZE);
    if (!stdinPipe || !stdoutPipe)
    {
        SerialPrintf("TERMINAL: pipe alloc failed\n");
        if (stdinPipe) PipeBufferDestroy(stdinPipe);
        if (stdoutPipe) PipeBufferDestroy(stdoutPipe);
        kfree(t->vfb);
        t->vfb = nullptr;
        return -1;
    }
    stdinPipe->readers = 1;
    stdinPipe->writers = 1;
    stdoutPipe->readers = 1;
    stdoutPipe->writers = 1;

    t->stdinPipe = stdinPipe;
    t->stdoutPipe = stdoutPipe;

    // Set up FdEntry array for bash: fd0=stdin read, fd1=stdout write, fd2=stdout write
    // flags bit 2 (0x04) marks these pipes as the controlling TTY so isatty()
    // returns true for them, even though the fd type is Pipe.
    FdEntry stdFds[3] = {};
    stdFds[0].type = FdType::Pipe;
    stdFds[0].flags = 0x04;    // read end, TTY-pipe
    stdFds[0].handle = stdinPipe;
    stdFds[0].refCount = 1;

    stdFds[1].type = FdType::Pipe;
    stdFds[1].flags = 0x01 | 0x04; // write end, TTY-pipe
    stdFds[1].statusFlags = 1; // O_WRONLY
    stdFds[1].handle = stdoutPipe;
    stdFds[1].refCount = 1;

    stdFds[2].type = FdType::Pipe;
    stdFds[2].flags = 0x01 | 0x04; // write end, TTY-pipe
    stdFds[2].statusFlags = 1; // O_WRONLY
    stdFds[2].handle = stdoutPipe;
    stdFds[2].refCount = 1;

    // Load bash ELF
    VnodeStat st;
    const char* bashPath = "/boot/BIN/BASH";
    if (VfsStatPath(bashPath, &st) != 0)
    {
        SerialPrintf("TERMINAL: bash not found at %s\n", bashPath);
        PipeBufferDestroy(stdinPipe);
        PipeBufferDestroy(stdoutPipe);
        kfree(t->vfb);
        t->vfb = nullptr;
        return -1;
    }

    auto* elfData = static_cast<uint8_t*>(kmalloc(st.size));
    if (!elfData) { SerialPrintf("TERMINAL: elf alloc failed (%lu bytes)\n", st.size); return -1; }

    Vnode* vn = VfsOpen(bashPath, 0);
    if (!vn) { SerialPrintf("TERMINAL: VfsOpen failed for %s\n", bashPath); kfree(elfData); return -1; }

    uint64_t off = 0;
    uint64_t remaining = st.size;
    while (remaining > 0)
    {
        uint64_t chunk = remaining > 65536 ? 65536 : remaining;
        int rd = VfsRead(vn, elfData + off, chunk, &off);
        if (rd <= 0) break;
        remaining -= rd;
    }
    VfsClose(vn);

    if (remaining > 0)
    {
        SerialPrintf("TERMINAL: bash ELF read incomplete (%lu remaining)\n", remaining);
        kfree(elfData);
        return -1;
    }

    // Spawn bash with piped FDs
    const char* bashArgv[] = { "bash", nullptr };

    // Build COLUMNS/LINES strings dynamically from actual terminal size
    char colsBuf[20], linesBuf[20];
    {
        // Simple itoa for COLUMNS=N and LINES=N
        auto itoa = [](char* buf, const char* prefix, uint32_t val) {
            int plen = 0;
            while (prefix[plen]) { buf[plen] = prefix[plen]; plen++; }
            if (val == 0) { buf[plen++] = '0'; buf[plen] = 0; return; }
            char tmp[10]; int tlen = 0;
            while (val > 0) { tmp[tlen++] = '0' + (val % 10); val /= 10; }
            for (int i = tlen - 1; i >= 0; i--) buf[plen++] = tmp[i];
            buf[plen] = 0;
        };
        itoa(colsBuf,  "COLUMNS=", t->cols);
        itoa(linesBuf, "LINES=",   t->rows);
    }

    // Build bash's environment by extending the kernel-shell defaults
    // (SSL_CERT_FILE, WAYLAND_DISPLAY, GTK_*, FONTCONFIG_*, XDG_*, ...).
    // Without this, bash and every program it spawns inherits a stripped
    // env and TLS / fontconfig / wayland / GTK silently break.
    //
    // Per-terminal additions: COLUMNS, LINES, PS1, TERMINFO_DIRS, override
    // PATH so /boot/BIN is searched.
    const char* const* baseEnv  = ShellGetDefaultEnvp();
    uint32_t            baseN   = ShellGetDefaultEnvpCount();

    static constexpr uint32_t kExtra = 6;  // PS1, COLUMNS, LINES, TERMINFO_DIRS, TERMINFO override, PATH override
    const char** bashEnvp = static_cast<const char**>(
        kmalloc((baseN + kExtra + 1) * sizeof(const char*)));
    if (!bashEnvp) {
        kfree(elfData);
        PipeBufferDestroy(stdinPipe);
        PipeBufferDestroy(stdoutPipe);
        kfree(t->vfb);
        t->vfb = nullptr;
        return -1;
    }

    uint32_t envIdx = 0;
    // Copy base env, but skip entries we override (PATH, TERMINFO).
    for (uint32_t i = 0; i < baseN; ++i) {
        const char* e = baseEnv[i];
        // Skip PATH= so we can put /boot/BIN first.
        if (e[0]=='P'&&e[1]=='A'&&e[2]=='T'&&e[3]=='H'&&e[4]=='=') continue;
        if (e[0]=='T'&&e[1]=='E'&&e[2]=='R'&&e[3]=='M'&&e[4]=='I'&&e[5]=='N'&&e[6]=='F'&&e[7]=='O'&&(e[8]=='='||e[8]=='_')) continue;
        bashEnvp[envIdx++] = e;
    }
    // Terminal-specific additions
    bashEnvp[envIdx++] = "PATH=/nix/profile/bin:"
                      "/nix/store/xkqd49dmldkqn4xk6dlm640f5blbv6hp-curl-8.18.0-bin/bin:"
                      "/nix/store/g6mlwdvpg92rchq352ll7jbi0pz7h43r-xz-5.8.2-bin/bin:"
                      "/nix/store/v8sa6r6q037ihghxfbwzjj4p59v2x0pv-bash-5.3p9/bin:"
                      "/boot/BIN:/boot/bin:/usr/bin:/bin";
    bashEnvp[envIdx++] = "TERMINFO=/boot/TERMINFO";
    bashEnvp[envIdx++] = "TERMINFO_DIRS=/boot/TERMINFO";
    bashEnvp[envIdx++] = "PS1=$ ";
    bashEnvp[envIdx++] = colsBuf;
    bashEnvp[envIdx++] = linesBuf;
    bashEnvp[envIdx]   = nullptr;

    Process* child = ProcessCreate(elfData, st.size, 1, bashArgv,
                                   static_cast<int>(envIdx), bashEnvp, stdFds);
    kfree(elfData);
    kfree(bashEnvp);

    if (!child)
    {
        SerialPrintf("TERMINAL: failed to spawn bash\n");
        PipeBufferDestroy(stdinPipe);
        PipeBufferDestroy(stdoutPipe);
        kfree(t->vfb);
        t->vfb = nullptr;
        return -1;
    }

    t->child = child;
    t->foregroundPgid = child->pgid; // default: bash's own group

    // Create the terminal's kernel thread
    Process* thread = KernelThreadCreate("terminal", TerminalThreadFn, t);
    if (!thread)
    {
        SerialPrintf("TERMINAL: failed to create thread\n");
        // child is already created, we'd need to clean up...
        return -1;
    }

    t->thread = thread;

    // Important: the child process needs a VFB for the compositor
    // but in WM mode we set it up differently — the terminal VFB
    // is managed by the terminal thread, not the child process.
    // We point the child's fbPixels at the terminal's VFB so the
    // compositor can blit it.
    child->fbVirtual = t->vfb;
    child->fbVfbWidth = clientW;
    child->fbVfbHeight = clientH;
    child->fbVfbStride = clientW;
    child->fbDirty = 1;
    // Mark as compositor-registered so pages aren't freed while blitting
    __atomic_store_n(&child->compositorRegistered, true, __ATOMIC_RELEASE);

    // Add both to scheduler
    SchedulerAddProcess(child);
    SchedulerAddProcess(thread);

    if (static_cast<uint32_t>(idx) >= g_terminalCount)
        g_terminalCount = static_cast<uint32_t>(idx) + 1;
    SerialPrintf("TERMINAL: created terminal %d, bash pid=%u, thread pid=%u\n",
              idx, child->pid, thread->pid);

    return idx;
}

void TerminalWriteInput(int termIdx, const char* data, uint32_t len)
{
    if (termIdx < 0 || termIdx >= static_cast<int>(g_terminalCount)) return;
    Terminal* t = &g_terminals[termIdx];
    if (!t->active) return;

    // User typing snaps the view back to live, matching the behaviour of
    // xterm / gnome-terminal / putty.
    if (t->viewOffset != 0 && len > 0)
    {
        t->viewOffset = 0;
        CellRedrawAll(t);
        if (t->child) t->child->fbDirty = 1;
        CompositorWake();
    }

    auto* pipe = static_cast<PipeBuffer*>(t->stdinPipe);

    for (uint32_t i = 0; i < len; i++)
    {
        char ch = data[i];

        // Ctrl+C (ETX, 0x03) → echo ^C and newline, don't pass to pipe
        // (signal delivery is handled by the compositor)
        if (ch == 0x03)
        {
            TermRenderGlyph(t, '^');
            TermRenderGlyph(t, 'C');
            TermRenderGlyph(t, '\n');
            t->dirty = true;
            if (t->child) t->child->fbDirty = 1;
            CompositorWake();
            continue;
        }

        // Ctrl+Z (SUB, 0x1A) → echo ^Z and newline
        if (ch == 0x1A)
        {
            TermRenderGlyph(t, '^');
            TermRenderGlyph(t, 'Z');
            TermRenderGlyph(t, '\n');
            t->dirty = true;
            if (t->child) t->child->fbDirty = 1;
            CompositorWake();
            continue;
        }

        // No local echo in TerminalWriteInput. All display is handled by
        // the terminal thread reading from bash's stdout pipe. Bash/readline
        // echoes characters itself via stdout writes.
    }
    t->dirty = true;
    if (t->child) t->child->fbDirty = 1;
    CompositorWake();

    // Write non-signal bytes to pipe
    for (uint32_t i = 0; i < len; i++)
    {
        if (data[i] == 0x03) continue;
        pipe->write(&data[i], 1);
    }

    // Wake whoever is blocked reading the stdin pipe (may be bash, vi, etc.)
    Process* reader = pipe->readerWaiter;
    if (reader && reader->state == ProcessState::Blocked)
    {
        pipe->readerWaiter = nullptr;
        reader->pendingWakeup = 1;
        SchedulerUnblock(reader);
    }
}

Terminal* TerminalGet(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(g_terminalCount)) return nullptr;
    return &g_terminals[idx];
}

Terminal* TerminalGetByThread(Process* proc)
{
    for (uint32_t i = 0; i < g_terminalCount; i++)
    {
        if (g_terminals[i].thread == proc) return &g_terminals[i];
    }
    return nullptr;
}

Terminal* TerminalFindByProcess(Process* proc)
{
    if (!proc) return nullptr;
    for (uint32_t i = 0; i < g_terminalCount; i++)
    {
        if (g_terminals[i].child == proc) return &g_terminals[i];
    }
    // Check if proc is a descendant of any terminal's child
    for (uint32_t i = 0; i < g_terminalCount; i++)
    {
        if (!g_terminals[i].child) continue;
        // Walk up parentPid chain
        Process* p = proc;
        for (int depth = 0; depth < 16 && p; depth++)
        {
            if (p == g_terminals[i].child) return &g_terminals[i];
            if (p->parentPid == 0) break;
            p = ProcessFindByPid(p->parentPid);
        }
    }
    return nullptr;
}

void TerminalClose(Terminal* t)
{
    if (!t || !t->active) return;

    SerialPrintf("TERMINAL: closing terminal (bash pid=%u)\n",
            t->child ? t->child->pid : 0);

    // Signal the terminal thread to stop
    t->active = false;

    // Kill the child process (bash) if still alive
    if (t->child && t->child->state != ProcessState::Terminated)
    {
        ProcessSendSignal(t->child, 9); // SIGKILL
    }

    // Clear child's VFB pointer so compositor won't blit stale data
    if (t->child)
    {
        __atomic_store_n(&t->child->fbVirtual, static_cast<uint32_t*>(nullptr), __ATOMIC_RELEASE);
        t->child->fbVfbWidth = 0;
    }

    // Free VFB memory to mark slot as reusable
    if (t->vfb)
    {
        kfree(t->vfb);
        t->vfb = nullptr;
    }
    if (t->altVfb)
    {
        kfree(t->altVfb);
        t->altVfb = nullptr;
    }
    if (t->cells)
    {
        kfree(t->cells);
        t->cells = nullptr;
    }
    if (t->scrollback)
    {
        kfree(t->scrollback);
        t->scrollback = nullptr;
    }
    t->scrollbackRows = 0;
    t->scrollbackHead = 0;
    t->scrollbackUsed = 0;
    t->viewOffset = 0;

    // Wake the terminal thread so it can exit its loop
    if (t->thread)
        SchedulerUnblock(t->thread);
}

void TerminalResize(Terminal* t, uint32_t newW, uint32_t newH)
{
    if (!t || !t->active || newW == 0 || newH == 0) return;
    if (newW == t->vfbW && newH == t->vfbH) return; // no change

    // Allocate new VFB
    uint32_t newSize = newW * newH * 4;
    auto* newVfb = static_cast<uint32_t*>(kmalloc(newSize));
    if (!newVfb) return;

    // Fill with background colour
    for (uint32_t i = 0; i < newW * newH; i++)
        newVfb[i] = t->bgColor;

    // Copy visible content from old VFB (row by row, clamped to min dimensions)
    uint32_t copyW = (newW < t->vfbW) ? newW : t->vfbW;
    uint32_t copyH = (newH < t->vfbH) ? newH : t->vfbH;
    for (uint32_t y = 0; y < copyH; y++)
    {
        auto* src = t->vfb + y * t->vfbW;
        auto* dst = newVfb + y * newW;
        for (uint32_t x = 0; x < copyW; x++)
            dst[x] = src[x];
    }

    // Free old VFBs (including alt screen if active)
    if (t->inAltScreen && t->altVfb)
    {
        kfree(t->altVfb);
        t->altVfb = nullptr;
        t->inAltScreen = false;
    }

    // If the child has user-space mappings of the OLD vfb pages, those
    // PTEs still point at the kernel pages we're about to free.  After
    // kfree(t->vfb) those pages may be reused for unrelated allocations,
    // and the next user write through the stale mapping silently
    // corrupts kernel state — Ian observed btop crashing the WM on
    // resize because of this.  Re-point each user PTE at the matching
    // page of newVfb (up to min(oldPages, newPages, mapPages)) before
    // freeing.  Pages beyond the new vfb are unmapped so a stray write
    // takes a clean #PF instead of a UAF.
    if (t->child)
    {
        uint64_t newKernVa = reinterpret_cast<uint64_t>(newVfb);
        uint64_t newPages = (newSize + 4095) / 4096;
        for (uint32_t mi = 0; mi < Process::MAX_FB_MAPS; ++mi)
        {
            auto& m = t->child->fbMaps[mi];
            if (m.length == 0) continue;
            uint64_t mapPages = m.length / 4096;
            for (uint64_t i = 0; i < mapPages; ++i)
            {
                VirtualAddress uva(m.vaddr + i * 4096);
                if (i < newPages)
                {
                    VirtualAddress newKva(newKernVa + i * 4096);
                    PhysicalAddress np = VmmVirtToPhys(KernelPageTable, newKva);
                    if (!np) continue;
                    VmmUnmapPage(t->child->pageTable, uva);
                    VmmMapPage(t->child->pageTable, uva, np,
                               VMM_WRITABLE | VMM_USER | VMM_NO_EXEC,
                               MemTag::Device, t->child->pid);
                }
                else
                {
                    VmmUnmapPage(t->child->pageTable, uva);
                }
            }
            // Truncate the recorded mapping so munmap doesn't try to
            // unmap pages beyond the new VFB.
            if (mapPages > newPages) m.length = newPages * 4096;
        }
    }

    kfree(t->vfb);

    // Install new VFB
    t->vfb  = newVfb;
    t->vfbW = newW;
    t->vfbH = newH;

    // Recalculate character grid
    uint32_t oldCols = t->cols;
    uint32_t oldRows = t->rows;
    t->cols = newW / static_cast<uint32_t>(g_fontAtlas.glyphs[0].advance);
    t->rows = newH / static_cast<uint32_t>(g_fontAtlas.lineHeight);
    if (t->cols == 0) t->cols = 1;
    if (t->rows == 0) t->rows = 1;

    // Clamp cursor to new dimensions
    if (t->curX >= t->cols) t->curX = t->cols - 1;
    if (t->curY >= t->rows) t->curY = t->rows - 1;

    // Reallocate the cell grid for the new dimensions.  Content is
    // copied anchored to the bottom-left so the visible output stays
    // where the user expects when shrinking.
    if (t->cells)
    {
        TermCell* newCells = static_cast<TermCell*>(
            kmalloc(t->cols * t->rows * sizeof(TermCell)));
        if (newCells)
        {
            CellFillBlank(t, newCells, t->cols * t->rows);
            uint32_t copyCols = (t->cols < oldCols) ? t->cols : oldCols;
            uint32_t copyRows = (t->rows < oldRows) ? t->rows : oldRows;
            uint32_t srcRowStart = oldRows - copyRows;
            uint32_t dstRowStart = t->rows - copyRows;
            for (uint32_t y = 0; y < copyRows; y++)
            {
                TermCell* src = t->cells + (srcRowStart + y) * oldCols;
                TermCell* dst = newCells + (dstRowStart + y) * t->cols;
                for (uint32_t x = 0; x < copyCols; x++) dst[x] = src[x];
            }
            kfree(t->cells);
            t->cells = newCells;
        }
    }

    // Reflow the scrollback ring if cols changed: copy each row clipped
    // or zero-padded to the new column count instead of dropping history.
    // Without this, wheel-up after a horizontal resize would appear to
    // do nothing.
    if (t->cols != oldCols && t->scrollback && t->scrollbackRows > 0)
    {
        TermCell* oldRing = t->scrollback;
        uint32_t  oldHead = t->scrollbackHead;
        uint32_t  oldUsed = t->scrollbackUsed;

        TermCell* newRing = static_cast<TermCell*>(
            kmalloc(t->scrollbackRows * t->cols * sizeof(TermCell)));
        if (newRing)
        {
            CellFillBlank(t, newRing, t->scrollbackRows * t->cols);
            uint32_t copyC = (t->cols < oldCols) ? t->cols : oldCols;
            for (uint32_t i = 0; i < oldUsed; i++)
            {
                uint32_t srcIdx = (oldHead + t->scrollbackRows - oldUsed + i)
                                  % t->scrollbackRows;
                TermCell* src = oldRing + srcIdx * oldCols;
                TermCell* dst = newRing + i * t->cols;
                for (uint32_t x = 0; x < copyC; x++) dst[x] = src[x];
            }
            kfree(oldRing);
            t->scrollback = newRing;
            t->scrollbackUsed = oldUsed;
            t->scrollbackHead = oldUsed % t->scrollbackRows;
        }
        else
        {
            t->scrollback = nullptr;
            t->scrollbackHead = 0;
            t->scrollbackUsed = 0;
        }
    }

    // A resize snaps the viewport back to live — the old offset no
    // longer makes sense against the new geometry.
    t->viewOffset = 0;

    // Update child process VFB pointer
    if (t->child)
    {
        t->child->fbVirtual    = t->vfb;
        t->child->fbVirtualSize = newSize;
        t->child->fbVfbWidth   = newW;
        t->child->fbVfbHeight  = newH;
        t->child->fbVfbStride  = newW;
        t->child->fbDirty      = 1;
    }

    t->dirty = true;

    SerialPrintf("TERMINAL: resized %ux%u -> %ux%u (%ux%u chars -> %ux%u chars)\n",
            oldCols * static_cast<uint32_t>(g_fontAtlas.glyphs[0].advance),
            oldRows * static_cast<uint32_t>(g_fontAtlas.lineHeight),
            newW, newH, oldCols, oldRows, t->cols, t->rows);

    // Send SIGWINCH to child process group
    if (t->child && t->child->state != ProcessState::Terminated)
    {
        ProcessSendSignalToGroup(t->child->pgid, 28); // SIGWINCH
    }
}

void TerminalScroll(Terminal* t, int32_t dy)
{
    if (!t || !t->active || dy == 0) return;
    if (!t->cells) return;

    // Positive dy = scroll UP (show older content, increase offset).
    // Negative dy = scroll DOWN (newer content, decrease offset).
    int64_t newOff = static_cast<int64_t>(t->viewOffset) + dy;
    if (newOff < 0) newOff = 0;
    if (newOff > static_cast<int64_t>(t->scrollbackUsed))
        newOff = static_cast<int64_t>(t->scrollbackUsed);
    uint32_t newOffU = static_cast<uint32_t>(newOff);
    if (newOffU == t->viewOffset) return;

    t->viewOffset = newOffU;
    CellRedrawAll(t);
    if (t->child) t->child->fbDirty = 1;
    CompositorWake();
}

} // namespace brook
