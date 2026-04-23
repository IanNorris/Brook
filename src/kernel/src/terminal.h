#pragma once

#include <stdint.h>

namespace brook {

struct Process;

// Maximum number of terminal windows
static constexpr uint32_t MAX_TERMINALS = 8;

// Default scrollback capacity (rows of history).  Per-terminal cost scales
// as cols * scrollbackRows * sizeof(TermCell); ≈1MB for an 80-col terminal
// with 1000 rows of history.
static constexpr uint32_t TERM_SCROLLBACK_ROWS = 1000;

// One character cell in the logical grid.  Carries enough attribute data
// to redraw a cell without re-parsing history.
struct TermCell
{
    char     ch;       // 0 = blank
    uint8_t  attr;     // reserved (bold/inverse later)
    uint16_t pad;
    uint32_t fg;       // XRGB
    uint32_t bg;       // XRGB
};

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

    // Alternate screen buffer (DEC private mode 1049)
    uint32_t* altVfb;         // saved main screen when in alt mode
    uint32_t  savedCurX, savedCurY;
    bool      inAltScreen;

    // Character-grid mirror of the visible screen, sized cols*rows.
    // Written alongside pixel rendering so scrollback redraw works
    // without maintaining a pixel-space history.
    TermCell* cells;

    // Ring buffer of scrolled-off rows.  Row n is at
    // scrollback[((scrollbackHead - scrollbackUsed + n) mod scrollbackRows) * cols].
    TermCell* scrollback;
    uint32_t  scrollbackRows;
    uint32_t  scrollbackHead;
    uint32_t  scrollbackUsed;

    // Scroll viewport offset in rows.  0 = live.  While > 0, new output
    // updates the cell grid / scrollback but not the displayed VFB;
    // keypress or downward scroll snaps back to 0.
    uint32_t  viewOffset;
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

// Shut down a terminal: kill child, stop thread, clean up.
void TerminalClose(Terminal* t);

// Resize a terminal's VFB and update character dimensions.
// Sends SIGWINCH to the child process.
void TerminalResize(Terminal* t, uint32_t newW, uint32_t newH);

// Scroll the terminal viewport by dy lines.  Positive dy scrolls UP
// (older content into view), negative scrolls down toward live output.
// Currently a no-op placeholder that logs — scrollback storage lands
// in a follow-up.
void TerminalScroll(Terminal* t, int32_t dy);

} // namespace brook
