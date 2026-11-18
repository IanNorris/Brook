# Individual Xwayland windows

Brook now has an opt-in owned rootless Xwayland path. Native controls show two
independent xeyes windows, with no enclosing X desktop. Authenticated Steam
Store and Library render in a native window, and Friends opens and moves as a
separate window. Longer Steam acceptance, broader transient policy and
shaped-window rendering remain open.

## GPU presentation of X11 windows (18 Nov 2026, `work/xwl-dmabuf`)

**Cause of `src cpu` for every X11 window (Dell gen 423, Torchlight II ~10
displayed fps, Stardew).** Xwayland already hands waylandd GPU buffers:
glamor's window buffers (and Present flips of a GL client's DRI3 pixmap) arrive
as `zwp_linux_dmabuf_v1` buffers, LINEAR, because waylandd's v4 feedback
offers only LINEAR XR24/AR24 on its one render node. The Dell log shows
`dmabuf buffer 1280x800 ... modifier=0x0`; nothing falls back to `wl_shm`, and
modifiers are not the problem. waylandd's GPU offer path
(`gpu_window_offer`, `gpu_window_offer_layers`) required the root to be an
acknowledged `xdg_toplevel`. Xwayland surfaces have role 4
(`xwayland_surface_v1`) and no xdg objects, so every X11 commit was counted
as `offer_skipped.no_root`. The Intel queue then waited for the frame, read the
whole buffer back (`TRY_CPU_READ` plus mmap copy) and composed it into the
VFB, and the renderer received a CPU source.

**Fix.** `gpu_offer_root_ready()` also accepts a role 4 root that
`xwayland_surface_ready()` reports paired with its mapped X window. A buffer
that does not match a pending native resize (`xwayland_accept_view`) still
waits, as on the CPU path (`offer_skipped.xwayland_resize_wait`). Counters:
`offers.xwayland` in `$XDG_RUNTIME_DIR` (or `/tmp`) `/brook-waylandd-wayland-0.json`. Kill switch:
`BROOK_WAYLAND_GPU_XWAYLAND=0`. Host test: `check_gpu_window_xwayland` in
`capture-release-test.c` (direct variant): offer and release, resize wait, and
an unpaired surface staying on the CPU.

**The 9% memset was not GPU buffer zero-fill.** Symbolising the gen 423
profile against the int22 `kernel.elf` puts it inside `epoll_wait_impl`.
`EpollWalkOwner` cleared the whole roughly 78 KiB `EpollWalk`, and
`clearMemo()` cleared about 24 KiB more, on every `epoll_wait` iteration. One
process (pid 0x1d5) spent about a CPU in `epoll_wait` (870 of its 963
samples). Now only `pins`, `attempted` and `masks` (13 KiB) are zeroed, and
memo nodes are initialised on allocation. That process is still busy-polling;
it is unidentified.

**Not changed.** X11 buffers stay LINEAR. Y- or X-tiled imports would need a
modifier field in the GPU window channel, plus kernel shim and VKRENDER
support. Whether Present flips or copies into the window pixmap depends on
Xwayland's `xwl_present_check_flip`. Either way the result is a LINEAR
dma-buf, which is now offered.

## Xwayland pacing, kernel lock convoys and tiled buffers (18 Nov 2026, `work/xwl-pacing`)

**Gen 427 result for the GPU-offer fix.** Torchlight II (800×600) is now
`src gpu` with all frames direct, but only 12.9 displayed fps (mean 77.5 ms).
Xwayland commits about 13.6 times a second (300 offers in 22 s). The GPU is
75% idle and the CPU 76% idle, so the game is waiting on latency. Evidence:
`display-perf/build/dell/tl2-int24/`.

**Where the time goes.** I matched the context-switch records (1 ms ticks)
in `prof.txt` against the int24 `kernel.elf`. The analysis scripts are in this
branch's report.
- **The global epoll graph mutex convoys.** One game-process thread
  (pid 748) calls `epoll_wait` about 1,700 times a second with events ready.
  Inside `EpollScanReady`, while holding `g_epollGraphLock`, it blocked 6,589
  times on its own process's address-space mutation lock (`CopyToUser`,
  then `UserMappingPageAccess`).
  - Every other epoll user in the system queued behind it.
  - waylandd (pid 24) spent about 5.9 s of 10 s blocked on the graph mutex and
    only 5 s idle-waiting.
  - Other epoll users (pids 30, 520, 575, 450) were blocked 5–9 s.
- **The per-process mapping mutex convoys.** `UserMappingPageAccess` takes
  `space->mutation` for every page of every user copy. The game's threads
  were blocked on it for seconds:
  - pid 470, the thread doing `IntelRenderIoctl`: 6.2 s of 10 s;
  - pid 687: 5.5 s;
  - pid 689: 2.5 s.
  This is not fixed here. It needs a lock-free read-only PTE check or a
  reader lock, in the same spirit as `work/kregion-lockfree`.
- **Present pacing.** Rootless Xwayland completes a sync flip at the next
  `wl_surface.frame` done after its commit. Mesa GLX with swap interval 1
  keeps only one swap outstanding (`loader_dri3_swap_buffers_msc` waits for
  `recv_sbc + 1 == send_sbc`). waylandd released callbacks only on
  PRESENTED, which comes after the renderer's composite (14.4 ms
  submit→fence, queued behind the game's 10 ms GPU jobs) and the flip at
  vblank. That serialised each frame with the whole renderer latency, about
  25–35 ms (`timeline` in `s1.json`), plus the convoy delays.

