// panic_screen.cpp — Full-screen Enkel-style panic display.
//
// Renders directly to the framebuffer with no TTY dependency.
// Uses the font atlas for text rendering.

#include "panic_screen.h"
#include "font_atlas.h"
#include "build_info.h"
#include "ksym_addrs.h"
#include "exception_info.h"
#include "serial.h"
#include "panic_qr.h"

namespace brook {

// Colour palette
static constexpr uint32_t BG_DARK    = 0x001A0000; // dark red
static constexpr uint32_t BG_BANNER  = 0x00CC0000; // bright red banner
static constexpr uint32_t FG_WHITE   = 0x00FFFFFF;
static constexpr uint32_t FG_GREY    = 0x00B0B0B0;
static constexpr uint32_t FG_YELLOW  = 0x00FFD700;
static constexpr uint32_t FG_CYAN    = 0x0080D0FF;

// Fill a rectangle with a solid colour.
static void FillRect(uint32_t* fb, uint32_t stride,
                     uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                     uint32_t color)
{
    for (uint32_t row = 0; row < h; row++)
        for (uint32_t col = 0; col < w; col++)
            fb[(y + row) * stride + (x + col)] = color;
}

// Render a single glyph at (px, py) with a given foreground colour.
// Returns the advance width.
static int DrawGlyph(uint32_t* fb, uint32_t stride,
                     uint32_t fbW, uint32_t fbH,
                     int px, int py, int codepoint, uint32_t fg)
{
    const FontAtlas& fa = g_fontAtlas;
    if (codepoint < static_cast<int>(fa.firstChar) ||
        codepoint >= static_cast<int>(fa.firstChar + fa.glyphCount))
        return (fa.glyphCount > 0) ? fa.glyphs[0].advance : 8;

    const GlyphInfo& gi = fa.glyphs[codepoint - static_cast<int>(fa.firstChar)];
    int gw = gi.atlasX1 - gi.atlasX0;
    int gh = gi.atlasY1 - gi.atlasY0;
    int drawX = px + gi.bearingX;
    int drawY = py + fa.ascent - gi.bearingY;

    uint32_t fgR = (fg >> 16) & 0xFF;
    uint32_t fgG = (fg >> 8) & 0xFF;
    uint32_t fgB = fg & 0xFF;

    for (int row = 0; row < gh; row++)
    {
        int sy = drawY + row;
        if (sy < 0 || static_cast<uint32_t>(sy) >= fbH) continue;
        for (int col = 0; col < gw; col++)
        {
            int sx = drawX + col;
            if (sx < 0 || static_cast<uint32_t>(sx) >= fbW) continue;

            uint8_t cov = fa.pixels[(gi.atlasY0 + row) * static_cast<int>(fa.atlasWidth)
                                     + (gi.atlasX0 + col)];
            if (!cov) continue;

            uint32_t& dst = fb[sy * stride + sx];
            uint32_t dR = (dst >> 16) & 0xFF;
            uint32_t dG = (dst >> 8) & 0xFF;
            uint32_t dB = dst & 0xFF;
            uint32_t a = cov;
            dst = (((fgR * a + dR * (255 - a)) / 255) << 16)
                | (((fgG * a + dG * (255 - a)) / 255) << 8)
                | ((fgB * a + dB * (255 - a)) / 255);
        }
    }
    return gi.advance;
}

// Draw a null-terminated string. Returns the x position after the last char.
static int DrawString(uint32_t* fb, uint32_t stride,
                      uint32_t fbW, uint32_t fbH,
                      int px, int py, const char* str, uint32_t fg)
{
    if (!str) return px;
    for (const char* p = str; *p; ++p)
    {
        if (*p == '\n') { py += g_fontAtlas.lineHeight; px = 20; continue; }
        px += DrawGlyph(fb, stride, fbW, fbH, px, py, *p, fg);
    }
    return px;
}

// Format a uint64 as "0x" + 16 hex digits into buf. Returns length.
static int Hex64(char* buf, uint64_t val)
{
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 15; i >= 0; i--)
    {
        int nibble = (val >> (i * 4)) & 0xF;
        buf[2 + (15 - i)] = (nibble < 10) ? ('0' + nibble) : ('a' + nibble - 10);
    }
    buf[18] = '\0';
    return 18;
}

