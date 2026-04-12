// clock_overlay.cpp — Kernel thread that renders uptime in the top-right
// corner of the physical framebuffer. Serves as a proof-of-concept for
// kernel threads and verifies they schedule, sleep, and wake correctly.

#include "clock_overlay.h"
#include "process.h"
#include "scheduler.h"
#include "compositor.h"
#include "font_atlas.h"
#include "serial.h"

// LAPIC tick counter (1 tick = 1ms).
namespace brook { extern volatile uint64_t g_lapicTickCount; }

namespace brook {

// Render a single glyph directly onto the physical framebuffer.
static uint32_t DrawGlyph(volatile uint32_t* fb, uint32_t fbStride,
                           uint32_t fbW, uint32_t fbH,
                           char c, uint32_t px, uint32_t py, uint32_t colour)
{
    const FontAtlas& fa = g_fontAtlas;
    int code = static_cast<int>(static_cast<uint8_t>(c));
    if (code < static_cast<int>(fa.firstChar) ||
        code >= static_cast<int>(fa.firstChar + fa.glyphCount))
        return 0;

    const GlyphInfo& gi = fa.glyphs[code - static_cast<int>(fa.firstChar)];
    int gw = gi.atlasX1 - gi.atlasX0;
    int gh = gi.atlasY1 - gi.atlasY0;

    int drawX = static_cast<int>(px) + gi.bearingX;
    int drawY = static_cast<int>(py) + fa.ascent - gi.bearingY;

    for (int row = 0; row < gh; ++row)
    {
        int absY = drawY + row;
        if (absY < 0 || static_cast<uint32_t>(absY) >= fbH) continue;
        for (int col = 0; col < gw; ++col)
        {
            int absX = drawX + col;
            if (absX < 0 || static_cast<uint32_t>(absX) >= fbW) continue;

            uint8_t cov = fa.pixels[(gi.atlasY0 + row) *
                static_cast<int>(fa.atlasWidth) + (gi.atlasX0 + col)];
            if (cov > 0)
            {
                uint32_t r = ((colour >> 16) & 0xFF) * cov / 255;
                uint32_t g = ((colour >> 8) & 0xFF) * cov / 255;
                uint32_t b = (colour & 0xFF) * cov / 255;
                fb[absY * fbStride + absX] = (r << 16) | (g << 8) | b;
            }
        }
    }

    return static_cast<uint32_t>(gi.advance);
}

static void DrawText(volatile uint32_t* fb, uint32_t fbStride,
                      uint32_t fbW, uint32_t fbH,
                      const char* text, uint32_t x, uint32_t y, uint32_t colour)
{
    while (*text)
    {
        x += DrawGlyph(fb, fbStride, fbW, fbH, *text, x, y, colour);
        ++text;
    }
}

static uint32_t MeasureText(const char* text)
{
    const FontAtlas& fa = g_fontAtlas;
    uint32_t w = 0;
    while (*text)
    {
        int code = static_cast<int>(static_cast<uint8_t>(*text));
        if (code >= static_cast<int>(fa.firstChar) &&
            code < static_cast<int>(fa.firstChar + fa.glyphCount))
        {
            w += static_cast<uint32_t>(
                fa.glyphs[code - static_cast<int>(fa.firstChar)].advance);
        }
        ++text;
    }
    return w;
}

// Format seconds as HH:MM:SS into buf. Returns pointer to buf.
static char* FormatUptime(char* buf, uint64_t totalSec)
{
    uint32_t h = static_cast<uint32_t>(totalSec / 3600);
    uint32_t m = static_cast<uint32_t>((totalSec % 3600) / 60);
    uint32_t s = static_cast<uint32_t>(totalSec % 60);

    buf[0] = static_cast<char>('0' + (h / 10) % 10);
    buf[1] = static_cast<char>('0' + h % 10);
    buf[2] = ':';
    buf[3] = static_cast<char>('0' + m / 10);
    buf[4] = static_cast<char>('0' + m % 10);
    buf[5] = ':';
    buf[6] = static_cast<char>('0' + s / 10);
    buf[7] = static_cast<char>('0' + s % 10);
    buf[8] = '\0';
    return buf;
}

// The kernel thread entry point.
static void ClockThreadFn(void* /*arg*/)
{
    // Wait for compositor to be ready.
    uint32_t fbW = 0, fbH = 0, fbStride = 0;
    volatile uint32_t* fb = nullptr;

    while (!fb)
    {
        fb = CompositorGetPhysFb(&fbStride);
        CompositorGetPhysDims(&fbW, &fbH);
        if (!fb)
        {
            // Sleep 100ms and try again.
            Process* self = ProcessCurrent();
            self->wakeupTick = g_lapicTickCount + 100;
            SchedulerBlock(self);
        }
    }

    const uint32_t lineH = static_cast<uint32_t>(g_fontAtlas.lineHeight);
    const uint32_t padding = 4;
    const uint32_t colour = 0x00FF00; // green

    SerialPrintf("CLOCK: overlay thread running, fb=%ux%u\n", fbW, fbH);

    for (;;)
    {
        uint64_t uptimeMs = g_lapicTickCount;
        uint64_t uptimeSec = uptimeMs / 1000;

        char buf[16];
        FormatUptime(buf, uptimeSec);

        uint32_t textW = MeasureText(buf);
        uint32_t x = fbW - textW - padding;
        uint32_t y = padding;

        // Clear background rect
        for (uint32_t row = y; row < y + lineH + 2 && row < fbH; ++row)
            for (uint32_t col = x > 2 ? x - 2 : 0; col < fbW; ++col)
                fb[row * fbStride + col] = 0x000000;

        // Draw the time
        DrawText(fb, fbStride, fbW, fbH, buf, x, y, colour);

        // Sleep 1 second
        Process* self = ProcessCurrent();
        self->wakeupTick = g_lapicTickCount + 1000;
        SchedulerBlock(self);
    }
}

void ClockOverlayStart()
{
    Process* thread = KernelThreadCreate("clock", ClockThreadFn, nullptr);
    if (!thread)
    {
        SerialPuts("CLOCK: failed to create kernel thread\n");
        return;
    }

    SchedulerAddProcess(thread);
    SerialPrintf("CLOCK: overlay thread started (pid %u)\n", thread->pid);
}

} // namespace brook