**Changes.**
- **waylandd:** an Xwayland root's frame callbacks are released once its
  commit is offered to the renderer, at most every 15 ms. Buffer release
  (SAFE after a newer commit is shown) still bounds how far the client runs
  ahead. `BROOK_WAYLAND_XWAYLAND_FRAME=presented` restores the old behaviour.
  The status file has a `pacing` block, always on, for native and x11
  clients. Each entry is `[count, sum_us, max_us]`:
  - `interval`: commit to the next commit;
  - `offer`: commit to offer sent;
  - `presented`: offer to PRESENTED;
  - `callback`: commit to frame callback done;
  - `turnaround`: frame callback done to the client's next commit;
  - also `dmabuf_created`, `early_callbacks` and `tiled_created`.
- **epoll:** events are copied out without the graph mutex. The mutex is
  retaken only to commit edge-triggered eventfd and sync-file snapshots,
  skipped if `epoll_ctl` changed the instance in between.
- **Tiled buffers:** waylandd advertises I915 Y- and X-tiled XR24/AR24 to the
  Xwayland client when the Intel GPU channel is present
  (`BROOK_WAYLAND_TILED=xwayland|all|0`).
  - Tiled buffers are offered as version 3 layers flagged `TILED_X` or
    `TILED_Y`.
  - The renderer's quad pass imports them through
    `VK_EXT_image_drm_format_modifier` (`BROOK_RENDER_TILED=0` disables
    this).
  - A frame that the quad pass does not draw retires the offer, so the window
    uses waylandd's detiled CPU copy instead.
  - Host results, UHD 630 (Gen9.5), 8 blended quads:

    | Size | LINEAR | X-tiled | Y-tiled |
    |---|---|---|---|
    | 3192×1632 | 48.6 ms | 15.3 ms | 14.5 ms |
    | 800×600 | 4.8 ms | 2.0 ms | 2.0 ms |

  - Correctness on the host Intel GPU: the quad pass draws X and Y sources,
    1:1 and scaled, exactly like the linear source. The tiling arithmetic
    matches Mesa's own tiled output (`intel-tiling-host-test.c`).
  - The Dell's renderer uses hasvk (Gen8), which advertises the extension.
    It has not been tested here.

**Gen 428 GPU job failure (`stardew-hang428`).** This is not caused by the
X11 offer path.
- pid 223 is a Chromium child, almost certainly its GPU process: it probed
  `/sys/bus/pci/devices/0000:00:02.0/boot_vga`. Chromium is a native Wayland
  (xdg) client.
- The failed source is window 1 incarnation 1, the first window, created
  before Steam. Its last good commit was at about 289 s.
- Stardew (pid 333) logged its first syscalls only after the reset, so its
  process started after it. Window 2 incarnation 8 (1280×720 XRGB, the
  usual Stardew size) then displayed 42.9 fps in `s0`→`s1`.
- The same signature happened on 16 Nov to VKRENDER itself, before any
  Xwayland offers existed (`hang-1116-lastlog`, pid 52): ring
  head = tail = acthd = 0xe0, `elsp=302`, `ipehr=0x4000001`, no CSB events.
- `DESKTOP_SCANOUT_STARVED` was logged twice; `scanout.starved` stayed at 2
  from `s0` to `s1`. It was a transient effect of the reset.

## Native geometry API

`WM_WINDOW_STATE` (`0xB08`) snapshots one calling provider's window without
mapping pixels or exposing foreign windows. Its shared 40-byte ABI is
`src/shared/wm_window_state.h`. It reports client coordinates, extent,
focus/visibility flags and creation incarnation. Zero discovers the identity;
subsequent queries can require it. Missing/foreign windows return `ENOENT`;
owned recycled slots return `ESTALE`.

`WM_PLACE_WINDOW` (`0xB09`) accepts the owned window's nonzero incarnation and
signed client coordinates, preserving size and focus. Both operations serialize
window access under the native mutex. Query output is prefaulted before that
mutex and copied after releasing it. Provider buffer sizes now preserve their
exact requested extents; interactive desktop resize minima remain separate.

The production window lifecycle host fixture passes ownership, incarnation,
placement, scaled decoration and signed-coordinate controls. The release kernel
builds; entry-state audits pass for the supported templates, together with
scheduler-start and lock/wait mutation checks. This is not universal handler
coverage. Frozen kernel SHA-256:
`f1092252021847281bf229169b2f9f2e99bb4a937c69c05c7a5c1eeb704367cd`.
`scripts/workloads/window_state_check.py --brook-guest` passes 23 native checks,
including inaccessible output, foreign ownership, placement and a 32x24 buffer.

## Owned server and private XWM

Launch with:

```sh
waylandd --xwayland /nix/profile/bin/Xwayland --xwm /nix/bin/brook-xwm --xdisplay :0
```

The launcher creates private Wayland, XWM, control and display-readiness socket
pairs. Only the exact compositor-created `wl_client` can see or bind
`xwayland-shell-v1`; UID and client-supplied names do not grant access. The shell
associates committed 64-bit serials with `WL_SURFACE_SERIAL` events in either
arrival order, scoped to a server generation. Destroying the short-lived shell
object preserves pending and committed surface association state.

