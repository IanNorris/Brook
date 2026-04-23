// debug_overlay.cpp — Kernel console: ring buffer + WM window + TCP debug server.
//
// A kernel thread owns a VFB and renders the last N lines of kernel log
// output into it. The compositor treats it as a regular window.
// A second kernel thread streams the ring buffer over TCP on port 1234.

#include "debug_overlay.h"
#include "font_atlas.h"
#include "spinlock.h"
#include "process.h"
#include "scheduler.h"
#include "compositor.h"
#include "window.h"
#include "serial.h"
#include "net.h"
#include "kprintf.h"
#include "string.h"

namespace brook {

// --- Ring buffer -----------------------------------------------------------

static constexpr uint32_t RING_LINES  = 512;   // total lines kept
static constexpr uint32_t RING_COLS   = 160;   // max chars per line

static char     g_ring[RING_LINES][RING_COLS + 1];
static uint32_t g_writeHead  = 0;   // next line index to write
static uint32_t g_totalLines = 0;   // total lines ever written
static uint32_t g_readHead   = 0;   // consumer read cursor (line index)
static uint32_t g_curCol     = 0;   // current column in partial line
static SpinLock g_ringLock;

void DebugOverlayInit()
{
    for (uint32_t i = 0; i < RING_LINES; i++)
        g_ring[i][0] = '\0';
    g_writeHead = 0;
    g_totalLines = 0;
    g_readHead = 0;
    g_curCol = 0;
}

// Finish the current partial line and advance the write head.
static void FinishLine()
{
    g_ring[g_writeHead][g_curCol] = '\0';
    g_writeHead = (g_writeHead + 1) % RING_LINES;
    g_totalLines++;
    g_curCol = 0;
    // Pre-clear next line
    g_ring[g_writeHead][0] = '\0';
}

void DebugOverlayPuts(const char* text)
{
    if (!text) return;

    uint64_t flags = SpinLockAcquire(&g_ringLock);

    for (const char* p = text; *p; ++p)
    {
        if (*p == '\n')
        {
            FinishLine();
            continue;
        }
        if (g_curCol < RING_COLS)
            g_ring[g_writeHead][g_curCol++] = *p;
    }

    SpinLockRelease(&g_ringLock, flags);
}

uint32_t DebugOverlayRead(char* out, uint32_t maxLines, uint32_t lineLen)
{
    uint64_t flags = SpinLockAcquire(&g_ringLock);

    // How many completed lines are available since last read?
    uint32_t available = g_totalLines - g_readHead;
    if (available > RING_LINES) available = RING_LINES; // wrapped

    uint32_t count = (available < maxLines) ? available : maxLines;

    // Start reading from the oldest unread line
    uint32_t startIdx;
    if (g_totalLines <= RING_LINES)
        startIdx = g_readHead;
    else
    {
        // Ring has wrapped — oldest available is at g_writeHead
        uint32_t oldest = g_totalLines - RING_LINES;
        if (g_readHead < oldest) g_readHead = oldest;
        startIdx = g_readHead;
    }

    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t ringIdx = (startIdx + i) % RING_LINES;
        char* dst = out + i * lineLen;
        const char* src = g_ring[ringIdx];
        uint32_t j = 0;
        while (j < lineLen - 1 && src[j]) { dst[j] = src[j]; j++; }
        dst[j] = '\0';
    }

    g_readHead = startIdx + count;

    SpinLockRelease(&g_ringLock, flags);
    return count;
}

uint32_t DebugOverlayTotalLines()
{
    return g_totalLines;
}

uint32_t DebugOverlayReadFrom(uint32_t* cursor, char* out, uint32_t maxLines, uint32_t lineLen)
{
    uint64_t flags = SpinLockAcquire(&g_ringLock);

    uint32_t available = g_totalLines - *cursor;
    if (available > RING_LINES) available = RING_LINES;

    uint32_t count = (available < maxLines) ? available : maxLines;

    uint32_t startIdx;
    if (g_totalLines <= RING_LINES)
        startIdx = *cursor;
    else
    {
        uint32_t oldest = g_totalLines - RING_LINES;
        if (*cursor < oldest) *cursor = oldest;
        startIdx = *cursor;
    }

    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t ringIdx = (startIdx + i) % RING_LINES;
        char* dst = out + i * lineLen;
        const char* src = g_ring[ringIdx];
        uint32_t j = 0;
        while (j < lineLen - 1 && src[j]) { dst[j] = src[j]; j++; }
        dst[j] = '\0';
    }

    *cursor = startIdx + count;

    SpinLockRelease(&g_ringLock, flags);
    return count;
}

// --- Kernel console window thread ------------------------------------------

