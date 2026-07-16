# SDL2 input probes — BRO-216 (yquake2 console text dead)

Two probes to pin why yquake2's **console text** is dead while its **scancode
bindings/menu** work, under Brook's waylandd.

## What we already know (code-verified)

- yquake2 calls `SDL_StartTextInput()` unconditionally at init
  (`src/client/input/sdl2.c:2922`) and emits console text from `SDL_TEXTINPUT`
  (`Char_Event`). So text input is **not** "never started".
- yquake2 links **SDL2** (`libSDL2-2.0.so.0`), not SDL3.
- SDL2's Wayland backend (`SDL_waylandevents.c`, `keyboard_handle_key`) sends
  text via `SDL_SendKeyboardText` **only when `keyboard_input_get_text()`
  succeeds**, which needs `input->xkb.state` to be a valid xkb state built in
  `keyboard_handle_keymap` from the `wl_keyboard.keymap` memfd. **Scancodes come
  from a fixed evdev→SDL_Scancode table and need no xkb.**
- Therefore: bindings work + console text dead ⇒ **SDL2's `xkb.state` is
  NULL/invalid** (the keymap event was not processed, or the keymap
  mmap/compile failed on Brook).

## Build

```
nix-build tools/sdl2-input-probe --no-out-link
```

Produces `bin/kbdprobe` (GUI) and `bin/xkb_memfd_probe` (headless).

## 1. `xkb_memfd_probe` — headless, run FIRST (most tractable)

Replicates SDL2's exact suspect path in one process:
`mmap(NULL, size, PROT_READ, MAP_PRIVATE, memfd)` → `xkb_keymap_new_from_string`.
Runnable from the working (musl) terminal — no GUI, no input injection.

```
<out>/bin/xkb_memfd_probe
```

Decision tree:

| Result | Meaning | Next |
|--------|---------|------|
| `FAIL: mmap(MAP_PRIVATE, memfd)` | Brook `MAP_PRIVATE` memfd mmap broken | kernel `sys_mmap` memfd path (treats all memfd maps as shared/lazy; add MAP_PRIVATE handling) |
| `FAIL: xkb_keymap_new_from_string` / `bytes_match=0` | mmap returns wrong/zero bytes | same kernel path — mapped data ≠ written data |
| `PASS` | mmap+compile fine | the SDL2 failure is the **SCM_RIGHTS keymap-fd hand-off** (waylandd→client) or SDL2 not processing the `wl_keyboard.keymap` event → run probe 2 |

Linux baseline: **PASS** (`key38 utf8='a'`), confirming the logic + keymap are valid.

## 2. `kbdprobe` — GUI, run under waylandd

```
SDL_VIDEODRIVER=wayland <out>/bin/kbdprobe
```

Press `q w e r t y`, `a s d f`, etc. Reads keypresses and logs to stderr/serial.
Esc quits.

| Observation | Meaning |
|-------------|---------|
| `KEYDOWN scancode=… sym=0/UNKNOWN`, **no** `TEXTINPUT` | xkb.state NULL/invalid → same root as probe 1's FAIL |
| `KEYDOWN … sym=a(valid)` + `TEXTINPUT 'a'` | xkb works — bug is above SDL (yquake2-specific; unlikely given code) |

## Root candidate (leading)

Brook's `sys_mmap` memfd branch (`syscall.cpp:~3718`) is commented "MAP_SHARED —
used by wl_shm" and lazily faults pages to the mfd's own backing regardless of
`MAP_PRIVATE`. For a read-only mapping this is functionally OK (reads see the
written bytes), so it may PASS — in which case the fault is the cross-process
keymap-fd hand-off (SCM_RIGHTS) delivering an fd whose mmap doesn't reflect
waylandd's written keymap. Probe 1 distinguishes these in one run.