`brook-xwm` accepts inherited descriptors, never ambient `DISPLAY`. XCB startup
and synchronous replies run in this worker rather than Wayland dispatch.
Composite manual redirection is required for real rootless Xwayland surfaces.
Normal X windows use Brook chrome; override-redirect windows use client pixels
without taking native keyboard focus. Root, support and InputOnly windows are
excluded from native presentation; InputOnly X map requests are still honored.
X borders are forced to zero to keep backing pixmaps and client extents aligned.

Follow-up controls honor an explicit `_MOTIF_WM_HINTS` decorations=0 request,
including a mapped-window change, while missing/malformed hints retain Brook
chrome. Client chrome remains focusable. The bridge forwards pointer-driven
`_NET_WM_MOVERESIZE`, maximization requests and `WM_CHANGE_STATE` iconification
to the existing native window operations. Maximize/minimize state flows back
through X properties. This implements full-window maximization rather than
independent horizontal/vertical modes; fullscreen is not implemented here.

The fixed-message stream has bounded queues, framing for partial/coalesced
reads, and fail-closed generation retirement on EOF, malformed messages or queue
exhaustion. XID incarnations reject stale commands. Transient parent references
carry resolved identities. Native geometry commands carry serials so an older
X acknowledgement cannot undo a newer native drag/resize. Old-size frames wait
for matching extents, and NULL-buffer unmapping clears native incarnation state.

Focus uses input hints and `WM_TAKE_FOCUS` with an asynchronously acquired X
server timestamp. Close uses `WM_DELETE_WINDOW` when supported, otherwise the
ordinary X client-close fallback. Reparenting away from the root retires the
native association; returning creates a fresh identity. Generation retirement
removes owned X windows without disconnecting native Wayland clients.

Native maximize/restore preserves unrelated `_NET_WM_STATE` atoms, and a
minimize-only update does not rewrite that property. Malformed or oversized
properties are left intact. An isolated X server fixture covers these cases;
concurrent direct property writers and fresh Steam/Dell acceptance remain
outside that test.

The XWM now drops synthetic X11 surface-serial events and ignores zero,
duplicate and stale serials without retiring the entire session. Its per-window
high-water mark survives an unmap. The isolated Xvfb tests exercise rejection
with the production helper and lifecycle handling with a test-only helper;
Xvfb cannot generate Xwayland's direct server-originated event. The change
still needs a real Xwayland association/remap run with the packaged version
before being deployed to the Dell.

The first live Steam review exposed a Friends input defect: native presentation
put Friends above the main window, while the X root stack put the main window
above Friends. Native activation now raises the matching X window as well as
focusing it. An overlapping-window control fails on the live baseline helper
and passes on the corrected helper. The same review confirms both Steam windows
explicitly request decorations=0; ignoring that property caused double chrome.
The restarted native O review verifies Friends' close button while it overlaps
Steam, plus Steam's own minimize button and taskbar restore, with no duplicate
chrome. The Q review additionally verifies client maximize/restore and Friends'
client-driven title drag and bottom-right resize: it moves independently from
(1070,432) to (752,234), then resizes from 300×650 to a matching 475×710 native
window and buffer. The maximized main client stays at 1920×1036.

The launcher uses a private `-displayfd` channel and checks the chosen display,
plus a bounded startup deadline. The first native attempt used `-nolock`, which
Xserver ignores for non-root callers. That attempt remains recorded; the
corrected ordinary-user launch succeeds. The new worker is deployed with the
compositor by fresh-image, image-update and private offline-staging scripts.

## Fullscreen and HiDPI (work/xwayland-hidpi)

**EWMH fullscreen.** SDL games ask for `_NET_WM_STATE_FULLSCREEN` (a property
set before mapping, or a client message) and then assume the window sits at
the output origin with the output size. SDL2's `SDL_GetWindowPosition`
returns the display origin for fullscreen windows, and MonoGame computes the
mouse as global position minus that, so a framed window (Stardew on the Dell:
client at 4,80 in a 3192x1632 maximized frame) offset the cursor by the title
bar and cut off the bottom of an output-sized back buffer. brook-xwm now
advertises the atom, reports the initial property at MapRequest and
add/remove/toggle messages, publishes the answer in `_NET_WM_STATE`, refuses
geometry requests while fullscreen and sends a synthetic root-relative
ConfigureNotify after every native configure (ICCCM 4.1.5). waylandd shows a
fullscreen window without chrome at the output origin with exactly the output
size, restores geometry/chrome/maximize on exit and ignores move, resize and
maximize requests meanwhile. The native taskbar still draws over (and takes
clicks in) the bottom strip; hiding it for a fullscreen window is a kernel
compositor follow-up.

**What Xwayland 24.1 offers.** The Dell and image profiles carry Xwayland
24.1.11 (pinned nixpkgs). `-hidpi` ("adjust to output scale when rootful") and
`-decorate` apply only to rootful mode; in rootless mode
`global_surface_scale` stays 1, no `wl_surface.set_buffer_scale` is sent for
X windows, and fractional-scale/viewport scaling is rootful-only. Native
per-client scaling (`_XWAYLAND_GLOBAL_OUTPUT_SCALE`, GNOME's
xwayland-native-scaling) is not in 24.1. waylandd sends Xwayland
`wl_output.scale` 1 and the device mode, and no xdg-output, so the rootless X
screen is 3200x1800 device pixels and every X window is shown 1:1.

