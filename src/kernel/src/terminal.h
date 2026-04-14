#pragma once

#include <stdint.h>

namespace brook {

struct Process;

// Maximum number of terminal windows
static constexpr uint32_t MAX_TERMINALS = 8;

struct Terminal
{
    Process*  thread;          // kernel thread running this terminal
    Process*  child;           // bash process
    uint32_t* vfb;             // virtual framebuffer (owned)
    uint32_t  vfbW, vfbH;     // VFB dimensions
    uint32_t  curX, curY;     // cursor position (in glyph cells)
    uint32_t  cols, rows;      // terminal size in characters
    uint32_t  fgColor;         // current foreground (XRGB)
    uint32_t  bgColor;         // current background (XRGB)
    void*     stdinPipe;       // PipeBuffer* — terminal writes here, bash reads
    void*     stdoutPipe;      // PipeBuffer* — bash writes here, terminal reads
    uint16_t  foregroundPgid;  // foreground process group (for Ctrl+C)
    bool      active;
    bool      dirty;           // VFB needs compositor blit
};

// Create a new terminal window and spawn bash in it.
// Returns the terminal index or -1 on failure.
int TerminalCreate(uint32_t clientW, uint32_t clientH);

// Write raw keyboard input to the focused terminal's stdin pipe.
void TerminalWriteInput(int termIdx, const char* data, uint32_t len);

// Get terminal by index (for compositor integration).
Terminal* TerminalGet(int idx);

// Get the terminal associated with a kernel thread process.
Terminal* TerminalGetByThread(Process* proc);

// Get the terminal whose child (bash) is the given process or whose
// child is an ancestor of the given process.
Terminal* TerminalFindByProcess(Process* proc);

} // namespace brook
