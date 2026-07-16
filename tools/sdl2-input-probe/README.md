# SDL2 input probes — BRO-216 (yquake2 console text dead)

Probes to pin why yquake2's **console text** is dead while its **scancode
bindings/menu** work, under Brook's waylandd.

## What we now know (code-verified)

- **The `libSDL2-2.0.so.0` yquake2 links is `sdl2-compat`** — a shim implementing
  the SDL2 ABI on top of **SDL3** (`DT_NEEDED libSDL3.so.0`, e.g.
  `sdl2-compat-2.32.70` → `sdl3-3.4.10`). So yquake2's real keyboard/text
  handling runs through **SDL3's** Wayland backend, not SDL2's. This reconciles
  the earlier SDL2-vs-SDL3 confusion.
- yquake2 calls `SDL_StartTextInput()` unconditionally at init
  (`src/client/input/sdl2.c:2922`), which sdl2-compat maps to SDL3.
- SDL3's `keyboard_handle_key` (`SDL_waylandevents.c`) emits console text via
  `SDL_SendKeyboardText` **only when `keyboard_input_get_text()` succeeds**,
  which requires `seat->keyboard.focus` **and** `seat->keyboard.xkb.state`
  (`SDL_waylandevents.c:2192`), the xkb state built in `keyboard_handle_keymap`
  from the `wl_keyboard.keymap` memfd.
- **Scancodes need no xkb**: when `is_virtual == false` they come from a fixed
  table `SDL_GetScancodeFromTable(SDL_SCANCODE_TABLE_XFREE86_2, key)`
  (`SDL_waylandevents.c:1851`). `is_virtual` stays `false` if the keymap fails
  to compile (early return at :1704 leaves it at its zero-init value).
- **SDL3 hard-links `libxkbcommon.so.0`** (DT_NEEDED) — it does **not** dlopen
  it — so the "dlopen fails on Brook" theory is ruled out; SDL3 loads xkbcommon
  exactly like GTK does.
- Therefore: bindings work + console text dead ⇒ **SDL3's `xkb.state` is
  NULL/invalid**: either the keymap event never arrived, the SCM_RIGHTS fd was
  bad, the `MAP_PRIVATE` memfd mmap returned wrong bytes, or the real keymap
  string failed to compile. GTK/mousepad text **works** on Brook via the same
  keymap-fd mechanism, so the bytes and libxkbcommon are fine — which points at
  something SDL3 does differently at the seat/keyboard/keymap wiring.

## Build

```
nix-build tools/sdl2-input-probe --no-out-link
```

Produces `bin/wl_keymap_probe` (decisive, headless), `bin/xkb_memfd_probe`
(headless), and `bin/kbdprobe` (GUI).

## 0. `wl_keymap_probe` — DECISIVE, headless, run FIRST

A minimal raw-libwayland client (no SDL, no GL, no game) that connects to
waylandd, binds `wl_seat`, creates a `wl_keyboard`, receives the **real** keymap
event (real fd via SCM_RIGHTS) and runs SDL3's **exact** path from
`keyboard_handle_keymap` + `keyboard_input_get_text`:

```
map    = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
keymap = xkb_keymap_new_from_string(ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
state  = xkb_state_new(keymap);
// then per test key: xkb_state_key_get_syms()==1 && xkb_keysym_to_utf8()>0
```

Unlike `xkb_memfd_probe` (which fabricates its own keymap + memfd), this uses
the **actual** keymap waylandd ships and the **actual** cross-process fd, so it
exercises the whole kernel + waylandd + xkb pipeline end to end.

```
WAYLAND_DISPLAY=<sock> <out>/bin/wl_keymap_probe
```

Decision tree:

| Result | Meaning | Next |
|--------|---------|------|
| `no wl_seat` / `no keymap event` | waylandd didn't advertise keyboard or didn't send keymap | waylandd `seat_get_keyboard` / `make_keymap_fd` path |
| `FAIL: KEYMAP fd=<0` | SCM_RIGHTS hand-off delivered a bad fd | kernel `sys_recvmsg` / `UnixFdSnap` install |
| `FAIL: mmap(...MAP_PRIVATE...)` | Brook memfd `MAP_PRIVATE` mmap broken | kernel `sys_mmap` memfd branch |
| `bytes_match` off / no NUL | mmap returns wrong/zero bytes | kernel memfd fault path |
| `FAIL: xkb_keymap_new_from_string` | real keymap string doesn't compile on Brook | waylandd keymap text |
| `nsyms!=1` / `utf8<=0` | compiled but text extraction fails (the yq2 gate) | keymap symbol tables |
| `PASS` | whole kernel+waylandd+xkb path clean | **BRO-216 is inside SDL3/sdl2-compat** (focus surface, `SDL_TextInputActive`, or seat wiring) — move the investigation into SDL3 |

**Linux baseline: PASS** — validated against a real KWin/Wayland session; the
probe received a 69978-byte keymap fd, mmap'd it `MAP_PRIVATE`, compiled it
(`English (UK)`, `is_virtual=0`), and produced text for `a s d 1 space z`.

## 1. `xkb_memfd_probe` — headless, fabricated keymap


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