**Default: apps scale themselves** (KDE's choice, GNOME's native-scaling
result). waylandd passes `BROOK_XWM_SCALE` (the output scale) and
`BROOK_XWM_PROTOCOL=2` to brook-xwm, `-dpi 96*scale` to Xwayland, and reports
RandR millimetres at 96*scale dpi to the Xwayland client only. brook-xwm puts
`Xft.dpi` and `Xcursor.size` in RESOURCE_MANAGER (keeping an existing
`Xft.dpi`) and owns XSETTINGS with `Xft/DPI`, `Gdk/WindowScalingFactor`,
`Gdk/UnscaledDPI` and `Gtk/CursorThemeSize`. GTK3, Qt6, Chromium/Electron
(via GTK settings or Xft.dpi), Tk (core DPI) and Xft text follow. Steam's
CEF UI needs `STEAM_FORCE_DESKTOPUI_SCALING`; the rootless launcher sets it
from the desktop scale (branch `work/steam-rootless-hidpi`, on top of
`work/steam-rootless`). Games keep choosing their own resolution in device
pixels.

**Fallback: scaled by the system, per app.** `BROOK_XWAYLAND_SCALED_APPS`
(waylandd's environment; comma list of WM_CLASS instance or class names,
case-insensitive, `*` for all; empty by default) marks the matching X client.
Its windows keep 1x X geometry at the native client origin and are presented
at the output scale: pointer input arrives in 1x surface units, odd native
sizes snap to whole 1x pixels, and override-redirect popups map through their
transient parent or, if none, the client's most recently active window
(native = parent + (x - parent_x) * scale). Limits: the app still sees the
global Xft.dpi/XSETTINGS (list only apps that ignore them, or launch a toolkit
app with `GDK_SCALE=1 GDK_DPI_SCALE=0.5` / `QT_SCALE_FACTOR=0.5`); it sees a
3200x1800 screen in its 1x units, so its own screen-edge placement (menus
near the bottom, centring) can be off; X pointer root coordinates of a scaled
window differ from native ones, so drag-and-drop between a scaled and a
native X window can target the wrong window. Unlike KDE's global mode, the X
root is never logical-sized.

**Compatibility.** The new wire messages (fullscreen requests, `BXWM_SCALED`)
are only sent when waylandd announces protocol 2, so a new brook-xwm under an
older waylandd behaves as before; an older brook-xwm under the new waylandd
just lacks fullscreen and hints. Both are needed for the fixes.

**Tests and evidence.** Nix check phase: the bridge test covers fullscreen
from a maximized window, request-before-map, ignored requests while
fullscreen, restore, scaled presentation, snap, client resize, popup anchors
and pointer units; the Xvfb helper test covers `_NET_SUPPORTED`, initial
property, client messages, synthetic ConfigureNotify, refused geometry while
fullscreen, remap, RESOURCE_MANAGER, XSETTINGS contents and scaled-client
marking, plus a protocol-1 run that must not emit the new messages. A private
software-composited QEMU review (Xwayland from the rootless Stardew review
image, 1920x1080 at 200%, current kernel/waylandd/brook-xwm, session
`97856bedea61430c892685d0bc0059b4`, stopped after a checked sync) used
`scripts/workloads/x11_hidpi_probe.py`: Xft.dpi 192, Xcursor.size 48, XSETTINGS
owner, core DPI 192; an Xlib window with the fullscreen property and an SDL2
(sdl2-compat) OpenGL `FULLSCREEN_DESKTOP` window both became chrome-less
1920x1080 at 0,0 with real and synthetic ConfigureNotify; SDL clicks at
(800,300)/(1500,900) reported identical event and global-minus-position
coordinates with the button held. An Xlib window at 400x300 was 1:1; the
listed class was 800x600 natively, a click 100,100 device pixels into it
arrived at 50,50 and its override-redirect popup appeared at the click. X11
Mousepad (GTK3) rendered at scale 2 with its File menu under the menu bar.
Screenshots: `build/workloads/xwayland-hidpi-20261116/evidence/`. An SDL3
window that recreates its X window after the fullscreen request (window
surface path) stayed framed; MonoGame creates an OpenGL window up front.

## Fullscreen owns the output, taskbar included (work/launchers-fullscreen)

Ian on Stardew (gen 426): fullscreen "doesn't cover the task bar, some buttons are still cut off". The window was output-sized, but the kernel WM still drew the taskbar over it and took clicks there.

