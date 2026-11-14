# Compositor direction

Brook owns the display and scanout independently of the renderer. The intended
architecture is a restartable userspace compositor with a device-neutral scene
and presentation interface. A compositor failure must not take down the OS;
uncertain GPU work must retain its resources until the device can safely retire
or recover them. Vulkan is a renderer backend, not a requirement on applications
or on the OS presentation interface.

## Current implementation

The OS window manager and software compositor remain kernel threads. On the
Dell, an opt-in supervised userspace Vulkan renderer captures the desktop's
backdrop, client surfaces, window chrome and overlay, composes them on Intel HD
Graphics, and submits completed frames through an OS-owned presentation lease.
The guarded Dell image can opt into Broadwell blits from protected renderer
outputs into the existing firmware framebuffer. Inline hardware presentation
is the default. With a buffered renderer, `BROOK_RENDER_ASYNC_PRESENT=1`
submits a ticket and polls for retirement alongside previously submitted Vulkan
work. The kernel retains the scene, display and source through the copy and
verified retirement. New capture waits during queued presentation, and the
render and blitter engines share a serial submission worker. The display plane
retains its firmware configuration and can sample the framebuffer during an update.
The two-buffer supervisor gives pending partial work up to 40 ms to retire
before scheduling an overlapping full frame. This reduces alternating
presentations on the idle Dell while keeping a second slot for slower work.
The software compositor maintains its backbuffer and resumes presentation when
the lease expires or the renderer releases it. The CPU copy remains available
when the blitter declines a frame.

Captured windows are bound by non-reused creation identities and retained
backing, not titles. A frame may lag visual changes, but it must not display
one window's content as another window after slot reuse. Sources may be written
without publication, so captured producer pixels are not necessarily immutable
snapshots. Per-window opacity, client alpha, clipping and integer scaling are
supported in the Vulkan scene; backdrop blur is not and currently makes the
renderer yield to the OS path. See `agent/AGENT_JOURNAL.md` for exact test scope,
current deployment, hardware and recovery state.

The Intel implementation supports multiple shared buffers, fences, protected
output and independent submission where ordering can be proved. The handoff
tracks the renderer incarnation and keeps uncertain GPU work quarantined. A
supervisor restarts exited renderers; a bounded recovery path can cancel a
synthetic blocked submission and restart after owner work drains. A genuine GPU
hang, stalled process exit and every shared-memory lifetime are not proven safe
by those tests. Software fallback, renderer process recovery and GPU reset are
separate guarantees.

An isolated Dell test has also passed a GPU buffer and its producer fence
between separate processes, waited on that fence in Vulkan, compared the
composed pixels, and returned an acknowledgement before producer reuse. This
establishes the transport and synchronization mechanism for that fixture. The
paired Wayland provider offers GPU buffers for eligible committed sources. The
renderer can also capture CPU-backed windows. The guarded Intel backend
snapshots already-admitted uses of a retained shared buffer into a sync-file;
a separate Vulkan process has consumed it successfully on the Dell. That
snapshot excludes later work and is not a buffer lease, so the provider must
prevent producer reuse until all consumer reads retire. Each retained buffer
must also be bound to the kernel window creation identity and its exact commit.
A renderer exit or
lost acknowledgement cannot by itself authorize buffer reuse. The software
fallback must retain safe content while GPU work is pending or uncertain.

## Next hardware performance pass

Use the same Dell for a Linux baseline and a Brook comparison. Replay a fixed
yquake2 GL3 demo with the same game assets, drawable size, render settings, swap
interval and AC power. Record the OS, Mesa and driver versions, display mode and
compositor. Use the existing [SDL swap probe](../tools/profiling/README.md) on
both systems and compare steady-state median and tail intervals. Also run the
same offscreen Vulkan scene on both systems to measure render and fence timing
without the desktop presentation path.

On Linux, collect i915 engine busy time and actual GPU frequency with
`intel_gpu_top`. If the swap trace shows stalls, use a separate instrumented
run for request and scheduler events supported by that kernel. On Brook, follow
the same producer commit from admission and cache synchronization through the
render breadcrumb, composition fence, blit retirement and acknowledgement.
Keep GPU timestamps and CPU elapsed time separate. [IGT tool documentation](https://android.googlesource.com/platform/external/igt-gpu-tools/+/refs/tags/aml_hef_341512030/man/intel_gpu_top.rst)

Use the largest measured gap to choose the first change. If Brook leaves the
engine idle between requests, investigate cache and dependency waits before
changing queue depth. If application rendering is close to Linux and most of
the gap is in desktop presentation, prioritize an OS-owned scanout buffer pool
with vblank retirement. Qualify cold and warm boots, sustained input, storage
flush and link recovery on this Dell before extending support to another GPU
or machine. The known xHCI reset timeout and later idle network drops need
separate evidence.

## Work remaining

- Develop a device-neutral presentation boundary that can use hardware scanout
  and synchronized buffer ownership without giving a renderer display control.
  The Dell blitter updates the existing framebuffer in place. Atomic display
  updates require ownership and retirement of separately mapped scanout buffers.
  `compositor_scanout_lifetime.h` contains an unwired policy for retaining
  active and pending buffers through a flip or an uncertain display state. A
  backend must supply actual producer-completion, flip-completion and display
  quiescence proofs; the policy performs no hardware access. Linux KMS describes
  the corresponding requirements to wait for rendering and to retain the old
  framebuffer until the flip completes: https://docs.kernel.org/gpu/drm-kms.html
- Preserve identity, clipping, alpha, ordering and lifetime semantics across
  renderer failure, software fallback, GPU completion and any new scanout path.
  Validate full and partial updates, effects, resizing and moving windows.
- Improve fault diagnostics and bounded recovery for actual device failures,
  retaining uncertain resources until the OS can prove they are no longer in
  use. Keep recovery images and the known-good boot slot available.
- Validate with real applications and comparable workloads. Offscreen Vulkan
  pixel tests or a frame counter alone do not establish panel correctness,
  application responsiveness, tear-free display or a performance gain.

The legacy `WM_SCANOUT_MAP`/`WM_SCANOUT_FLIP` calls expose and copy the kernel
backbuffer without a renderer ownership or completion-fence protocol. They are
not a replacement for the guarded handoff. The existing virtio GPU path and the
software compositor remain useful independent backends and fallback paths.

For the experimental direct-source channel, set `BROOK_DESKTOP_GPU_SOCKET` on
the renderer and `BROOK_WAYLAND_GPU_SOCKET` to the same path on the provider.
When their users differ, `BROOK_DESKTOP_GPU_GID` gives a numeric provider group
write access: the renderer retains ownership and creates the socket with mode
0770. Use a directory controlled by the renderer and a group limited to trusted
providers. Invalid group configuration or a failed ownership/mode change leaves
the channel unavailable. The provider still authenticates the attached renderer,
and each offered source is checked against its window owner; these checks require
a kernel with the GPU window peer and source validation interfaces.
For an explicitly prepared boot image, `--gpu-source-provider` on
`prepare_hardware_kernel_pair.py` embeds the specified provider and configures
both boot services with the same socket. It requires the Vulkan desktop opt-in
and a source script containing exactly one embedded provider service. This
configuration is experimental until the resulting image passes the recovery and
physical checks; the preparation command itself does not deploy it.
