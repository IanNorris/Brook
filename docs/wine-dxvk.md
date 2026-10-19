# Wine and DXVK acceptance

Work branch: `wip/wine-dxvk-20261019`, based on Vulkan desktop checkpoint
`6546b611`. Main is unchanged; merging requires approval.

## Scope

Establish a reliable Direct3D 11 application path through Wine and DXVK on
Brook's hardware Vulkan stack. Begin with a deterministic Windows rendering
probe, then a lightweight Windows game or application. Verify rendering,
input, audio where supported, window management, repeated launches, resource
retirement and checked shutdown. Record startup and steady performance and
fix bounded compatibility gaps. Preserve Linux GL/Vulkan acceptance and saved
data. Full Steam/Proton, anti-cheat and broader optimization are excluded.

## Pinned starting point

- Wine64 11.0 and the populated, checked Wine prefix from
  [console/Notepad acceptance](wine-smoke.md).
- DXVK 2.7.1 from the existing pinned nixpkgs, x64 DLLs only. The primary
  [release documentation](https://github.com/doitsujin/dxvk/blob/v2.7.1/README.md)
  identifies `d3d11.dll` and `dxgi.dll` as the D3D11 pair.
- Mesa Venus 26.0.6, QEMU 10.2.2 and bounded virglrenderer 1.3.0 on Intel UHD
  630, using the [accepted desktop presentation path](vulkan-desktop.md).
- Eight vCPUs, 4 GiB, private checked disk clones, GPU composition and silent
  host audio. No host desktop or microphone access.

DXVK DLLs are copied beside the test executable, with native D3D11/DXGI
selection scoped to the test. A separate copy of the populated prefix keeps
the original profile unchanged. Per-app WSI debug flags are not supplied.
DXVK device logs must identify Venus; Wine's builtin OpenGL D3D path is not
accepted as DXVK success.

## First probe

`tools/d3d11-probe-pkg` builds a Windows x64 executable with pinned MinGW.
It requests a hardware D3D11 feature-level 11.0 device, compiles vertex/pixel
shaders, draws a red triangle on a blue target and checks both center and
corner pixels after staging readback. This distinguishes a real shader draw
from successful device creation or a clear-only result. It has an offscreen
mode to isolate rendering from window-system integration, and a windowed mode
with bounded frames and input/size event counters.

The initial diagnostic run is under
`build/workloads/wine-dxvk-20261019/candidate/`; it is not yet application
acceptance. Build with `nix-build tools/d3d11-probe-pkg -o build/d3d11-probe-result`.
Private disks, executables and raw logs remain outside git.

## Completion gates

1. Prove native DXVK selection and hardware Venus rendering with deterministic
   readback; retain exact logs and complete command lineage/output.
2. Exercise a visible D3D11 window, keyboard input, resize, minimize/restore,
   close/relaunch and a lightweight Windows workload. Measure startup and frame
   timings. Observe audio if the selected workload produces it.
3. Repeat with Linux GL/Vulkan regression workloads; require complete resource
   retirement, heap integrity, saved document/profile preservation, checked
   shutdown and offline filesystem checks.
4. Commit tested checkpoints to the work branch. Main requires approval.

## Initial diagnostics

The first prefix preparation failed during Python metadata copying, before
DXVK ran; its traceback exceeded bridge retention. That run is excluded and
its stopped disks are preserved. The retry uses a separate byte/hash/mode
verified prefix copy. The original saved prefix is never the test prefix.

Vanilla Wine 11.0 with DXVK 2.7.1 creates a hardware D3D11 feature-level 11.0
device on Venus and renders/readbacks the expected red triangle and blue corner.
One run then loses the Wine group; another returns zero but a Wine service
faults. Neither is accepted. The fault instruction matches `nsiproxy.so`'s
`update_if_table`, immediately after `if_nameindex()` returns NULL. Brook lacks
AF_NETLINK route queries, which the pinned glibc needs for interface enumeration.
Wine 11 dereferences that failure result without checking it.

`tools/wine-brook-pkg` backports upstream Wine commit
[`08dbf01a`](https://github.com/wine-mirror/wine/commit/08dbf01aaa3689f8c8a8a22276658f372b023b03),
which checks the NULL result and reports the failure. It does not implement
interface enumeration or fabricate a successful netlink response. The patched run
`build/workloads/wine-dxvk-20261019/patched-candidate3/` passes three hardware
D3D11 triangle/readback launches and normal exits, without the former user
fault. Original documents and profiles match the checked seed after shutdown
and offline filesystem checks. This is an offscreen checkpoint, not full
window/game acceptance.

Brook now implements committed `wp_viewporter` crop/destination state, buffer
scale and all eight transforms. Each commit retains a geometry wrapper around
shared immutable pixel storage. Metadata-only commits do not recapture or
release DMA-BUF producers. Cursor selection also uses committed geometry.
CPU sampling and GPU UVs use matching pixel centers, with 64-bit rational UVs
for fractional sources and clipping. Integer scene requests retain their
semantics; optional flags use the existing reserved wire word. The internal
module interface is ABI 33, requiring the kernel and every driver to rebuild
together. Geometry follows the [viewporter specification](https://raw.githubusercontent.com/wayland-mirror/wayland-protocols/main/stable/viewporter/viewporter.xml)
and [core buffer transform/scale contract](https://raw.githubusercontent.com/wayland-mirror/wayland/main/protocol/wayland.xml).

Validation so far:

- All 57 hosted tests passed; the scene test additionally covers fractional
  crops, maximum fixed-point numerators, all transforms, and a combined
  non-square, scale-two, reflected quarter-turn case.
- All ten production Wayland tests pass, including committed/cached viewport
  pixels, shared DMA-BUF storage without duplicate capture/release, cursor
  sampling, destruction/recreation, and ten protocol error scenarios.
- `viewport-candidate` and `viewport-diagnostic2` pass the actual hardware
  compositor boot readback, including fractional crops and all transforms.
- `viewport-diagnostic2/window1-initial.png` shows the D3D11 triangle in a
  correctly cropped 648x394 Wine frame, without the former padded white area.

These remain diagnostic runs, not complete Wine/DXVK acceptance. The second
repeated offscreen launch in `viewport-candidate` is killed with SIGKILL during
device teardown after correct shader readback. No guest fault or renderer error
was recorded. A CSD mouse drag reaches Brook's resize handler and Wine's
configure acknowledgement, but a new frame has not appeared; native dimensions
are 1288x753 while the rendered backing remains at its earlier size. Both runs
completed checked shutdown and offline filesystem checks. Process tracing and
a deterministic Win32 resize are the next diagnostics. Windows Snake, repeated
visible launch/close, profiling, Linux GL/Vulkan regressions and final saved-data
audits remain required before completing this goal.

The `maximize-candidate` diagnostic identified the intermittent SIGKILL. Wine's
server (PID 39/incarnation 244) retained former service PID 61/incarnation 248
for its delayed cleanup timer. Brook immediately recycled ID 61 as a new Mesa
worker (incarnation 322) in D3D process 33/incarnation 242. The server's later
`kill(61, SIGKILL)` therefore killed the D3D group. The serial log records both
the old exec identity and the new fatal target. Thread-directed PID lookup and
fatal group delivery were behaving as expected; recycling the newest freed ID
made the numeric-ID race frequent. The candidate allocator now prefers unused
IDs and recycles free IDs in FIFO order, retaining birth reservations and full
namespace exhaustion behavior. Wraparound, reuse order and concurrent shutdown
reservation tests pass along with all 57 host tests. The first cyclic VM passes
three repeated D3D11 renders and exits; full window/game acceptance remains open. A bounded
numeric PID namespace still permits reuse after a complete cycle; this does
not provide pidfd-style lifetime identity.

A separate configure issue is now isolated: Wine deliberately ignores normal
state size hints. Brook was omitting `MAXIMIZED` when its native WM maximized a
window, so the WM enlarged the presentation without resizing the D3D target.
The candidate carries native maximize state with the resize event and preserves
its desired dimensions during focus changes. CSD mouse-drag resizing still
needs a `RESIZING` transaction; deterministic Win32 `SetWindowPos` resizing to
1280x720 already passes shader readback and normal close in
`fatal-trace-candidate`. That run is diagnostic (temporary kernel logging and
process tracing), and stopped at the assertion detecting the missing native
maximize redraw; it is not full acceptance.

`resize-candidate2` passes one offscreen render and two complete visible D3D11
cycles: identity shader readback at 640x360 and 1280x720, actual CSD drag,
minimize/restore, true maximized redraw, restore to the dragged dimensions,
input and normal exit. Native normal resize/restore now uses a bounded
`RESIZING` transaction. It clears after a successful commit acknowledging the
current configure; activation preserves the desired pair and state, stale
acknowledgements cannot complete a replacement, and a valid smaller client size
is accepted. Protocol tests exercise these ordering cases. Drag actions wait
for Wine to acquire its resize grab before moving the pointer; the earlier
short wait produced a zero-distance drag and is excluded.

The Windows SDL3 Snake executable reaches D3D11 device/swapchain creation but
returns failure because DXVK 2.7.1's `SetRotation` returns `E_NOTIMPL` for the
identity request SDL3 makes. The next package checkpoint adds identity-only
Set/GetRotation handling, following the no-op behavior in
[current upstream DXVK](https://github.com/doitsujin/dxvk/blob/master/src/dxgi/dxgi_swapchain.cpp).
Actual non-identity rotation remains unsupported, and invalid requests retain
errors. The D3D11 probe now tests that contract; the patched package and Snake
still need runtime acceptance. Temporary kernel termination logging has been
removed from the production candidate; its patch and diagnostic artifacts are
preserved with the workload evidence.