- **Kernel state:** `WM_SET_FULLSCREEN` (syscall 526, `BROOK_SYS_WM_SET_FULLSCREEN` in `wm_window_state.h`) saves geometry, chrome and maximized state, then makes the window chrome-less at 0,0 at the full output size (`CompositorGetPhysDims`, e.g. 3200×1800, not the work area). The client gets `WM_EVT_RESIZED` with `WM_RESIZE_FULLSCREEN` (4, never with MAXIMIZED), and `WM_WINDOW_STATE` reports `BROOK_WINDOW_STATE_FULLSCREEN`. Leaving restores all of it. While fullscreen, maximize, moves and decoration changes are ignored (decoration requests update what leaving restores). The debug window list has `"fullscreen"`.
- **Taskbar:** `WmTaskbarHidden()` is true while the focused window is fullscreen, visible and not minimized, and neither the launcher nor the volume popup is open. Then `WmShellSurfaces` omits the taskbar, so every compositor path stops drawing it. `WmTaskbarHitTest` returns nothing, the tray tooltip is off, and input routing uses `WmTaskbarTop()` (the screen height), so the whole output reaches the window. Super opens the launcher with the taskbar above the game. Alt+Tab, minimize or focusing another window brings the taskbar back. Maximize and the work area still use `WmDesktopHeight`.
- **Older brook-xwm:** the Xwayland WM on the Dell (`h9x75map…`) makes EWMH fullscreen windows itself (chrome-less, at the origin, output-sized). The kernel counts such a focused window as fullscreen too, so the kernel pair alone fixes Stardew.
- **waylandd:** `xdg_toplevel.set_fullscreen`/`unset_fullscreen` call `WM_SET_FULLSCREEN`. A request made before the window exists is applied when it is created. The configure carries `XDG_TOPLEVEL_STATE_FULLSCREEN` at the output size. The XWM path uses the kernel call too, and falls back to its own geometry when the kernel returns ENOSYS.
- **Tests:**
  - host `test_window_surfaces`: fullscreen enter/leave, taskbar hidden/shown with the launcher, volume popup, focus and minimize, and the legacy shape;
  - waylandd checks: `xwayland-bridge-test` kernel path and fallback, and the `surface-frame-test` fullscreen configure.
- **QEMU** (software composition, 1920×1080 at 200%, session `eee90e30da484bcab4370a851e43a0c7`, evidence `build/workloads/fullscreen-20261118/evidence/`):
  - With the old brook-xwm, an X11 `_NET_WM_STATE_FULLSCREEN` probe covered all four corner markers with no taskbar. Clicks at y=1065/1070 reached the X client.
  - Super showed the launcher and taskbar over it; closing the launcher hid the taskbar again.
  - A second window brought the taskbar back; Alt+Tab to the game hid it.
  - With the new waylandd and brook-xwm, the X11 probe and an SDL2 Wayland `FULLSCREEN_DESKTOP` window both reported `fullscreen: true` at 1920×1080. The Wayland client got the click at the bottom edge, and on leaving it returned to its framed window with the taskbar.

## Evidence and remaining acceptance

The Nix check phase exercises the actual shell and bridge adapters. It covers
both association orders, protocol errors, retained pixels, independent windows,
geometry ACK ordering, stale-size frames, NULL-buffer remap, focus/close and EOF
isolation. A mapped native xdg toplevel keeps its native identity and retained
pixels through helper EOF and server disconnect. A replacement generation can
reuse XIDs/serials while rejecting delayed messages from the retired generation.
Native review N additionally terminates the owned Xwayland server: its X
windows disappear while the same native Mousepad window retains its identity,
receives focus and accepts newly typed text. Replacement-server acceptance
was subsequently exercised with Steam open as described below; Dell acceptance
remains open.
A fresh headless Xvfb control additionally exercises the real XCB
worker: InputOnly maps, border suppression, serial association, configure,
globally active client focus, cooperative close, reparent and XID reuse. The
stale-XID control also rejected a deliberately broken worker during development.
These controls do not replace native Xwayland or Steam acceptance.

In private review `steam-rootless-xwayland-n-20261024`, real xeyes windows have
exact 160x100 and 240x140 client sizes. Dragging one leaves the other stationary.
Maximize reaches a matching 1916x996 buffer; restore returns to 240x140 at the
moved position. Cropped captures show pupils responding to left/right pointer
positions. Both window buttons close their X clients. Mousepad runs as a native
Wayland client on the same compositor. The corrected compositor/worker were
hash-verified runtime overrides in this first review; repeat fresh-boot acceptance
with the corrected packaged launcher succeeds in review
`steam-client-container-o-20261024`: the service compositor starts its own
Xwayland and worker on display `:0`, without runtime executable overrides.

The same review preserves the authenticated profile and runs Steam without a
loader audit or preload. Store renders at 1280x800; native maximize produces a
1916x996 Library with game artwork. Friends opens at 300x650 and a drag moves it
independently while Library stays maximized. The native Wayland editor remains
on the same compositor. These private captures establish individual-window
presentation and real application rendering; they do not yet establish all
dialogs, successful installation or long-term stability. Friends' own client
close button is verified after the focus/stack correction; the later Q review
adds client-driven movement and resizing controls.

A later private copy of the Stardew review was attempted with
`BROOK_NET_RESTRICT=1` and silent audio to avoid contacting the Steam service
or changing cloud saves. The local debug forward remained usable and a guest
TCP connection to 1.1.1.1:80 timed out. Steam reached an “Unexpected Transport
Error” window and did not launch Stardew; this offline control does not assess
game compatibility or prove the cause of the error. The VM was stopped after a
checked filesystem and device flush. Original review disks were not modified.
The option restricts the script's QEMU user-network adapter; caller-supplied
extra network devices are outside its scope. VDE is rejected with this option.
The private stderr contains repeated WebSocket connection failures, socket
creation failures reporting unsupported address family, and failed minidump
writes. These observations do not distinguish an offline authentication failure
from an internal runtime failure. No Stardew window was observed.