// Format a uint16 as "0x" + 4 hex digits.
static int Hex16(char* buf, uint16_t val)
{
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 3; i >= 0; i--)
    {
        int nibble = (val >> (i * 4)) & 0xF;
        buf[2 + (3 - i)] = (nibble < 10) ? ('0' + nibble) : ('a' + nibble - 10);
    }
    buf[6] = '\0';
    return 6;
}

// Draw "LABEL  0xVALUE" returning new y.
static int DrawReg64(uint32_t* fb, uint32_t stride, uint32_t fbW, uint32_t fbH,
                     int px, int py, const char* label, uint64_t val, uint32_t fg)
{
    int x = DrawString(fb, stride, fbW, fbH, px, py, label, FG_GREY);
    char hex[20];
    Hex64(hex, val);
    DrawString(fb, stride, fbW, fbH, x, py, hex, fg);
    return py;
}

void PanicScreenRender(uint32_t* fb, uint32_t fbW, uint32_t fbH,
                       uint32_t fbStride, const PanicScreenInfo* info)
{
    if (!fb || fbW == 0 || fbH == 0) return;

    // fbStride comes from TtyGetFramebuffer which returns bytes — convert to pixels
    uint32_t stride = fbStride / sizeof(uint32_t);

    const FontAtlas& fa = g_fontAtlas;
    int lineH = fa.lineHeight;
    int glyphW = (fa.glyphCount > 0) ? fa.glyphs[0].advance : 8;

    // 1. Fill entire screen with dark red background
    FillRect(fb, stride, 0, 0, fbW, fbH, BG_DARK);

    // 2. Draw bright red banner at top
    uint32_t bannerH = static_cast<uint32_t>(lineH) + 16;
    FillRect(fb, stride, 0, 0, fbW, bannerH, BG_BANNER);
    DrawString(fb, stride, fbW, fbH, 20, 8, "KERNEL PANIC", FG_WHITE);

    // Build info on banner right side
    {
        int bx = static_cast<int>(fbW) - 400;
        if (bx < 200) bx = 200;
        bx = DrawString(fb, stride, fbW, fbH, bx, 8, "Brook OS ", FG_GREY);
        bx = DrawString(fb, stride, fbW, fbH, bx, 8, BuildDate(), FG_GREY);
        bx = DrawString(fb, stride, fbW, fbH, bx, 8, " (", FG_GREY);
        bx = DrawString(fb, stride, fbW, fbH, bx, 8, BuildGitHash(), FG_GREY);
        DrawString(fb, stride, fbW, fbH, bx, 8, ")", FG_GREY);
    }

    int y = static_cast<int>(bannerH) + 8;
    int leftMargin = 20;

    // 3. Error description
    if (info->message)
    {
        y += 4;
        DrawString(fb, stride, fbW, fbH, leftMargin, y, info->message, FG_YELLOW);
        y += lineH;
    }

    // Exception description if vector is set
    if (info->vector > 0 && info->regs)
    {
        bool fromUser = (info->regs->cs & 3) != 0;
        const char* desc = ExceptionDescribe(
            static_cast<uint32_t>(info->vector),
            static_cast<uint32_t>(info->errorCode),
            info->regs->cr2, info->regs->rip, fromUser);
        DrawString(fb, stride, fbW, fbH, leftMargin, y, desc, FG_GREY);
        y += lineH;
    }

    y += 8; // spacing

    // 4. Register dump (left column, two registers per line)
    if (info->regs)
    {
        const PanicCPURegs& r = *info->regs;
        DrawString(fb, stride, fbW, fbH, leftMargin, y, "Registers:", FG_CYAN);
        y += lineH;

        // RIP with symbol
        {
            int x = DrawString(fb, stride, fbW, fbH, leftMargin, y, "  RIP ", FG_GREY);
            char hex[20]; Hex64(hex, r.rip);
            x = DrawString(fb, stride, fbW, fbH, x, y, hex, FG_WHITE);
            const char* symName = nullptr; uint64_t symOff = 0;
            if (KsymFindByAddr(r.rip, &symName, &symOff))
            {
                x = DrawString(fb, stride, fbW, fbH, x, y, "  ", FG_GREY);
                x = DrawString(fb, stride, fbW, fbH, x, y, symName, FG_YELLOW);
            }
            y += lineH;
        }

        // Two columns of GPRs
        struct RegPair { const char* name; uint64_t val; };
        RegPair gprs[] = {
            {"  RAX ", r.rax}, {"  RBX ", r.rbx},
            {"  RCX ", r.rcx}, {"  RDX ", r.rdx},
            {"  RSI ", r.rsi}, {"  RDI ", r.rdi},
            {"  RSP ", r.rsp}, {"  RBP ", r.rbp},
            {"  R8  ", r.r8},  {"  R9  ", r.r9},
            {"  R10 ", r.r10}, {"  R11 ", r.r11},
            {"  R12 ", r.r12}, {"  R13 ", r.r13},
            {"  R14 ", r.r14}, {"  R15 ", r.r15},
        };
        int col2X = leftMargin + 30 * glyphW; // second column
        for (int i = 0; i < 16; i += 2)
        {
            DrawReg64(fb, stride, fbW, fbH, leftMargin, y,
                      gprs[i].name, gprs[i].val, FG_WHITE);
            if (i + 1 < 16)
                DrawReg64(fb, stride, fbW, fbH, col2X, y,
                          gprs[i+1].name, gprs[i+1].val, FG_WHITE);
            y += lineH;
        }

        // RFLAGS
        DrawReg64(fb, stride, fbW, fbH, leftMargin, y,
                  "  RFLAGS ", r.rflags, FG_WHITE);
        y += lineH;

        // Control registers
        DrawReg64(fb, stride, fbW, fbH, leftMargin, y,
                  "  CR0 ", r.cr0, FG_WHITE);
        DrawReg64(fb, stride, fbW, fbH, col2X, y,
                  "  CR2 ", r.cr2, FG_WHITE);
        y += lineH;
        DrawReg64(fb, stride, fbW, fbH, leftMargin, y,
                  "  CR3 ", r.cr3, FG_WHITE);
        DrawReg64(fb, stride, fbW, fbH, col2X, y,
                  "  CR4 ", r.cr4, FG_WHITE);
        y += lineH;

        // Segment registers
        {
            char seg[8];
            int x = DrawString(fb, stride, fbW, fbH, leftMargin, y, "  CS ", FG_GREY);
            Hex16(seg, r.cs);
            x = DrawString(fb, stride, fbW, fbH, x, y, seg, FG_WHITE);
            x = DrawString(fb, stride, fbW, fbH, x, y, "  DS ", FG_GREY);
            Hex16(seg, r.ds);
            x = DrawString(fb, stride, fbW, fbH, x, y, seg, FG_WHITE);
            x = DrawString(fb, stride, fbW, fbH, x, y, "  SS ", FG_GREY);
            Hex16(seg, r.ss);
            DrawString(fb, stride, fbW, fbH, x, y, seg, FG_WHITE);
            y += lineH;
        }
    }

    y += 8;

    // 5. Stack trace with symbols
    if (info->trace && info->trace->depth > 0)
    {
        DrawString(fb, stride, fbW, fbH, leftMargin, y, "Stack Trace:", FG_CYAN);
        y += lineH;

        for (uint8_t i = 0; i < info->trace->depth && y + lineH < static_cast<int>(fbH) - 20; i++)
        {
            char prefix[8];
            prefix[0] = ' '; prefix[1] = ' '; prefix[2] = '#';
            prefix[3] = '0' + (i / 10);
            prefix[4] = '0' + (i % 10);
            prefix[5] = ' '; prefix[6] = ' '; prefix[7] = '\0';

            int x = DrawString(fb, stride, fbW, fbH, leftMargin, y, prefix, FG_GREY);
            char hex[20]; Hex64(hex, info->trace->rip[i]);
            x = DrawString(fb, stride, fbW, fbH, x, y, hex, FG_WHITE);

            const char* symName = nullptr; uint64_t symOff = 0;
            if (KsymFindByAddr(info->trace->rip[i], &symName, &symOff))
            {
                x = DrawString(fb, stride, fbW, fbH, x, y, "  ", FG_GREY);
                DrawString(fb, stride, fbW, fbH, x, y, symName, FG_YELLOW);
            }
            y += lineH;
        }
    }

    // 6. Render QR code in the right column
    PanicRenderQR(fb, fbW, fbH, fbStride, info->regs, info->trace);

    // 7. "System halted" at bottom
    DrawString(fb, stride, fbW, fbH, leftMargin,
               static_cast<int>(fbH) - lineH - 8,
               "System halted. Scan QR code to decode crash data.",
               FG_GREY);
}

} // namespace brook
