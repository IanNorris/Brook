# Terminal, TTY & Audio Subsystem Documentation

## Overview

Brook's terminal subsystem provides VT100/ANSI terminal emulation with
scrollback, a legacy TTY framebuffer layer, a built-in shell, and a
basic PCM audio mixer.

## Files

| File | Lines | Purpose |
|------|-------|---------|
| terminal.cpp/h | ~1500 | VT100 terminal emulator: cells, scrollback, CSI parsing |
| tty.cpp/h | ~350 | Legacy TTY: direct framebuffer text output (boot/panic) |
| shell.cpp/h | ~400 | Built-in kernel shell: command parsing, builtins |
| audio.cpp/h | ~300 | PCM audio mixer: stream management, /dev/dsp interface |

## Architecture

```
User process writes to /dev/ttyN or /dev/pts/N
  → Terminal CSI/VT100 parser
    → Cell grid update (curX, curY, attributes)
    → Glyph rendering to per-terminal VFB
    → Compositor blit to screen

Legacy TTY (boot/early):
  kprintf → TtyPutChar → direct framebuffer pixel writes

Audio:
  User writes PCM to /dev/dsp
    → AudioPlay() → mixer stream queue
    → HDA driver drains mixed samples to hardware ring buffer
```

### Key Mechanisms

- **Terminal cells**: Grid of `TermCell` (codepoint + fg/bg/attr). Backed by
  scrollback ring buffer for history.
- **CSI parser**: Handles cursor movement (CUP/CUU/CUD/CUF/CUB), erase
  (ED/EL), SGR (color/bold/underline), scroll regions, and alternate screen.
- **VFB rendering**: Each terminal has a kernel-allocated VFB; glyphs rendered
  via font atlas. User process sees the VFB through memory-mapped pages.
- **Shell**: Simple command parser with builtins (cd, ls, cat, echo, etc.).
  Forks child processes for external commands.
- **Audio mixer**: Mixes multiple PCM streams into a single output buffer.
  HDA driver pulls mixed samples. Stream tracking via fixed-size array.

## Audit Findings (2026-05-10)

### Known Issues
- **BRO-125**: CSI parameter parsing doesn't clamp values; large cursor
  positions cause OOB pixel writes to VFB
- **BRO-126**: Audio mixer globals accessed without synchronization from
  multiple threads
- Terminal array `g_terminals[]` accessed without lock during concurrent
  create/destroy
- `TerminalClose()` can race with rendering thread — potential double-free
  of VFB pages
- TTY region calculations can underflow if region exceeds framebuffer
- Scrollback ring `scrollbackRows` used as modulo divisor without zero check
- Shell argument parsing has fixed buffer sizes without overflow protection

### Overall Assessment
**Functional but fragile under concurrency.** The VT100 emulation covers
the common ANSI sequences well enough for bash, btop, and nano. Main risks
are the unclamped CSI parameters (exploitable from userspace) and the
unprotected terminal/audio globals on SMP. The TTY layer is legacy and
only used during early boot — low risk.