On the Dell's 3200x1800 software desktop, the VM-tested compositor and XWM
were staged in RAM and launched with the installed Xwayland 24.1.11. Two xeyes
windows appeared independently; native maximize and restore returned the first
to its prior size, and closing it through native chrome left the other open.
The default Xwayland configuration had no GLX extension. A temporary wrapper
using `-shm +extension GLX +iglx`, with `LIBGL_DRIVERS_PATH` pointing at the
installed Mesa DRI directory and `LIBGL_ALWAYS_SOFTWARE=1`, exposed GLX 1.4.
The GLX control obtained a current llvmpipe context and read back a red pixel
with no X or GL errors. This is software rendering, not Intel acceleration.

The first Steam retry exited because this SSD boot lacked the SteamRT provider
cache. A cache generated and validated from the installed profile, plus its
matching loader, were installed only in the RAM initrd. With a private null
PulseAudio sink and the temporary Xwayland wrapper, the next bounded Steam run
opened separate Steam and Friends List windows. Samples reported no denied task
births and over 5 GB free; these observations do not establish input correctness,
long-term stability or game launch. Account screenshots and logs remain private.

The compositor now offers the opt-in `BROOK_XWAYLAND_SOFTWARE_GLX=1` launch
mode with the same flags and child-only Mesa environment. The default launch is
unchanged. Host checks cover actual fork/exec arguments and environment, including
restart with the optional dumb scheduler; the physical run above used the
equivalent temporary wrapper, not this newly built binary. Indirect GLX is enabled
in this opt-in mode; whether direct software GLX works without it remains open.
The newly built option was then booted in a private software-rendered QEMU
guest: the GLX control created a current llvmpipe context and read a red pixel
without X or GL errors. After terminating the owned Xwayland server, the
compositor restarted it and the same GLX control passed again. The guest was
stopped after a checked filesystem and device flush; the packaged option still
requires a Dell run.

Known gaps and remaining gates:

- Finish Steam dialogs/navigation, input-alignment and a small-download
  acceptance run through the rootless path. Initial authenticated Store,
  Library, Friends and independent movement are verified.
- Validate replacement-server behavior and Steam relaunch after server loss
  on the Dell. Private VM results are described below.
- Transient identities currently affect initial placement; later parent motion,
  minimize and stacking need broader policy/acceptance. Existing native popup
  raising also groups by provider, which can include unrelated Wayland clients.
- XShape bounding-region clipping is not implemented. Shaped xeyes buffers can
  expose undefined pixels outside the eye shapes; do not treat these captures as
  proof of clean shaped-window rendering.
- MCP semantic close can dispatch a cooperative event to an API window even
  when its client origin is unknown. It does not infer an X client PID or
  provide client-group exit confirmation without a verified origin; it never
  signals the provider as a fallback for an unknown client.
- Steam authenticated evidence, profiles and downloaded software remain private.
  The previous SDL3 host crash review disks are preserved independently.

The compositor now retries an owned Xwayland generation after server or worker
loss, with capped exponential backoff. It waits for both retired children to
be reaped, escalating a stuck child after a grace period, and avoids spawning
during shutdown. The host fixture covers a stopped worker and preservation of
an unrelated native surface. In a private software-rendered VM, terminating
the real server and then the replacement worker each produced a new generation;
a fresh xeyes window opened after each. That VM run preceded the final bounded
shutdown-cleanup adjustment, which has host-test coverage. Existing X clients
disconnected as expected. This is not a Steam-load or Dell stability result.

The final compositor build was subsequently exercised in a private,
software-rendered VM with a null-output PulseAudio server. Steam reached its
rootless main window, and compositor maximize and restore actions completed.
Terminating Xwayland while Steam was open left the compositor running; a new
server and worker appeared, and a fresh xeyes window opened through them. The
guest was stopped after a checked filesystem and device flush. Existing X
clients disconnect on server loss; this test does not show Steam automatically
reconnecting, establish long-term Steam stability, or validate the Dell.

A further private software-rendered VM run exercised a manual Steam relaunch
after server loss. Steam first opened a window using the same null-output
PulseAudio server; after terminating the owned Xwayland process, the compositor
created a new server and worker and the original Steam job drained (exit 1).
Launching the same client again reached a new Steam window. This establishes a
window-level relaunch in that test, not automatic app recovery, interaction
with the relaunched client, audible audio, or Dell acceptance. The VM stopped
after a checked filesystem and device flush.

With the cooperative-close kernel change in a private VM, xeyes reported an
unknown client origin and advertised close. A bridge close completed by event
delivery, the window disappeared and the xeyes job exited 0. Closing one of
two xeyes windows left the other and their shared compositor provider alive;
a stale window incarnation was rejected. This establishes the X11 control path
in that VM, not Steam close behavior or Dell acceptance. The top-level bare-metal
test build separately failed to link existing fixture dependencies including
ModuleRetainAddresses and CPU memory-type helpers; the kernel itself compiled.
Those standalone test dependencies were subsequently repaired, and the five
affected bare-metal fixtures passed in QEMU.

`python3 tools/vm_bridge/accept_x11_close.py --session SESSION` repeats the close
control on two pre-launched disposable xeyes windows. It verifies close-event
delivery, survival of the sibling sharing the provider, and rejection of the
closed window's reference before closing the second window. The script passed
against the private software-rendered VM; it intentionally closes both windows
and does not verify either client process exits.

