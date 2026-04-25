// Brook kernel shell — boot script executor and interactive command line.
//
// Provides a simple command interpreter that can:
//   - Execute a startup script (/boot/INIT.RC)
//   - Accept interactive commands from the keyboard
//   - Spawn user-mode ELF processes with compositor auto-tiling
//
// This runs in kernel mode on the BSP.  It's deliberately minimal —
// just enough to launch programs and inspect system state.

#include "shell.h"
#include "vfs.h"
#include "process.h"
#include "scheduler.h"
#include "compositor.h"
#include "window.h"
#include "terminal.h"
#include "tty.h"
#include "display.h"
#include "font_atlas.h"
#include "kprintf.h"
#include "keyboard.h"
#include "memory/heap.h"
#include "memory/virtual_memory.h"
#include "serial.h"
#include "smp.h"
#include "panic.h"
#include "memory/physical_memory.h"
#include "apic.h"
#include "klog.h"
#include "profiler.h"
#include "module.h"
#include "audio.h"
#include "debug_overlay.h"

// Strace control (defined in syscall.cpp)
bool StraceEnablePid(uint32_t pid, bool enable);
int  StraceEnableName(const char* name, bool enable);
void StraceEnableAll(bool enable);

namespace brook {

// ---------------------------------------------------------------------------
// Shell state
// ---------------------------------------------------------------------------

static constexpr uint32_t MAX_LINE    = 256;
static constexpr uint32_t MAX_ARGS    = 16;


// Auto-tiling grid state
static uint32_t g_gridCols  = 0;    // 0 = auto-compute
static uint32_t g_gridRows  = 0;
static uint8_t  g_scale     = 3;    // Default downscale factor
static uint32_t g_vfbWidth  = 640;  // Default VFB dimensions
static uint32_t g_vfbHeight = 400;
static int32_t  g_winX     = -1;   // -1 = auto-cascade
static int32_t  g_winY     = -1;
static bool     g_ttyFull   = false; // When true, skip VFB for spawned processes
// 'set vfb auto/full' fullscreen mode: only the first subsequent spawn (the
// rendering server, e.g. waylandd) gets a VFB.  Subsequent processes
// (Wayland clients) talk to that server over the wayland socket and must
// NOT have their own VFB — otherwise the compositor blits their black
// 1920x1080 backbuffer over the server's, producing a flicker-then-black
// effect that's been mistaken for "client only renders one frame".
static bool     g_vfbFullscreenMode = false;
static bool     g_vfbFullscreenConsumed = false;

// Track spawned process count for auto-tiling placement
static uint32_t g_spawnCount = 0;

// True while executing a boot script (before scheduler starts)
static bool g_scriptMode = false;

// Default environment for spawned processes
static const char* g_defaultEnvp[] = {
    "HOME=/",
    "PATH=/nix/profile/bin:/nix/bin:"
          "/nix/store/xkqd49dmldkqn4xk6dlm640f5blbv6hp-curl-8.18.0-bin/bin:"
          "/nix/store/g6mlwdvpg92rchq352ll7jbi0pz7h43r-xz-5.8.2-bin/bin:"
          "/nix/store/v8sa6r6q037ihghxfbwzjj4p59v2x0pv-bash-5.3p9/bin:"
          "/boot/BIN:/boot/bin:/usr/bin:/bin",
    "TERM=linux",
    "SHELL=/boot/BIN/BASH",
    "USER=root",
    "LOGNAME=root",
    "SSL_CERT_FILE=/nix/store/mg063aj0crwhchqayf2qbyf28k6mlrxm-nss-cacert-3.121/etc/ssl/certs/ca-bundle.crt",
    "CURL_CA_BUNDLE=/nix/store/mg063aj0crwhchqayf2qbyf28k6mlrxm-nss-cacert-3.121/etc/ssl/certs/ca-bundle.crt",
    "NETSURFRES=/nix/store/m64fp6340nd6s98fawnwvvkx4v81660k-netsurf-brook-3.11-brook/share/netsurf/",
    "WAYLAND_DISPLAY=wayland-0",
    "XDG_RUNTIME_DIR=/tmp",
    "TMPDIR=/tmp",
    "XCURSOR_PATH=/nix/store/aaipci08wnfa7d64lmd5vyn9l2bkihg5-brook-cursor-theme-0.1/share/icons",
    "XCURSOR_THEME=default",
    "XCURSOR_SIZE=24",
    nullptr
};

// ---------------------------------------------------------------------------
// String helpers (kernel has no libc)
// ---------------------------------------------------------------------------

static uint32_t StrLen(const char* s)
{
    uint32_t n = 0;
    while (s[n]) ++n;
    return n;
}

static bool StrEq(const char* a, const char* b)
{
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static void StrCopy(char* dst, const char* src, uint32_t maxLen)
{
    uint32_t i = 0;
    while (src[i] && i < maxLen - 1) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

static bool IsWhitespace(char c) { return c == ' ' || c == '\t'; }
static bool IsDigit(char c) { return c >= '0' && c <= '9'; }

static uint32_t ParseUint(const char* s)
{
    uint32_t v = 0;
    while (IsDigit(*s)) { v = v * 10 + (*s - '0'); ++s; }
    return v;
}

// ---------------------------------------------------------------------------
// Auto-tiling: compute grid position for the Nth process
// ---------------------------------------------------------------------------

static void ComputeGridPosition(uint32_t index, int16_t* outX, int16_t* outY,
                                 uint32_t* outCols, uint32_t* outRows)
{
    uint32_t physW, physH;
    CompositorGetPhysDims(&physW, &physH);

    uint32_t cellW = g_vfbWidth / g_scale;
    uint32_t cellH = g_vfbHeight / g_scale;
    if (cellW == 0) cellW = 1;
    if (cellH == 0) cellH = 1;

    uint32_t cols = g_gridCols;
    uint32_t rows = g_gridRows;

    if (cols == 0 || rows == 0)
    {
        // Auto-compute: fit as many as possible
        cols = physW / cellW;
        rows = physH / cellH;
        if (cols == 0) cols = 1;
        if (rows == 0) rows = 1;
    }

    uint32_t col = index % cols;
    uint32_t row = (index / cols) % rows;

    // Center the grid
    int16_t gridX0 = static_cast<int16_t>((physW - cellW * cols) / 2);
    int16_t gridY0 = static_cast<int16_t>((physH - cellH * rows) / 2);

    *outX = gridX0 + static_cast<int16_t>(col * cellW);
    *outY = gridY0 + static_cast<int16_t>(row * cellH);
    *outCols = cols;
    *outRows = rows;
}

// ---------------------------------------------------------------------------
// Load an ELF from VFS path into a buffer
// ---------------------------------------------------------------------------

static uint8_t* LoadElf(const char* path, uint64_t* outSize)
{
    Vnode* vn = VfsOpen(path, 0);
    if (!vn) return nullptr;

    constexpr uint64_t MAX_ELF_SIZE = 32 * 1024 * 1024;
    constexpr uint64_t ELF_BUF_PAGES = MAX_ELF_SIZE / 4096;

    VirtualAddress bufAddr = VmmAllocPages(ELF_BUF_PAGES,
        VMM_WRITABLE, MemTag::Heap, KernelPid);
    if (!bufAddr)
    {
        VfsClose(vn);
        return nullptr;
    }

    auto* buf = reinterpret_cast<uint8_t*>(bufAddr.raw());
    uint64_t size = 0;
    uint64_t offset = 0;
    while (size < MAX_ELF_SIZE)
    {
        int ret = VfsRead(vn, buf + size, 4096, &offset);
        if (ret <= 0) break;
        size += static_cast<uint64_t>(ret);
    }
    VfsClose(vn);

    *outSize = size;
    return buf;
}

static void FreeElfBuffer(uint8_t* buf)
{
    constexpr uint64_t MAX_ELF_SIZE = 32 * 1024 * 1024;
    constexpr uint64_t ELF_BUF_PAGES = MAX_ELF_SIZE / 4096;
    VmmFreePages(VirtualAddress(reinterpret_cast<uint64_t>(buf)), ELF_BUF_PAGES);
}

// ---------------------------------------------------------------------------
// Try to resolve a binary path: check path as-is, then /boot/BIN/<NAME>
// ---------------------------------------------------------------------------

static bool ResolveBinaryPath(const char* name, char* outPath, uint32_t maxLen)
{
    // If it starts with '/', try it directly
    if (name[0] == '/')
    {
        VnodeStat st;
        if (VfsStatPath(name, &st) == 0 && !st.isDir)
        {
            StrCopy(outPath, name, maxLen);
            return true;
        }
    }

    // Try /boot/BIN/<NAME> (upper-case)
    char upper[64] = {};
    uint32_t i = 0;
    while (name[i] && i < 62)
    {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        upper[i] = c;
        ++i;
    }
    upper[i] = '\0';

    // Build path: /boot/BIN/<UPPER>
    char tryPath[128] = "/boot/BIN/";
    uint32_t pLen = StrLen(tryPath);
    for (uint32_t j = 0; upper[j] && pLen < 126; ++j)
        tryPath[pLen++] = upper[j];
    tryPath[pLen] = '\0';

    VnodeStat st;
    if (VfsStatPath(tryPath, &st) == 0 && !st.isDir)
    {
        StrCopy(outPath, tryPath, maxLen);
        return true;
    }

    // Try /boot/<NAME> (upper-case)
    char tryPath2[128] = "/boot/";
    pLen = StrLen(tryPath2);
    for (uint32_t j = 0; upper[j] && pLen < 126; ++j)
        tryPath2[pLen++] = upper[j];
    tryPath2[pLen] = '\0';

    if (VfsStatPath(tryPath2, &st) == 0 && !st.isDir)
    {
        StrCopy(outPath, tryPath2, maxLen);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Shebang resolution — peek first bytes of a file; if "#!interp [arg]\n",
// copy interpreter and optional argument into caller buffers. Returns true
// if a shebang was present and parsed.
// ---------------------------------------------------------------------------

static bool PeekShebang(const char* path, char* outInterp, uint32_t interpSize,
                        char* outArg, uint32_t argSize)
{
    Vnode* vn = VfsOpen(path, 0);
    if (!vn) return false;

    char header[256] = {};
    uint64_t offset = 0;
    int n = VfsRead(vn, reinterpret_cast<uint8_t*>(header), sizeof(header) - 1, &offset);
    VfsClose(vn);
    if (n < 4 || header[0] != '#' || header[1] != '!') return false;

    // Skip leading whitespace after "#!"
    int i = 2;
    while (i < n && (header[i] == ' ' || header[i] == '\t')) ++i;

    // Interpreter path runs until whitespace or newline
    uint32_t j = 0;
    while (i < n && header[i] != ' ' && header[i] != '\t'
           && header[i] != '\n' && header[i] != '\r'
           && j + 1 < interpSize)
    {
        outInterp[j++] = header[i++];
    }
    outInterp[j] = '\0';
    if (j == 0) return false;

    // Optional single argument — everything up to newline (preserving spaces).
    // Traditional Unix: only ONE argument allowed; remaining text is one string.
    outArg[0] = '\0';
    while (i < n && (header[i] == ' ' || header[i] == '\t')) ++i;
    uint32_t k = 0;
    while (i < n && header[i] != '\n' && header[i] != '\r' && k + 1 < argSize)
    {
        outArg[k++] = header[i++];
    }
    // Strip trailing whitespace
    while (k > 0 && (outArg[k-1] == ' ' || outArg[k-1] == '\t')) --k;
    outArg[k] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
// Spawn a process from a binary path with arguments
// ---------------------------------------------------------------------------

static Process* SpawnProcess(const char* path, int argc, const char* const* argv)
{
    uint64_t elfSize = 0;
    uint8_t* elfBuf = LoadElf(path, &elfSize);
    if (!elfBuf)
    {
        KPrintf("shell: cannot load '%s'\n", path);
        return nullptr;
    }

    DbgPrintf("SHELL: loaded '%s' (%lu bytes)\n", path, elfSize);

    // Disable interrupts during process creation to avoid timer ISR interference
    __asm__ volatile("cli");

    // Count environment entries
    int envc = 0;
    while (g_defaultEnvp[envc]) ++envc;

    Process* proc = ProcessCreate(elfBuf, elfSize, argc, argv,
                                   envc, g_defaultEnvp);

    __asm__ volatile("sti");

    FreeElfBuffer(elfBuf);

    if (!proc)
    {
        KPrintf("shell: failed to create process from '%s'\n", path);
        return nullptr;
    }

    // Name the process after the binary
    const char* baseName = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/') baseName = p + 1;

    uint32_t ni = 0;
    while (baseName[ni] && ni < 27)
    {
        proc->name[ni] = baseName[ni];
        ++ni;
    }
    // Append spawn index
    proc->name[ni++] = '_';
    if (g_spawnCount >= 10) proc->name[ni++] = static_cast<char>('0' + (g_spawnCount / 10) % 10);
    proc->name[ni++] = static_cast<char>('0' + g_spawnCount % 10);
    proc->name[ni] = '\0';

    // Set up compositor placement
    bool skipVfb = g_ttyFull;
    if (g_vfbFullscreenMode)
    {
        if (g_vfbFullscreenConsumed)
        {
            // Subsequent processes in fullscreen mode are wayland clients —
            // they don't own a VFB, the renderer (first process) does.
            skipVfb = true;
        }
        else
        {
            g_vfbFullscreenConsumed = true;
        }
    }
    if (!skipVfb)
    {
        if (WmIsActive())
        {
            // WM mode: create a window for this process
            int16_t winX = (g_winX >= 0) ? static_cast<int16_t>(g_winX)
                         : static_cast<int16_t>(40 + (g_spawnCount % 8) * 30);
            int16_t winY = (g_winY >= 0) ? static_cast<int16_t>(g_winY)
                         : static_cast<int16_t>(40 + (g_spawnCount % 8) * 30);
            g_winX = -1;  // Reset after use
            g_winY = -1;
            uint16_t vfbW = static_cast<uint16_t>(g_vfbWidth);
            uint16_t vfbH = static_cast<uint16_t>(g_vfbHeight);
            uint8_t upscale = g_scale;

            // Display size = VFB × upscale
            uint16_t displayW = static_cast<uint16_t>(vfbW * upscale);
            uint16_t displayH = static_cast<uint16_t>(vfbH * upscale);

            // Set up the VFB at native resolution
            CompositorSetupProcess(proc, winX + WM_BORDER_WIDTH,
                                   winY + WM_TITLE_BAR_HEIGHT + WM_BORDER_WIDTH,
                                   vfbW, vfbH, 1);

            WmCreateWindow(proc, winX, winY, displayW, displayH, proc->name, upscale);
            DbgPrintf("SHELL: spawned '%s' pid=%u as WM window at (%d,%d) %ux%u (vfb %ux%u scale %u)\n",
                          proc->name, proc->pid, winX, winY, displayW, displayH, vfbW, vfbH, upscale);
        }
        else
        {
            int16_t destX, destY;
            uint32_t cols, rows;
            ComputeGridPosition(g_spawnCount, &destX, &destY, &cols, &rows);
            CompositorSetupProcess(proc, destX, destY, g_vfbWidth, g_vfbHeight, g_scale);
            DbgPrintf("SHELL: spawned '%s' pid=%u at (%d,%d) grid=%ux%u scale=%u\n",
                          proc->name, proc->pid, destX, destY, cols, rows, g_scale);
        }
    }
    SchedulerAddProcess(proc);
    KPrintf("  → %s (pid %u)\n", proc->name, proc->pid);

    ++g_spawnCount;
    return proc;
}

// ---------------------------------------------------------------------------
// Parse a line into argv tokens (modifies the line buffer in-place)
// ---------------------------------------------------------------------------

static int ParseLine(char* line, const char* argv[], uint32_t maxArgs)
{
    int argc = 0;
    char* p = line;

    while (*p && argc < static_cast<int>(maxArgs))
    {
        // Skip whitespace
        while (IsWhitespace(*p)) ++p;
        if (*p == '\0' || *p == '#') break;

        // Handle quoted strings
        if (*p == '"')
        {
            ++p;
            argv[argc++] = p;
            while (*p && *p != '"') ++p;
            if (*p == '"') *p++ = '\0';
        }
        else
        {
            argv[argc++] = p;
            while (*p && !IsWhitespace(*p)) ++p;
            if (*p) *p++ = '\0';
        }
    }

    return argc;
}

// ---------------------------------------------------------------------------
// Execute a single parsed command
// ---------------------------------------------------------------------------

// Forward declarations for built-in commands
static void CmdHelp();
static void CmdPs();
static void CmdMem();
static void CmdLs(int argc, const char* const* argv);

// Background wallpaper loader thread — reads ~8MB WALLPAPER.RAW without
// blocking the shell/WM init path.
static void WallpaperLoaderThread(void* /*arg*/)
{
    using namespace brook;

    VnodeStat st;
    if (VfsStatPath("/boot/WALLPAPER.RAW", &st) != 0 || st.size <= 8)
    {
        KLog("wallpaper: file not found or too small");
        return;
    }

    auto* pixels = static_cast<uint32_t*>(kmalloc(st.size));
    if (!pixels)
    {
        KLog("wallpaper: alloc failed (%llu bytes)", st.size);
        return;
    }

    Vnode* vn = VfsOpen("/boot/WALLPAPER.RAW", 0);
    if (!vn)
    {
        kfree(pixels);
        KLog("wallpaper: open failed");
        return;
    }

    uint64_t off = 0;
    uint64_t remaining = st.size;
    auto* dest = reinterpret_cast<uint8_t*>(pixels);
    while (remaining > 0)
    {
        uint64_t chunk = remaining > 65536 ? 65536 : remaining;
        int rd = VfsRead(vn, dest + off, chunk, &off);
        if (rd <= 0) break;
        remaining -= rd;
    }
    VfsClose(vn);

    if (remaining != 0)
    {
        kfree(pixels);
        KLog("wallpaper: read error");
        return;
    }

    uint32_t wpW = pixels[0];
    uint32_t wpH = pixels[1];
    uint64_t expected = 8 + static_cast<uint64_t>(wpW) * wpH * 4;
    if (st.size == expected && wpW > 0 && wpH > 0
        && wpW <= 3840 && wpH <= 2160)
    {
        CompositorSetWallpaper(pixels + 2, wpW, wpH);
        KLog("wallpaper: loaded (%ux%u)", wpW, wpH);
    }
    else
    {
        kfree(pixels);
        KLog("wallpaper: bad header (%ux%u, size %llu)", wpW, wpH, st.size);
    }
}

static int ExecCommand(int argc, const char* const* argv)
{
    if (argc == 0) return 0;

    const char* cmd = argv[0];

    // Built-in: help
    if (StrEq(cmd, "help") || StrEq(cmd, "?"))
    {
        CmdHelp();
        return 0;
    }

    // Built-in: clear
    if (StrEq(cmd, "clear") || StrEq(cmd, "cls"))
    {
        TtyClear();
        return 0;
    }

    // Built-in: source <script> — execute a boot script
    if (StrEq(cmd, "source") && argc >= 2)
    {
        char resolved[128];
        // Try as-is first, then with / prefix
        if (argv[1][0] == '/')
        {
            for (uint32_t i = 0; i < sizeof(resolved) - 1 && argv[1][i]; i++)
                resolved[i] = argv[1][i];
            resolved[sizeof(resolved) - 1] = '\0';
        }
        else
        {
            resolved[0] = '/';
            uint32_t i = 0;
            for (; i < sizeof(resolved) - 2 && argv[1][i]; i++)
                resolved[i + 1] = argv[1][i];
            resolved[i + 1] = '\0';
        }
        ShellExecScript(resolved);
        return 0;
    }

    // Built-in: ps
    if (StrEq(cmd, "ps"))
    {
        CmdPs();
        return 0;
    }

    // Built-in: mem
    if (StrEq(cmd, "mem"))
    {
        CmdMem();
        return 0;
    }

    // Built-in: ls [path]
    if (StrEq(cmd, "ls") || StrEq(cmd, "dir"))
    {
        CmdLs(argc, argv);
        return 0;
    }

    // Built-in: set <key> [<value>]
    if (StrEq(cmd, "set") && argc >= 2)
    {
        // 'set wm' — enable window manager mode (no value needed)
        if (StrEq(argv[1], "wm"))
        {
            WmInit();
            WmSetActive(true);
            g_ttyFull = false; // WM takes over the screen

            // Suppress TTY framebuffer rendering — the kernel console WM window
            // handles log display, and TTY writes would briefly flash under the
            // compositor each frame.
            TtySuppressDisplay(true);

            // Load wallpaper asynchronously so WM startup isn't blocked.
            {
                using namespace brook;
                Process* wpThread = KernelThreadCreate("wp_loader",
                    WallpaperLoaderThread, nullptr);
                if (wpThread)
                    SchedulerAddProcess(wpThread);
            }

            KPrintf("window manager: enabled\n");

            // Spawn kernel console window (shows kernel log in a WM window).
            KernelConsoleSpawn();

            // Spawn TCP debug server (streams kernel log on port 1234).
            DebugTcpSpawn();

            return 0;
        }

        if (argc < 3)
        {
            KPrintf("set: missing value\n");
            return -1;
        }

        if (StrEq(argv[1], "grid"))
        {
            // Parse "CxR" format
            const char* val = argv[2];
            uint32_t c = ParseUint(val);
            while (*val && *val != 'x' && *val != 'X') ++val;
            if (*val) ++val;
            uint32_t r = ParseUint(val);
            if (c > 0 && r > 0)
            {
                g_gridCols = c;
                g_gridRows = r;
                KPrintf("grid: %ux%u\n", c, r);
            }
            else
            {
                g_gridCols = 0;
                g_gridRows = 0;
                KPrintf("grid: auto\n");
            }
        }
        else if (StrEq(argv[1], "scale"))
        {
            uint32_t s = ParseUint(argv[2]);
            if (s >= 1 && s <= 16)
            {
                g_scale = static_cast<uint8_t>(s);
                KPrintf("scale: %u\n", s);
            }
        }
        else if (StrEq(argv[1], "vfb"))
        {
            // 'set vfb auto' / 'set vfb full' — use physical FB size at scale 1
            // (i.e. fullscreen 1:1).  Convenient for wayland_flower.rc and
            // similar single-app shells where you want the client to occupy
            // the whole screen.
            if (StrEq(argv[2], "auto") || StrEq(argv[2], "full"))
            {
                uint32_t physW = 0, physH = 0;
                CompositorGetPhysDims(&physW, &physH);
                if (physW > 0 && physH > 0)
                {
                    g_vfbWidth  = physW;
                    g_vfbHeight = physH;
                    g_scale     = 1;
                    g_vfbFullscreenMode = true;
                    g_vfbFullscreenConsumed = false;
                    // Fullscreen client takes over the screen — silence the
                    // TTY framebuffer writer so kernel log lines don't draw
                    // over the client's surface.
                    TtySuppressDisplay(true);
                    KPrintf("vfb: %ux%u (physical, 1:1, tty suppressed, fullscreen)\n", physW, physH);
                }
                else
                {
                    KPrintf("vfb: physical dims unavailable\n");
                }
                return 0;
            }
            // Parse "WxH" format
            const char* val = argv[2];
            uint32_t w = ParseUint(val);
            while (*val && *val != 'x' && *val != 'X') ++val;
            if (*val) ++val;
            uint32_t h = ParseUint(val);
            if (w > 0 && h > 0)
            {
                g_vfbWidth = w;
                g_vfbHeight = h;
                KPrintf("vfb: %ux%u\n", w, h);
            }
        }
        else if (StrEq(argv[1], "pos"))
        {
            // Parse "XxY" or "X,Y" format for window position
            const char* val = argv[2];
            int32_t x = static_cast<int32_t>(ParseUint(val));
            while (*val && *val != 'x' && *val != 'X' && *val != ',') ++val;
            if (*val) ++val;
            int32_t y = static_cast<int32_t>(ParseUint(val));
            g_winX = x;
            g_winY = y;
            KPrintf("pos: %d,%d\n", x, y);
        }
        else if (StrEq(argv[1], "tty"))
        {
            if (StrEq(argv[2], "full"))
            {
                brook::TtySetRegion(0, 0, 0, 0);
                g_ttyFull = true;
                KPrintf("tty: full screen\n");
            }
            else if (StrEq(argv[2], "bar"))
            {
                constexpr uint32_t STATUS_BAR_LINES = 6;
                uint32_t barH = STATUS_BAR_LINES * static_cast<uint32_t>(brook::g_fontAtlas.lineHeight);
                uint32_t fbW = 0, fbH = 0, fbStride = 0;
                uint32_t* fbPtr = nullptr;
                brook::TtyGetFramebuffer(&fbPtr, &fbW, &fbH, &fbStride);
                if (fbH > barH)
                    brook::TtySetRegion(0, fbH - barH, fbW, barH);
                g_ttyFull = false;
                KPrintf("tty: status bar\n");
            }
        }
        else
        {
            KPrintf("set: unknown key '%s'\n", argv[1]);
        }
        return 0;
    }

    // Built-in: terminal — spawn a terminal window (requires WM mode)
    if (StrEq(cmd, "terminal"))
    {
        if (!WmIsActive())
        {
            KPrintf("terminal: window manager not active (use 'set wm' first)\n");
            return -1;
        }

        // Default terminal size: 800x600 client area
        uint32_t clientW = 800;
        uint32_t clientH = 600;

        int termIdx = TerminalCreate(clientW, clientH);
        if (termIdx < 0)
        {
            KPrintf("terminal: failed to create terminal\n");
            return -1;
        }

        // Create a WM window for the terminal
        Terminal* t = TerminalGet(termIdx);
        if (t && t->child)
        {
            int16_t winX = static_cast<int16_t>(60 + (termIdx % 6) * 40);
            int16_t winY = static_cast<int16_t>(60 + (termIdx % 6) * 40);

            // Set up compositor dest coords — terminal VFB is already set by TerminalCreate.
            // Do NOT call CompositorSetupProcess (it would allocate a new VFB).
            t->child->fbDestX = winX + WM_BORDER_WIDTH;
            t->child->fbDestY = winY + WM_TITLE_BAR_HEIGHT + WM_BORDER_WIDTH;
            t->child->fbScale = 1;
            t->child->fbDirty = 1;

            WmCreateWindow(t->child, winX, winY,
                          static_cast<uint16_t>(clientW),
                          static_cast<uint16_t>(clientH), "Terminal");

            KPrintf("terminal: created (bash pid %u)\n", t->child->pid);
        }
        return 0;
    }

    // Built-in: run [--as <name>] <path> [args...] — explicit run command
    // --as overrides argv[0] for the spawned process (useful for busybox applets)
    if (StrEq(cmd, "run") && argc >= 2)
    {
        int pathIdx = 1;
        const char* argv0Override = nullptr;
        bool strace = false;

        // Parse flags before the path
        while (pathIdx < argc) {
            if (StrEq(argv[pathIdx], "--as") && pathIdx + 1 < argc) {
                argv0Override = argv[pathIdx + 1];
                pathIdx += 2;
            } else if (StrEq(argv[pathIdx], "--strace")) {
                strace = true;
                pathIdx++;
            } else {
                break;
            }
        }

        if (pathIdx >= argc)
        {
            KPrintf("usage: run [--as <name>] [--strace] <path> [args...]\n");
            return -1;
        }

        char resolved[128];
        if (!ResolveBinaryPath(argv[pathIdx], resolved, sizeof(resolved)))
        {
            KPrintf("shell: '%s' not found\n", argv[pathIdx]);
            return -1;
        }

        // Build the effective argv starting from the user's args.
        const char* effArgv[32];
        int effArgc = argc - pathIdx;
        if (effArgc > 32) effArgc = 32;
        for (int i = 0; i < effArgc; ++i) effArgv[i] = argv[pathIdx + i];
        effArgv[0] = resolved;

        // Shebang unwinding: up to 4 levels of script-invoking-script.
        static char shebangPath[128];
        static char interpStore[4][128];
        static char interpArgStore[4][128];
        StrCopy(shebangPath, resolved, sizeof(shebangPath));

        for (int depth = 0; depth < 4; ++depth)
        {
            char interp[128];
            char interpArg[128];
            if (!PeekShebang(shebangPath, interp, sizeof(interp),
                             interpArg, sizeof(interpArg)))
                break;

            if (!ResolveBinaryPath(interp, interpStore[depth],
                                   sizeof(interpStore[depth])))
            {
                KPrintf("shell: interpreter '%s' not found (for %s)\n",
                        interp, shebangPath);
                return -1;
            }
            StrCopy(interpArgStore[depth], interpArg,
                    sizeof(interpArgStore[depth]));

            const char* nArgv[32];
            int nArgc = 0;
            nArgv[nArgc++] = interpStore[depth];
            if (interpArgStore[depth][0] && nArgc < 31)
                nArgv[nArgc++] = interpArgStore[depth];
            if (nArgc < 31) nArgv[nArgc++] = shebangPath;
            for (int i = 1; i < effArgc && nArgc < 31; ++i)
                nArgv[nArgc++] = effArgv[i];

            for (int i = 0; i < nArgc; ++i) effArgv[i] = nArgv[i];
            effArgc = nArgc;
            StrCopy(shebangPath, interpStore[depth], sizeof(shebangPath));
        }

        Process* proc = nullptr;
        if (argv0Override)
        {
            const char* newArgv[32];
            newArgv[0] = argv0Override;
            for (int i = 1; i < effArgc && i < 31; ++i)
                newArgv[i] = effArgv[i];
            proc = SpawnProcess(shebangPath, effArgc, newArgv);
        }
        else
        {
            proc = SpawnProcess(shebangPath, effArgc, effArgv);
        }

        if (proc && strace) {
            proc->straceEnabled = true;
            KPrintf("[strace enabled for pid %u]\n", proc->pid);
        }
        return 0;
    }

    // Built-in: wait — block until all user processes finish
    if (StrEq(cmd, "wait"))
    {
        // In script mode, 'wait' is a no-op — the scheduler hasn't started yet.
        // Processes will begin running after the script finishes and
        // SchedulerStart() is called. In interactive mode (future), this could
        // block until all child processes have exited.
        if (g_scriptMode)
        {
            KPrintf("(wait deferred until scheduler starts)\n");
            return 0;
        }
        KPrintf("Waiting for processes...\n");
        while (SchedulerReadyCount() > 0)
        {
            __asm__ volatile("hlt");
        }
        KPrintf("All processes finished.\n");
        return 0;
    }

    // Built-in: shutdown / halt
    if (StrEq(cmd, "shutdown") || StrEq(cmd, "halt"))
    {
        KPrintf("Shutting down.\n");
        // QEMU shutdown via ACPI (port 0x604 for q35)
        __asm__ volatile("outw %0, %1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
        for (;;) __asm__ volatile("hlt");
    }

    // Built-in: reboot
    if (StrEq(cmd, "reboot"))
    {
        KPrintf("Rebooting.\n");
        // Triple fault: load null IDT and trigger interrupt
        struct { uint16_t limit; uint64_t base; } __attribute__((packed)) nullIdt = {0, 0};
        __asm__ volatile("lidt %0; int3" :: "m"(nullIdt));
        for (;;) __asm__ volatile("hlt");
    }

    // Built-in: panic [message] — deliberately trigger a kernel panic (for testing)
    if (StrEq(cmd, "panic"))
    {
        const char* msg = (argc >= 2) ? argv[1] : "deliberate test panic";
        KernelPanic("SHELL: %s\n", msg);
    }

    // Built-in: crashtest <mode> — trigger specific kernel crash types
    if (StrEq(cmd, "crashtest"))
    {
        const char* mode = (argc >= 2) ? argv[1] : nullptr;
        if (!mode || StrEq(mode, "help"))
        {
            SerialPuts("crashtest modes:\n"
                       "  panic       — KernelPanic() with message\n"
                       "  nullptr     — kernel NULL pointer dereference (#PF)\n"
                       "  divzero     — kernel divide by zero (#DE)\n"
                       "  gpf         — trigger #GP via invalid MSR write\n"
                       "  stackoverflow — kernel stack overflow via recursion\n"
                       "  ud          — undefined opcode (#UD)\n"
                       "  doublefault — corrupt RSP to trigger double fault\n");
            return 0;
        }
        if (StrEq(mode, "panic"))
        {
            KernelPanic("crashtest: deliberate kernel panic");
        }
        else if (StrEq(mode, "nullptr"))
        {
            // Write to an unmapped kernel-space address — guaranteed to fault.
            // 0xFFFFFF0000000000 is in the higher half but well outside our mappings.
            volatile int* p = reinterpret_cast<volatile int*>(0xFFFFFF0000000000ULL);
            *p = 42;
        }
        else if (StrEq(mode, "divzero"))
        {
            volatile int a = 1, b = 0;
            *(volatile int*)&a = a / b;
        }
        else if (StrEq(mode, "gpf"))
        {
            // Write to an invalid MSR — triggers #GP
            __asm__ volatile("wrmsr" :: "c"(0xDEAD), "a"(0), "d"(0));
        }
        else if (StrEq(mode, "stackoverflow"))
        {
            // Use a simple recursive function pointer to eat stack
            typedef void (*RecurseFn)(volatile int);
            static RecurseFn g_recurse;
            g_recurse = [](volatile int d) __attribute__((noinline)) {
                volatile char buf[4096];
                buf[0] = (char)d;
                g_recurse(d + 1);
            };
            g_recurse(0);
        }
        else if (StrEq(mode, "ud"))
        {
            __asm__ volatile("ud2");
        }
        else
        {
            SerialPuts("crashtest: unknown mode\n");
        }
        return 0;
    }

    // Built-in: log — dump kernel log
    if (StrEq(cmd, "log"))
    {
        KLogDump();
        return 0;
    }

    // Built-in: profile <duration_ms> — start sampling profiler
    if (StrEq(cmd, "profile"))
    {
        uint32_t ms = 0;
        if (argc >= 2) {
            // Simple atoi
            const char* s = argv[1];
            while (*s >= '0' && *s <= '9')
                ms = ms * 10 + (*s++ - '0');
        }
        if (ms == 0) ms = 10000;  // default 10 seconds
        ProfilerStart(ms);
        return 0;
    }

    // Built-in: heap [check|poison on|poison off] — heap diagnostics
    if (StrEq(cmd, "heap"))
    {
        if (argc >= 2 && StrEq(argv[1], "check"))
        {
            bool ok = HeapCheckIntegrity();
            KPrintf("Heap integrity: %s\n", ok ? "OK" : "CORRUPT");
        }
        else if (argc >= 3 && StrEq(argv[1], "poison"))
        {
            bool on = StrEq(argv[2], "on");
            HeapSetPoison(on);
            KPrintf("Heap poison: %s\n", on ? "enabled" : "disabled");
        }
        else
        {
            HeapDumpStats();
        }
        return 0;
    }

    // Built-in: modload <path> — load a kernel module
    if (StrEq(cmd, "modload") && argc >= 2)
    {
        ModuleHandle* h = ModuleLoad(argv[1]);
        if (h)
            KPrintf("Loaded module '%s' v%s\n", h->info->name, h->info->version);
        else
            KPrintf("Failed to load '%s'\n", argv[1]);
        return h ? 0 : -1;
    }

    // Built-in: modunload <name> — unload a kernel module
    if (StrEq(cmd, "modunload") && argc >= 2)
    {
        ModuleHandle* h = ModuleFind(argv[1]);
        if (h)
        {
            KPrintf("Unloading module '%s'\n", argv[1]);
            ModuleUnload(h);
        }
        else
        {
            KPrintf("Module '%s' not found\n", argv[1]);
        }
        return 0;
    }

    // Built-in: modlist — list loaded modules
    if (StrEq(cmd, "modlist") || StrEq(cmd, "lsmod"))
    {
        ModuleDump();
        return 0;
    }

    // Built-in: beep [freq] [ms] — play a sine wave tone via HDA audio
    if (StrEq(cmd, "beep"))
    {
        if (!AudioAvailable())
        {
            KPrintf("No audio driver registered\n");
            return -1;
        }

        uint32_t freq = 440;    // A4 by default
        uint32_t durationMs = 500;

        if (argc >= 2) {
            const char* s = argv[1]; freq = 0;
            while (*s >= '0' && *s <= '9') freq = freq * 10 + (*s++ - '0');
        }
        if (argc >= 3) {
            const char* s = argv[2]; durationMs = 0;
            while (*s >= '0' && *s <= '9') durationMs = durationMs * 10 + (*s++ - '0');
        }
        if (freq < 20) freq = 20;
        if (freq > 20000) freq = 20000;
        if (durationMs > 10000) durationMs = 10000;

        // Generate 16-bit mono PCM at 48000 Hz
        constexpr uint32_t sampleRate = 48000;
        uint32_t numSamples = (sampleRate * durationMs) / 1000;
        uint32_t bufSize = numSamples * 2; // 16-bit = 2 bytes per sample

        // Sine approximation lookup table (256 entries, Q15 format)
        // sin(i * 2π / 256) * 32767
        static const int16_t sinTable[256] = {
                 0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
              6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
             12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
             18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
             23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
             27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
             30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
             32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
             32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
             32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,
             30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,
             27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
             23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,
             18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,
             12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
              6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,
                 0,   -804,  -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
             -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
            -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
            -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
            -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
            -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
            -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
            -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
            -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
            -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
            -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
            -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
            -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
            -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
            -12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,
             -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,   -804,
        };

        // Cap buffer at 64KB (HDA buffer limit)
        if (bufSize > 65536) bufSize = 65536;
        numSamples = bufSize / 2;

        auto* buf = static_cast<int16_t*>(kmalloc(bufSize));
        if (!buf) { KPrintf("Out of memory\n"); return -1; }

        // Generate sine wave using fixed-point phase accumulator
        // phase is 24.8 fixed point into the 256-entry table
        uint32_t phaseInc = (freq * 256 * 256) / sampleRate;
        uint32_t phase = 0;
        for (uint32_t i = 0; i < numSamples; i++)
        {
            uint8_t idx = (phase >> 8) & 0xFF;
            buf[i] = sinTable[idx] / 2; // -16383..16383 (50% volume)
            phase += phaseInc;
        }

        KPrintf("Playing %u Hz for %u ms (%u samples)\n", freq, durationMs, numSamples);
        int ret = AudioPlay(buf, bufSize, sampleRate, 1, 16);
        if (ret < 0)
            KPrintf("AudioPlay failed: %d\n", ret);

        kfree(buf);
        return ret >= 0 ? 0 : -1;
    }

    // Built-in: sched [policy_name] — show or switch scheduler policy
    if (StrEq(cmd, "sched"))
    {
        if (argc < 2)
        {
            KPrintf("Scheduler policy: %s\n", SchedulerPolicyName());
            return 0;
        }
        if (SchedulerSwitchPolicy(argv[1]))
            KPrintf("Switched to '%s'\n", argv[1]);
        else
            KPrintf("Failed to switch to '%s'\n", argv[1]);
        return 0;
    }

    // Built-in: display [WxH] — show or change display mode
    if (StrEq(cmd, "display") || StrEq(cmd, "mode"))
    {
        if (argc < 2)
        {
            DisplayMode dm;
            DisplayGetMode(&dm);
            const DisplayOps* active = DisplayGetActive();
            KPrintf("Display: %s, %ux%u @ %ubpp, stride=%u\n",
                    active->name, dm.width, dm.height, dm.bpp, dm.stride);
            return 0;
        }
        // Parse WxH
        uint32_t w = 0, h = 0;
        const char* p = argv[1];
        while (*p >= '0' && *p <= '9') w = w * 10 + (*p++ - '0');
        if (*p == 'x' || *p == 'X') p++;
        while (*p >= '0' && *p <= '9') h = h * 10 + (*p++ - '0');
        if (w == 0 || h == 0) {
            KPrintf("Usage: display <width>x<height>  (e.g. display 1280x720)\n");
            return -1;
        }
        if (DisplaySetMode(w, h))
            KPrintf("Display mode set to %ux%u\n", w, h);
        else
            KPrintf("Failed to set display mode %ux%u\n", w, h);
        return 0;
    }

    // Built-in: strace <pid|name|all> [off] — enable/disable syscall tracing
    if (StrEq(cmd, "strace"))
    {
        if (argc < 2) {
            KPrintf("Usage: strace <pid|name|all> [off]\n");
            return 0;
        }
        bool enable = !(argc >= 3 && StrEq(argv[2], "off"));

        if (StrEq(argv[1], "all")) {
            StraceEnableAll(enable);
            KPrintf("strace %s for all processes\n", enable ? "ON" : "OFF");
        } else {
            // Try as pid first
            uint32_t pid = 0;
            const char* s = argv[1];
            while (*s >= '0' && *s <= '9')
                pid = pid * 10 + (*s++ - '0');
            if (*s == '\0' && pid > 0) {
                if (StraceEnablePid(pid, enable))
                    KPrintf("strace %s for pid %u\n", enable ? "ON" : "OFF", pid);
                else
                    KPrintf("pid %u not found\n", pid);
            } else {
                int count = StraceEnableName(argv[1], enable);
                KPrintf("strace %s for %d process(es) matching '%s'\n",
                        enable ? "ON" : "OFF", count, argv[1]);
            }
        }
        return 0;
    }

    // Try as a program name (implicit run)
    char resolved[128];
    if (ResolveBinaryPath(cmd, resolved, sizeof(resolved)))
    {
        SpawnProcess(resolved, argc, argv);
        return 0;
    }

    KPrintf("shell: unknown command '%s'\n", cmd);
    return -1;
}

// ---------------------------------------------------------------------------
// Built-in command implementations
// ---------------------------------------------------------------------------

static void CmdHelp()
{
    KPrintf("Brook shell commands:\n");
    KPrintf("  run [--as name] <prog> [args]  Launch a program\n");
    KPrintf("  <prog> [args]      Same as 'run <prog>'\n");
    KPrintf("  wait               Wait for all processes to exit\n");
    KPrintf("  ps                 List running processes\n");
    KPrintf("  mem                Show memory usage\n");
    KPrintf("  ls [path]          List directory contents\n");
    KPrintf("  set grid <CxR>     Set process grid layout\n");
    KPrintf("  set scale <N>      Set compositor downscale\n");
    KPrintf("  set vfb <WxH>      Set VFB dimensions (or 'auto'/'full')\n");
    KPrintf("  set pos <X,Y>      Set next window position\n");
    KPrintf("  clear              Clear screen\n");
    KPrintf("  shutdown           Power off\n");
    KPrintf("  reboot             Reboot\n");
    KPrintf("  panic [msg]        Trigger test panic\n");
    KPrintf("  log                Dump kernel log\n");
    KPrintf("  modload <path>     Load a kernel module\n");
    KPrintf("  modunload <name>   Unload a kernel module\n");
    KPrintf("  modlist            List loaded modules\n");
    KPrintf("  sched [policy]     Show/switch scheduler policy\n");
    KPrintf("  display [WxH]      Show/change display mode\n");
    KPrintf("  strace <pid|name|all> [off]  Syscall tracing\n");
    KPrintf("  help               Show this help\n");
}

static void CmdPs()
{
    // Query scheduler for process info
    uint32_t ready = SchedulerReadyCount();
    KPrintf("Processes: %u in run queue\n", ready);

    // We don't have direct access to the process list from here,
    // but we can report the spawn count and ready count.
    KPrintf("Total spawned: %u\n", g_spawnCount);
}

static void CmdMem()
{
    uint64_t freePages = PmmGetFreePageCount();
    uint64_t totalPages = PmmGetTotalPageCount();
    uint64_t freeMB = (freePages * 4096) / (1024 * 1024);
    uint64_t totalMB = (totalPages * 4096) / (1024 * 1024);
    uint64_t usedMB = totalMB - freeMB;

    KPrintf("Memory: %lu MB used / %lu MB total (%lu MB free)\n",
            usedMB, totalMB, freeMB);
    KPrintf("Pages:  %lu used / %lu total\n",
            totalPages - freePages, totalPages);
}

static void CmdLs(int argc, const char* const* argv)
{
    const char* path = (argc >= 2) ? argv[1] : "/boot";

    Vnode* dir = VfsOpen(path, 0);
    if (!dir)
    {
        KPrintf("ls: cannot open '%s'\n", path);
        return;
    }

    KPrintf("%s:\n", path);

    DirEntry entry;
    uint32_t cookie = 0;
    while (VfsReaddir(dir, &entry, &cookie) == 1)
    {
        if (entry.isDir)
            KPrintf("  %s/\n", entry.name);
        else
            KPrintf("  %s  (%lu bytes)\n", entry.name, entry.size);
    }

    VfsClose(dir);
}

// ---------------------------------------------------------------------------
// Read a line from VFS file (for script execution)
// ---------------------------------------------------------------------------

static int ReadScriptLine(Vnode* vn, uint64_t* offset, char* buf, uint32_t maxLen)
{
    uint32_t i = 0;
    while (i < maxLen - 1)
    {
        char c;
        int ret = VfsRead(vn, &c, 1, offset);
        if (ret <= 0)
        {
            if (i == 0) return -1; // EOF with no data
            break;
        }
        if (c == '\n') break;
        if (c == '\r') continue;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return static_cast<int>(i);
}

// ---------------------------------------------------------------------------
// Read a line from keyboard (for interactive mode)
// ---------------------------------------------------------------------------

static int ReadInteractiveLine(char* buf, uint32_t maxLen)
{
    uint32_t i = 0;

    while (i < maxLen - 1)
    {
        char c = KbdGetChar();

        // Enter
        if (c == '\n' || c == '\r')
        {
            TtyPutChar('\n');
            break;
        }

        // Backspace
        if (c == '\b' || c == 127)
        {
            if (i > 0)
            {
                --i;
                // Erase character on screen: backspace, space, backspace
                TtyPutChar('\b');
                TtyPutChar(' ');
                TtyPutChar('\b');
            }
            continue;
        }

        // Printable characters only
        if (c >= 32 && c < 127)
        {
            buf[i++] = c;
            TtyPutChar(c);
        }
    }

    buf[i] = '\0';
    return static_cast<int>(i);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int ShellExecScript(const char* path)
{
    Vnode* vn = VfsOpen(path, 0);
    if (!vn)
    {
        SerialPrintf("SHELL: script '%s' not found\n", path);
        return -1;
    }

    KPrintf("Executing %s\n", path);
    SerialPrintf("SHELL: executing script '%s'\n", path);
    g_scriptMode = true;

    char line[MAX_LINE];
    uint64_t offset = 0;
    [[maybe_unused]] uint32_t lineNum = 0;

    while (ReadScriptLine(vn, &offset, line, MAX_LINE) >= 0)
    {
        ++lineNum;

        // Skip empty lines and comments
        const char* p = line;
        while (IsWhitespace(*p)) ++p;
        if (*p == '\0' || *p == '#') continue;

        DbgPrintf("SHELL: [%u] %s\n", lineNum, line);

        const char* argv[MAX_ARGS];
        int argc = ParseLine(line, argv, MAX_ARGS);
        if (argc > 0)
        {
            ExecCommand(argc, argv);
        }
    }

    VfsClose(vn);
    g_scriptMode = false;

    SerialPrintf("SHELL: script '%s' finished\n", path);
    return 0;
}

[[noreturn]] void ShellInteractive()
{
    KPrintf("\nBrook OS shell — type 'help' for commands\n");

    char line[MAX_LINE];

    for (;;)
    {
        // Print prompt
        TtyPrintf("brook> ");

        int len = ReadInteractiveLine(line, MAX_LINE);
        if (len <= 0) continue;

        const char* argv[MAX_ARGS];
        int argc = ParseLine(line, argv, MAX_ARGS);
        if (argc > 0)
        {
            ExecCommand(argc, argv);
        }
    }
}

} // namespace brook
