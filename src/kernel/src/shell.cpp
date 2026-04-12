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
#include "tty.h"
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

// Track spawned process count for auto-tiling placement
static uint32_t g_spawnCount = 0;

// True while executing a boot script (before scheduler starts)
static bool g_scriptMode = false;

// Default environment for spawned processes
static const char* g_defaultEnvp[] = { "HOME=/", nullptr };

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

    constexpr uint64_t MAX_ELF_SIZE = 2 * 1024 * 1024;
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
    constexpr uint64_t MAX_ELF_SIZE = 2 * 1024 * 1024;
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

    Process* proc = ProcessCreate(elfBuf, elfSize, argc, argv,
                                   1, g_defaultEnvp);

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
    int16_t destX, destY;
    uint32_t cols, rows;
    ComputeGridPosition(g_spawnCount, &destX, &destY, &cols, &rows);

    CompositorSetupProcess(proc, destX, destY, g_vfbWidth, g_vfbHeight, g_scale);
    SchedulerAddProcess(proc);

    DbgPrintf("SHELL: spawned '%s' pid=%u at (%d,%d) grid=%ux%u scale=%u\n",
                  proc->name, proc->pid, destX, destY, cols, rows, g_scale);
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

    // Built-in: set <key> <value>
    if (StrEq(cmd, "set") && argc >= 3)
    {
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
        else
        {
            KPrintf("set: unknown key '%s'\n", argv[1]);
        }
        return 0;
    }

    // Built-in: run <path> [args...] — explicit run command
    if (StrEq(cmd, "run") && argc >= 2)
    {
        char resolved[128];
        if (!ResolveBinaryPath(argv[1], resolved, sizeof(resolved)))
        {
            KPrintf("shell: '%s' not found\n", argv[1]);
            return -1;
        }
        SpawnProcess(resolved, argc - 1, argv + 1);
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

    // Built-in: log — dump kernel log
    if (StrEq(cmd, "log"))
    {
        KLogDump();
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
    KPrintf("  run <prog> [args]  Launch a program\n");
    KPrintf("  <prog> [args]      Same as 'run <prog>'\n");
    KPrintf("  wait               Wait for all processes to exit\n");
    KPrintf("  ps                 List running processes\n");
    KPrintf("  mem                Show memory usage\n");
    KPrintf("  ls [path]          List directory contents\n");
    KPrintf("  set grid <CxR>     Set process grid layout\n");
    KPrintf("  set scale <N>      Set compositor downscale\n");
    KPrintf("  set vfb <WxH>      Set VFB dimensions\n");
    KPrintf("  clear              Clear screen\n");
    KPrintf("  shutdown           Power off\n");
    KPrintf("  reboot             Reboot\n");
    KPrintf("  panic [msg]        Trigger test panic\n");
    KPrintf("  log                Dump kernel log\n");
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
    uint32_t lineNum = 0;

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