A subsequent private software-rendered VM run exercised this close action on
the rootless Steam window with a null-output PulseAudio server. The cooperative
request completed and the window disappeared; Steam continued running, so
window close was not treated as application exit. A separate normal Steam
shutdown request completed with exit 0, and the original client job drained
with exit 0. A fresh xeyes window then opened on the still-running compositor.
The guest stopped after a checked filesystem and device flush. This does not
establish Steam shutdown behavior on the Dell or audible playback.

## Xwayland interval-timer lead

The serial log of the earlier rootless Steam run reports over 50,000
`ENOSYS` entries for syscall 38 from Xwayland. X server smart scheduling uses
`setitimer(ITIMER_REAL)`; this is a concrete missing interface, but the log
alone does not establish that it caused a freeze or quantify performance.
`scripts/workloads/itimer_real_check.c` provides an externally time-bounded
Linux oracle for shared-thread cancellation, fork, exec, alarm interoperability
and blocked periodic delivery. It passed ten runs on the Linux host, and its
statically linked musl build also passed there. The same static executable in a
disposable two-CPU QEMU/TCG guest failed its first `setitimer` call with
`ENOSYS`; Brook does not yet implement the interface. A correct implementation
must share the process-wide alarm, preserve it across exec (including nonleader
exec), avoid inheriting it on fork, and handle pending signals and periodic
rearming. Ordinary POSIX timers have different semantics and cannot simply be
substituted. VIRTUAL and PROF need separate CPU accounting.

Update (work/compat-batch1): `setitimer`/`getitimer` are now implemented in
`posix_timer.cpp` as one record per thread group, shared with `alarm()`.
REAL follows the semantics above (fork clears, exec keeps, a periodic timer
restarts when its SI_KERNEL SIGALRM is dequeued, ignored expiries are dropped
without restart). VIRTUAL/PROF are charged from the 1 ms scheduler tick. The
oracle's two busy-loop children still need IRQ-return delivery, which remains
the opt-in `irq-signal-probe` path.

Upstream's [smart-scheduling timer](https://raw.githubusercontent.com/XQuartz/xorg-server/master/os/utils.c)
explains the repeated REAL timer calls. Whether a `-dumbSched` diagnostic A/B
improves this particular workload remains untested. For that comparison, start
the compositor with `BROOK_XWAYLAND_DUMB_SCHED=1`; it passes `-dumbSched` to its
owned Xwayland, while leaving the default invocation unchanged. Xwayland 24.1.14
disables both smart-scheduling timer signals and threaded input for that option,
so any performance change cannot be attributed to the timer alone. Rebuilding
the compositor and its host suite passed. A private software-rendered VM clone
of the Xwayland-capable review was booted once in each mode with the same
compositor binary. In both, a 240x140 xeyes client appeared as an individual
rootless window. The normal run's sampled serial log reached at least 256
unimplemented syscall-38 calls from Xwayland; the diagnostic run showed none
through the xeyes check. The runs were not duration-matched benchmarks, and
this does not establish input correctness, performance or Steam stability.
Steam A/B and hardware acceptance are still required. This diagnostic does not
implement `setitimer`.

An initial Steam attempt in that private clone was invalid because it omitted
the earlier review's validated provider-loader payload. With the current
kernel, matching modules and the original payload restored, SteamRT progressed
and launched approximately 90 tasks, but no Steam window appeared. The private
runtime log reported repeated `Too many open files` errors. A later run on the
same private configuration showed Steam's shared descriptor table at the
kernel's 1,024-entry capacity: `/proc/89/fd` contained 945 `memfd` descriptors,
39 paths and 40 other descriptors. Several Steam threads exposed the same full
table. The log also reported repeated software Vulkan/EGL initialization
failures; this host has no available render node, so a failure/retry loop is a
plausible lead, not an established cause. Descriptor names and allocation stacks
were not captured. No matching kernel FD-exhaustion diagnostic was observed;
several descriptor installation paths return EMFILE without that log. The VM
was synced and stopped. This is not a matched Steam A/B or Dell result.

An opt-in, globally bounded `opt/memfd-trace=1` diagnostic then identified the
repeated allocations as Steam-created `pulseaudio` memfds when no Pulse server
was configured. With a private null-output Pulse server and its cookie passed
into SteamRT, the runaway stopped and a rootless Steam sign-in window appeared.
That establishes a provisioning problem in this clone, not the cause of the
Dell freeze or evidence that scheduler mode improved stability. The trace can
include application-provided names and is disabled by default. A two-CPU VM
exercise checked invalid and kernel-space names, enabled/disabled behavior,
and the 159-line lifetime cap after 4,100 creations.

The same private clone was subsequently booted with Xwayland's default scheduler
and a fresh null-output Pulse server. Steam reached a rootless main window;
one Steam process held 119 descriptors, including two memfds, at inspection.
Xwayland's sampled unimplemented syscall-38 count reached at least 14,848,
while the diagnostic-scheduler run logged none. The sequential runs reused a
writable clone and differed in persisted runtime state, so neither timing nor
window differences establish a scheduler performance effect. Neither run
establishes long-term Steam stability or reproduces the Dell freeze.

The fixture also checks caught and default `SIGALRM` delivery to a child
spinning without syscalls; these checks pass on Linux. Brook's current caught
signal delivery occurs at syscall return, so the timer must remain disabled
until IRQ return can deliver it too. An isolated `RealIntervalTimer` state
machine models pending, periodic phase and reprogramming; it is not connected
to the syscall or production alarm. Group identity, locking, signal disposition,
lifecycle and IRQ integration remain.

