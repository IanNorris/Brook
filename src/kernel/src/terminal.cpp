#include "terminal.h"
#include "pipe.h"
#include "process.h"
#include "scheduler.h"
#include "compositor.h"
#include "window.h"
#include "font_atlas.h"
#include "input.h"
#include "memory/heap.h"
#include "kprintf.h"
#include "serial.h"
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
// Glyph rendering into the terminal VFB
// ---------------------------------------------------------------------------

static void TermRenderGlyph(Terminal* t, char ch)
{
    if (!t->vfb || !g_fontAtlas.pixels) return;

    // Handle control characters
    if (ch == '\n')
    {
        t->curX = 0;
        t->curY++;
        if (t->curY >= t->rows)
        {
            // Scroll up one line
            uint32_t lineH = static_cast<uint32_t>(g_fontAtlas.lineHeight);
            uint32_t scrollBytes = t->vfbW * (t->vfbH - lineH);
            auto* dst = reinterpret_cast<uint8_t*>(t->vfb);
            auto* src = dst + t->vfbW * lineH * 4;
            for (uint32_t i = 0; i < scrollBytes * 4; i++)
                dst[i] = src[i];
            // Clear last line
            uint32_t clearStart = t->vfbW * (t->vfbH - lineH);
            for (uint32_t i = clearStart; i < t->vfbW * t->vfbH; i++)
                t->vfb[i] = t->bgColor;
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

    t->curX++;
    if (t->curX >= t->cols)
    {
        t->curX = 0;
        t->curY++;
        if (t->curY >= t->rows)
        {
            // Scroll (same as \n)
            uint32_t lineH = static_cast<uint32_t>(g_fontAtlas.lineHeight);
            uint32_t scrollBytes = t->vfbW * (t->vfbH - lineH);
            auto* dst = reinterpret_cast<uint8_t*>(t->vfb);
            auto* src = dst + t->vfbW * lineH * 4;
            for (uint32_t i = 0; i < scrollBytes * 4; i++)
                dst[i] = src[i];
            uint32_t clearStart = t->vfbW * (t->vfbH - lineH);
            for (uint32_t i = clearStart; i < t->vfbW * t->vfbH; i++)
                t->vfb[i] = t->bgColor;
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
        if (mode == 2 || mode == 3)
        {
            // Clear entire screen
            for (uint32_t i = 0; i < t->vfbW * t->vfbH; i++)
                t->vfb[i] = t->bgColor;
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
        }
        else if (mode == 1) // clear from start to cursor
        {
            for (uint32_t y = pixY; y < pixY + lineH && y < t->vfbH; y++)
                for (uint32_t x = 0; x <= pixX && x < t->vfbW; x++)
                    t->vfb[y * t->vfbW + x] = t->bgColor;
        }
        else if (mode == 2) // clear entire line
        {
            for (uint32_t y = pixY; y < pixY + lineH && y < t->vfbH; y++)
                for (uint32_t x = 0; x < t->vfbW; x++)
                    t->vfb[y * t->vfbW + x] = t->bgColor;
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

    KPrintf("TERMINAL: thread started for pid %u, reading from pipe\n", self->pid);

    while (t->active)
    {
        char buf[256];

        // Register as reader waiter so pipe writers wake us immediately
        outPipe->readerWaiter = self;
        uint32_t n = outPipe->read(buf, sizeof(buf));

        if (n == 0)
        {
            // Check if child still alive
            if (t->child && t->child->state == ProcessState::Terminated)
            {
                KPrintf("TERMINAL: child exited, closing\n");
                break;
            }
            // Block until writer wakes us or timeout
            self->wakeupTick = g_lapicTickCount + 50;
            SchedulerBlock(self);
            continue;
        }

        outPipe->readerWaiter = nullptr;

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
            t->child->fbDirty = 1;
            t->dirty = false;
            CompositorWake();
        }
    }

    t->active = false;
    DbgPrintf("TERMINAL: thread exiting\n");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int TerminalCreate(uint32_t clientW, uint32_t clientH)
{
    KPrintf("TERMINAL: creating %ux%u, count=%u\n", clientW, clientH, g_terminalCount);
    if (g_terminalCount >= MAX_TERMINALS) { KPrintf("TERMINAL: max terminals reached\n"); return -1; }

    int idx = static_cast<int>(g_terminalCount);
    Terminal* t = &g_terminals[idx];

    // Allocate VFB
    uint32_t vfbSize = clientW * clientH * 4;
    t->vfb = static_cast<uint32_t*>(kmalloc(vfbSize));
    if (!t->vfb) { KPrintf("TERMINAL: vfb alloc failed (%u bytes)\n", vfbSize); return -1; }

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

    // Create pipes
    auto* stdinPipe = static_cast<PipeBuffer*>(kmalloc(sizeof(PipeBuffer)));
    auto* stdoutPipe = static_cast<PipeBuffer*>(kmalloc(sizeof(PipeBuffer)));
    if (!stdinPipe || !stdoutPipe)
    {
        KPrintf("TERMINAL: pipe alloc failed\n");
        if (stdinPipe) kfree(stdinPipe);
        if (stdoutPipe) kfree(stdoutPipe);
        kfree(t->vfb);
        t->vfb = nullptr;
        return -1;
    }

    // Zero-init pipes
    for (uint64_t i = 0; i < sizeof(PipeBuffer); i++)
    {
        reinterpret_cast<uint8_t*>(stdinPipe)[i] = 0;
        reinterpret_cast<uint8_t*>(stdoutPipe)[i] = 0;
    }
    stdinPipe->readers = 1;
    stdinPipe->writers = 1;
    stdoutPipe->readers = 1;
    stdoutPipe->writers = 1;

    t->stdinPipe = stdinPipe;
    t->stdoutPipe = stdoutPipe;

    // Set up FdEntry array for bash: fd0=stdin read, fd1=stdout write, fd2=stdout write
    FdEntry stdFds[3] = {};
    stdFds[0].type = FdType::Pipe;
    stdFds[0].flags = 0;       // read end
    stdFds[0].handle = stdinPipe;
    stdFds[0].refCount = 1;

    stdFds[1].type = FdType::Pipe;
    stdFds[1].flags = 1;       // write end
    stdFds[1].statusFlags = 1; // O_WRONLY
    stdFds[1].handle = stdoutPipe;
    stdFds[1].refCount = 1;

    stdFds[2].type = FdType::Pipe;
    stdFds[2].flags = 1;       // write end
    stdFds[2].statusFlags = 1; // O_WRONLY
    stdFds[2].handle = stdoutPipe;
    stdFds[2].refCount = 1;

    // Load bash ELF
    VnodeStat st;
    const char* bashPath = "/boot/BIN/BASH";
    if (VfsStatPath(bashPath, &st) != 0)
    {
        KPrintf("TERMINAL: bash not found at %s\n", bashPath);
        kfree(stdinPipe);
        kfree(stdoutPipe);
        kfree(t->vfb);
        t->vfb = nullptr;
        return -1;
    }

    auto* elfData = static_cast<uint8_t*>(kmalloc(st.size));
    if (!elfData) { KPrintf("TERMINAL: elf alloc failed (%lu bytes)\n", st.size); return -1; }

    Vnode* vn = VfsOpen(bashPath, 0);
    if (!vn) { KPrintf("TERMINAL: VfsOpen failed for %s\n", bashPath); kfree(elfData); return -1; }

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
        KPrintf("TERMINAL: bash ELF read incomplete (%lu remaining)\n", remaining);
        kfree(elfData);
        return -1;
    }

    // Spawn bash with piped FDs
    const char* bashArgv[] = { "bash", nullptr };
    const char* bashEnvp[] = {
        "HOME=/",
        "PATH=/boot/BIN",
        "TERM=dumb",
        "PS1=$ ",
        nullptr
    };

    Process* child = ProcessCreate(elfData, st.size, 1, bashArgv, 4, bashEnvp, stdFds);
    kfree(elfData);

    if (!child)
    {
        KPrintf("TERMINAL: failed to spawn bash\n");
        kfree(stdinPipe);
        kfree(stdoutPipe);
        kfree(t->vfb);
        t->vfb = nullptr;
        return -1;
    }

    t->child = child;

    // Create the terminal's kernel thread
    Process* thread = KernelThreadCreate("terminal", TerminalThreadFn, t);
    if (!thread)
    {
        KPrintf("TERMINAL: failed to create thread\n");
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

    g_terminalCount++;
    KPrintf("TERMINAL: created terminal %d, bash pid=%u, thread pid=%u\n",
              idx, child->pid, thread->pid);

    return idx;
}

void TerminalWriteInput(int termIdx, const char* data, uint32_t len)
{
    if (termIdx < 0 || termIdx >= static_cast<int>(g_terminalCount)) return;
    Terminal* t = &g_terminals[termIdx];
    if (!t->active) return;

    auto* pipe = static_cast<PipeBuffer*>(t->stdinPipe);

    // Local echo: always render printable input to the terminal VFB.
    // bash/readline turns off ECHO and handles display via stdout writes,
    // but with TERM=dumb, readline's echo is delayed (buffered in the pipe).
    // Immediate local echo gives responsive feedback. If readline also echoes,
    // both render to the same cursor position — harmless overwrite.
    for (uint32_t i = 0; i < len; i++)
    {
        char ch = data[i];
        if (ch == '\r') ch = '\n';
        if (ch >= 32 || ch == '\n' || ch == '\b')
        {
            TermRenderGlyph(t, ch);
        }
    }
    t->dirty = true;
    t->child->fbDirty = 1;
    CompositorWake();

    pipe->write(data, len);

    // Wake bash if it's blocked on stdin read
    if (t->child && t->child->state == ProcessState::Blocked)
    {
        t->child->pendingWakeup = 1;
        SchedulerUnblock(t->child);
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

} // namespace brook
