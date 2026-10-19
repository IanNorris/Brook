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