An independent `scripts/workloads/signal_irq_delivery.c` isolates caught
signal delivery from timers: a masked sibling sends `tgkill` after the target
enters a userspace-only loop. It passes on Linux and fails in a disposable
two-CPU Brook QEMU/TCG guest, confirming that the return-path gap is real.
The IRQ register-layout adapter is not connected to the wrappers yet. Its
host tests check the distinct syscall and IRQ register mappings; the existing
11-case guest signal-return suite passed after extracting the syscall mapping.

A QEMU-only `opt/irq-signal-probe=1` experiment now tries caught-signal
delivery at LAPIC timer and reschedule-IPI return. On a disposable two-CPU TCG
guest, the no-syscall-loop fixture passed with the option and failed on the same
kernel without it. The existing 11-case signal-return guest fixture also passed
with the option enabled. Caught-only claims now select the handler while holding
the source notification lock; hosted contention tests cover both ordinary and
POSIX timer sources. The IRQ fixture passed again with that change. This is not
production acceptance: default dispositions remain on their existing path.
The static `signal_irq_registers.c` fixture passed on Linux and an opt-in
two-CPU QEMU/TCG guest: it checks RAX/RCX/R11, carry, a red-zone sentinel,
YMM15 round-trip, normal and alternate stacks, and an edited return RIP. This
does not inspect the exported FP image, cover all registers, or prove which IRQ
wrapper delivered the signal. A separate static fixture passed on Linux and
opt-in QEMU for private alternate-stack write isolation across fork and an
unwritable alternate stack; the guest logged the IRQ frame copy failure, the
child died with SIGSEGV and the parent survived. The userspace isolation check
alone does not distinguish COW from eager copying. In a separate two-CPU TCG
boot, `opt/irq-signal-probe=2` disabled timer-return delivery; a privileged
test-kernel-only control requested reschedule IPIs while the register fixture
ran. Two target-PID `source=ipi` records and the passing fixture establish
both stack cases went through the reschedule wrapper. They do not prove which
individual IPI caused each delivery or establish physical SMP behavior.
The compiled entry audit covers both hook arguments, source and target, with
mutation checks; it remains a structural check. Concurrent termination and
broader SMP coverage remain. The option defaults off and has not been deployed
to Dell.

On the physical Dell generation 347, a static musl build of the caught-signal
fixture confirmed successful `tgkill` submission with `send_result=1` while
the userspace-only target reported `delivered=0` and exited 3. The same binary
with a periodic `gettid` syscall in the target loop exited 0. Both variants
passed on the Linux host. The guest jobs completed with no live descendants.
This confirms the delivery gap on this Dell boot. The fixture result is
consistent with the sparse Chromium profiler samples, while the profiler's
signal handling has not been directly traced. The syscall-control binary
printed the older no-syscall success label; its command line and receipt record
the periodic-syscall mode. See `build/hardware/dell-media-20261113/` under
`gen347-signal-irq-dell-detail` and `gen347-signal-irq-syscall-control`.

An independent review of the experimental IRQ path found the protected
usercopy region and deferred termination boundary coherent. It identified
concurrent group termination during blocking or COW signal-frame copy as an
unverified case. The reviewer also noted that an earlier pending default
disposition can defer a caught signal, and that the existing opt-in is QEMU
fw_cfg only. A physical diagnostic needs a separate default-off configuration
and validation of those cases.

The `signal_irq_termination.c` fixture now uses the existing mapping-test
usercopy barrier to hold signal-frame copying after prefault. In a disposable
two-CPU QEMU/TCG boot with `BROOK_MAPPING_TEST_HOOKS=ON` and
`opt/irq-signal-probe=1`, a privileged parent observed one waiting copy,
requested `SIGKILL`, and confirmed the child was still unreaped and the
barrier still held. After release it reaped the child with `SIGKILL`, saw no
handler execution, and verified its private alternate-stack bytes and a
subsequent write. The fixture reported `SIGNAL_IRQ_TERMINATION_PASS`. This
covers termination during the suspended checked copy after prefault. It does
not establish termination during COW resolution or identify which interrupt
wrapper delivered the signal. The test-only mapping barrier needed its clock
read updated to call `ApicTickCount()` directly before the kernel would build.
The first unprivileged control could not arm the privileged hook. Both
disposable VMs were stopped by the external timeout. Kernel and fixture hashes,
build output and serial logs are in
`build/hardware/dell-media-20261113/irq-signal-termination-qemu/`.

A follow-up test-only kernel logs the interrupt source after claiming the
caught signal. The marker-enabled two-CPU QEMU boot passed the same
termination fixture with `source=0` for the timer return. A second boot used
the existing test-only IPI control and `opt/irq-signal-probe=2`, which disables
timer-return delivery. It passed with `source=1`. The fixture held the same
post-prefault usercopy barrier in each run. An otherwise identical boot
without the marker or fw_cfg option failed to reach the barrier, as expected.
The new `/etc/irq-signal-probe` marker enables the existing experimental path
at boot and is absent by default. This marker permits a physical diagnostic;
it has not been deployed or validated on the Dell. The IPI fixture uses
`BROOK_REGISTER_TEST_HOOKS` and repeated test-only IPI requests. Logs and
kernel hashes are in `irq-signal-termination-qemu/`.