static constexpr uint32_t CONSOLE_W      = 800;
static constexpr uint32_t CONSOLE_H      = 500;
static constexpr uint32_t CONSOLE_BG     = 0x001A1A2E;  // dark blue-grey
static constexpr uint32_t CONSOLE_FG_R   = 0xC0;
static constexpr uint32_t CONSOLE_FG_G   = 0xE0;
static constexpr uint32_t CONSOLE_FG_B   = 0xC0;

// Render a single glyph into the VFB at pixel position (px, py).
static void RenderGlyph(uint32_t* vfb, uint32_t stride, uint32_t w, uint32_t h,
                         int px, int py, int codepoint)
{
    const FontAtlas& fa = g_fontAtlas;
    if (codepoint < static_cast<int>(fa.firstChar) ||
        codepoint >= static_cast<int>(fa.firstChar + fa.glyphCount))
        return;

    const GlyphInfo& gi = fa.glyphs[codepoint - static_cast<int>(fa.firstChar)];
    int gw = gi.atlasX1 - gi.atlasX0;
    int gh = gi.atlasY1 - gi.atlasY0;
    int drawX = px + gi.bearingX;
    int drawY = py + fa.ascent - gi.bearingY;

    for (int row = 0; row < gh; row++)
    {
        int sy = drawY + row;
        if (sy < 0 || static_cast<uint32_t>(sy) >= h) continue;
        for (int col = 0; col < gw; col++)
        {
            int sx = drawX + col;
            if (sx < 0 || static_cast<uint32_t>(sx) >= w) continue;

            uint8_t cov = fa.pixels[(gi.atlasY0 + row) * static_cast<int>(fa.atlasWidth)
                                     + (gi.atlasX0 + col)];
            if (!cov) continue;

            uint32_t& dst = vfb[sy * stride + sx];
            uint32_t dR = (dst >> 16) & 0xFF;
            uint32_t dG = (dst >> 8) & 0xFF;
            uint32_t dB = dst & 0xFF;
            uint32_t a = cov;
            uint32_t oR = (CONSOLE_FG_R * a + dR * (255 - a)) / 255;
            uint32_t oG = (CONSOLE_FG_G * a + dG * (255 - a)) / 255;
            uint32_t oB = (CONSOLE_FG_B * a + dB * (255 - a)) / 255;
            dst = (oR << 16) | (oG << 8) | oB;
        }
    }
}

extern volatile uint64_t g_lapicTickCount;

static void KernelConsoleThread(void* /*arg*/)
{
    Process* self = ProcessCurrent();

    // Set up VFB via compositor
    if (!CompositorSetupProcess(self, 20, 60, CONSOLE_W, CONSOLE_H, 1))
    {
        SerialPuts("KCONSOLE: failed to set up VFB\n");
        return;
    }

    // Create WM window
    WmCreateWindow(self, 20, 60,
                   static_cast<uint16_t>(CONSOLE_W),
                   static_cast<uint16_t>(CONSOLE_H), "Kernel Console");

    const FontAtlas& fa = g_fontAtlas;
    uint32_t lineH = static_cast<uint32_t>(fa.lineHeight);
    uint32_t glyphW = (fa.glyphCount > 0) ? static_cast<uint32_t>(fa.glyphs[0].advance) : 8;
    uint32_t visibleLines = (CONSOLE_H - 4) / lineH; // leave small margin
    uint32_t maxCols = (CONSOLE_W - 4) / glyphW;

    // Local display buffer — stores what's currently rendered
    // We keep a full screen of lines and render on change.
    static char dispBuf[64][RING_COLS + 1];
    uint32_t dispCount = 0;
    uint32_t lastTotal = 0;

    for (;;)
    {
        uint32_t nowTotal = DebugOverlayTotalLines();
        if (nowTotal != lastTotal)
        {
            // New content available — re-render the VFB.
            // Read all available lines from ring (up to visible count).
            // We want the LATEST visibleLines lines, not just new ones.
            uint64_t flags = SpinLockAcquire(&g_ringLock);

            uint32_t totalAvail = g_totalLines;
            uint32_t count = (totalAvail < visibleLines) ? totalAvail : visibleLines;

            // Calculate start position: we want the last `count` lines
            uint32_t startLine;
            if (totalAvail <= RING_LINES)
                startLine = (totalAvail > count) ? totalAvail - count : 0;
            else
                startLine = totalAvail - count;

            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t ringIdx = (startLine + i) % RING_LINES;
                const char* src = g_ring[ringIdx];
                uint32_t j = 0;
                while (j < RING_COLS && src[j]) { dispBuf[i][j] = src[j]; j++; }
                dispBuf[i][j] = '\0';
            }
            dispCount = count;
            // Update read cursor so we don't re-read
            g_readHead = totalAvail;

            SpinLockRelease(&g_ringLock, flags);
            lastTotal = nowTotal;

            // Clear VFB to background
            uint32_t* vfb = self->fbVirtual;
            uint32_t totalPixels = CONSOLE_W * CONSOLE_H;
            for (uint32_t i = 0; i < totalPixels; i++)
                vfb[i] = CONSOLE_BG;

            // Render text lines
            for (uint32_t i = 0; i < dispCount; i++)
            {
                int px = 4; // left margin
                int py = 2 + static_cast<int>(i * lineH);
                const char* line = dispBuf[i];
                uint32_t col = 0;
                for (const char* cp = line; *cp && col < maxCols; ++cp, ++col)
                {
                    RenderGlyph(vfb, CONSOLE_W, CONSOLE_W, CONSOLE_H,
                                px, py, static_cast<int>(*cp));
                    px += static_cast<int>(glyphW);
                }
            }

            __atomic_store_n(&self->fbDirty, 1u, __ATOMIC_RELEASE);
            CompositorWake();
        }

        // Sleep briefly, then check for new content
        self->wakeupTick = g_lapicTickCount + 100; // ~100ms
        SchedulerBlock(self);
    }
}

void KernelConsoleSpawn()
{
    Process* thread = KernelThreadCreate("kconsole", KernelConsoleThread, nullptr);
    if (!thread)
    {
        SerialPuts("KCONSOLE: failed to create thread\n");
        return;
    }
    SchedulerAddProcess(thread);
    SerialPrintf("KCONSOLE: spawned pid=%u\n", thread->pid);
}

// --- TCP debug server thread -----------------------------------------------

static constexpr uint16_t DEBUG_TCP_PORT = 1234;
static constexpr uint32_t TCP_LINE_LEN   = 160;
static constexpr uint32_t TCP_BATCH      = 32; // lines per send batch

static void DebugTcpThread(void* /*arg*/)
{
    // Create listen socket
    int listenSock = SockCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock < 0)
    {
        SerialPuts("DEBUG_TCP: failed to create socket\n");
        return;
    }

    SockAddrIn addr;
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(DEBUG_TCP_PORT);
    addr.sin_addr   = 0; // INADDR_ANY

    if (SockBind(listenSock, &addr) < 0)
    {
        SerialPuts("DEBUG_TCP: bind failed\n");
        SockClose(listenSock);
        return;
    }

    if (SockListen(listenSock, 2) < 0)
    {
        SerialPuts("DEBUG_TCP: listen failed\n");
        SockClose(listenSock);
        return;
    }

    SerialPrintf("DEBUG_TCP: listening on port %u\n", DEBUG_TCP_PORT);

    // Accept loop — handle one client at a time
    for (;;)
    {
        SockAddrIn peer;
        int clientSock = SockAccept(listenSock, &peer);
        if (clientSock < 0)
        {
            // Brief pause then retry
            Process* self = ProcessCurrent();
            self->wakeupTick = g_lapicTickCount + 500;
            SchedulerBlock(self);
            continue;
        }

        SerialPrintf("DEBUG_TCP: client connected (sock %d)\n", clientSock);

        // Send banner
        const char* banner = "=== Brook OS Debug Console ===\r\n";
        SockSend(clientSock, banner, strlen(banner));

        // Stream ring buffer content
        uint32_t cursor = 0; // start from beginning of ring
        char lineBuf[TCP_BATCH][TCP_LINE_LEN + 1];

        for (;;)
        {
            uint32_t count = DebugOverlayReadFrom(&cursor,
                reinterpret_cast<char*>(lineBuf), TCP_BATCH, TCP_LINE_LEN + 1);

            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t len = 0;
                while (lineBuf[i][len]) len++;
                // Append \r\n
                lineBuf[i][len] = '\r';
                lineBuf[i][len + 1] = '\n';
                int ret = SockSend(clientSock, lineBuf[i], len + 2);
                if (ret < 0)
                    goto client_done;
            }

            if (count == 0)
            {
                // No new data — sleep briefly
                Process* self = ProcessCurrent();
                self->wakeupTick = g_lapicTickCount + 200; // ~200ms
                SchedulerBlock(self);
            }
        }

    client_done:
        SerialPuts("DEBUG_TCP: client disconnected\n");
        SockClose(clientSock);
    }
}

void DebugTcpSpawn()
{
    Process* thread = KernelThreadCreate("debug_tcp", DebugTcpThread, nullptr);
    if (!thread)
    {
        SerialPuts("DEBUG_TCP: failed to create thread\n");
        return;
    }
    SchedulerAddProcess(thread);
    SerialPrintf("DEBUG_TCP: spawned pid=%u\n", thread->pid);
}

} // namespace brook
