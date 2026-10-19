# Hardware Vulkan desktop integration

Active work branch: `wip/vulkan-desktop-20261019`, starting at `f5573cef`.
The initial bring-up proved hardware Venus compute, offscreen rasterization and
native Wayland swapchains with Mesa CPU staging. See [bring-up evidence](vulkan-bringup.md).
Direct DMA-BUF presentation, automatic policy and safe staging fallback now pass
the final acceptance below on the pinned Intel configuration. Tested checkpoints
remain on the work branch; merging to main requires approval.

## Goal

Make hardware Vulkan a supported Brook desktop application path. Implement and
validate DMA-BUF presentation and the synchronization required by the pinned
Mesa Venus driver. Applications should launch without manually supplied WSI
debug flags, using an explicit, safe staging fallback where necessary. Compare
direct and staged presentation using the same vkcube and vkmark workloads.
Verify resize, minimize/restore, close/relaunch, concurrent GL/Vulkan use and
complete resource retirement over repeated cycles. Preserve existing application
behavior and saved data; require clean shutdown and filesystem checks. Keep
DXVK/Proton and broader performance optimization outside this goal. Commit tested
checkpoints to a work branch and merge only after approval.

## Required contracts and evidence

| Contract | Required evidence |
| --- | --- |
| DMA-BUF layout | One-plane advertised formats/modifiers, flags, dimensions, stride, offset and actual canonical resource extent validated before host import. Legacy GL metadata cannot be silently replaced with client claims. |
| Descriptor synchronization | Real, immutable sync-file descriptors with correct ownership, FILE_INFO, poll/epoll, seek and CLOEXEC behavior, compared with Linux. Eventfds cannot impersonate sync files. |
| GPU completion | Actual Vulkan queue work followed by an exported semaphore sync_fd; mapped results must match immediately after polling that fd, before ordinary Vulkan fence/idle waits. Preserve the selected queue ring. |
| Shared reservations | DMA-BUF import/export state belongs to the canonical resource across PRIME/GEM/rights aliases. Never omit a pending dependency or treat command-ring acceptance as GPU completion. |
| Cross-API presentation | Import the validated exported allocation into the host GL context, capture an immutable compositor-owned image, fence the compositor access before releasing the producer buffer. Handle unmappable device-local allocations without assuming a CPU mapping. |
| Automatic policy | Ordinary desktop app launches select the proven direct path or explicit staging fallback before swapchain creation. Missing sync ioctls are not a safe fallback signal: Mesa may simply skip synchronization. Force a capability-failure case to verify fallback. |
| Repeated mixed workloads | Same vkcube/vkmark runs under direct and staged policy, resize/minimize/restore/close/relaunch, concurrent GL, correct changing frames, complete byte output and lineage, timings and memory recorded. |
| Preservation and cleanup | Conserved backing/blob/aperture resources, no quarantines or app descendants/windows, heap integrity, checked shutdown, offline fsck and matching saved documents/profiles. |

## Initial implementation

The Wayland params path now retains offset/modifier fields and validates plane
indices, duplicate planes, consumed params, flags, formats, dimensions and row
layout. The shared layout helper supports a final allocation-bound check, which
must be applied by the kernel before Venus image import. Wayland's preliminary
check alone does not prove backing bounds. Existing implicit-modifier GL images
remain distinguishable from explicit linear Venus images.

Completed-only sync files are appropriate while virtgpu submissions synchronously
wait for their requested host timeline. The descriptor must be published only
after that wait, not after an ordinary queue acknowledgement. A ring0-only raw
probe tests descriptor ABI, not Vulkan GPU completion. The stronger compute
probe's optional `BROOK_VULKAN_SYNC_FD=1` path signals and exports a binary
semaphore after 256 dependent dispatches, polls the exported sync file, then
checks exact readback before waiting for the ordinary Vulkan fence.

The existing host renderer implements `PIPE_RESOURCE_SET_TYPE` for importing
untyped DMA-BUF resources into a GL context. At this initial stage, its use still
needed kernel ownership, layout and producer/compositor completion validation and
actual presentation acceptance. Unsupported import must not be hidden by relabeling a staged frame
as a direct one.

### Completed synchronization foundation checkpoint

Linux reference: `build/workloads/vulkan-desktop-sync-20261019/linux3-syncfd/`.
Brook: `build/workloads/vulkan-desktop-sync-20261019/candidate3/`, kernel SHA-256
`1f0b755c197e68ac42363b5f92eb3c581e252ec75050588b5952f3e233fb2070`, ABI 31.
Both pass the same sync-file descriptor probe: FILE_INFO count/detail and
malformed requests, permanent non-consuming read readiness, read-only/CLOEXEC,
invalid read/write and seek, LT/ET/one-shot epoll and MOD rearm, wrong descriptor,
close/dup/fork/SCM_RIGHTS lifetimes after closing the DRM context. The stronger
Vulkan probe passes three 256-dispatch/readback rounds through exported
queue-signaled semaphore sync files, plus triangle rasterization. Brook's actual
hardware GLES RGB/persistent coherent buffer checks also pass.

Brook drains all seven tracked command groups with complete stdout/stderr,
returns blob/aperture/quarantine counts to zero, balances backing mappings and
bytes, passes heap integrity, completes checked shutdown and all four offline
filesystem checks, and preserves the 12 document fixtures and saved profiles.
All 56 host tests and both compiled entry audits pass. The Wayland package's
production parameter-state, layout and existing surface ownership tests pass.
Wayland/kernel image import and automatic fallback are still subsequent work;
this checkpoint does not claim direct presentation acceptance.

The descriptor models a completed fence, not a mutable eventfd counter. The
implementation matches the [Linux sync-file ABI and implementation](https://raw.githubusercontent.com/torvalds/linux/v6.18/drivers/dma-buf/sync_file.c).
New fences are read-only, CLOEXEC and non-seekable; their readiness cannot be
consumed or signaled by the recipient. Only a verified selected-timeline wait
can create them. DMA-BUF reservation ioctls remain unimplemented at this
checkpoint, so native unstaged presentation is not enabled.

### Canonical DMA-BUF reservations checkpoint

Linux reference: `build/workloads/vulkan-desktop-reservation-20261019/linux3-intel/`.
Brook: `build/workloads/vulkan-desktop-reservation-20261019/candidate/`, kernel
SHA-256 `9c95d923c456baf15a549279873a605199e79418334e6499b557910779eb782f`,
ABI 31. The shared raw probe passes import/export READ, WRITE and RW flags,
invalid flags/descriptors, read-only copyout failure without a descriptor leak,
and aliases after closing original GEM/DRM/PRIME descriptors. The completed
reservation belongs to the canonical resource. Listed Venus BO submissions
wait for their selected timeline even when no outgoing fence was requested;
uncertain completion prevents export and safe retirement. Export waits at most
five seconds for admitted submissions and can return EINTR/ETIMEDOUT instead
of Linux's unsignaled fence. This bounded, completed-only subset is intentional.

All seven guest commands pass, including queue-signaled Vulkan semaphore
readback and persistent hardware GL. Resources return to zero, heap integrity,
checked shutdown and four offline filesystem checks pass. All 12 document
fixtures and saved profiles match the checked seed. All 57 host tests pass.

This is not yet sufficient for direct presentation: ordinary Venus shared-ring
commands may omit BO lists. Capture must require a current-frame imported WRITE
fence and enforce producer-buffer release ordering. GL export currently fences
the creating context; it does not prove completion of writes from another
importing GL context. Cross-context rendering must be rejected or tracked before
the new image-import path can claim that guarantee.

### Direct image capture checkpoint

Brook: `build/workloads/vulkan-desktop-import-20261019/direct-candidate4/`,
kernel SHA-256 `4cbe8e41e9e11759a0cbe39625d555b7380d401f5a869529bf530a4f6d813734`,
all 12 modules ABI 32. Pinned Intel/Mesa/QEMU/renderer versions remain unchanged.
Wayland binary SHA-256:
`6cc4a6ec09bcaffce6212cfa9428575fdf59a9510dda5bdc2173d75efab61c83`.

The kernel checks the held GEM capability, canonical allocation extent,
shareable Venus allocation, explicit LINEAR modifier, format/stride/offset and
current imported WRITE fence. A capture-read reservation remains busy through
the actual scene worker's import/copy/completion fence. Conflicting listed BO
submissions are rejected and completed DMA-BUF exports wait. Safe allocation or
cancellation failures preserve the frame's WRITE token for retry. Uncertain
GPU completion stays quarantined. The source lease outlives this callback.

Image preparation runs on the compositor worker, outside desktop frame
assembly. The driver imports through `PIPE_RESOURCE_SET_TYPE`, rejects changed
layouts for an already typed allocation, and never maps device-local images
on the CPU. It does not substitute a GL producer fence for Vulkan completion.
The compositor's own copy is fenced before Wayland releases the producer.
Legacy numeric/capture operations reject Venus resources.

Acceptance passed two full vkcube cycles (800×600, resize to 1280×720,
minimize/restore, maximize to 1916×996 on a 1920×1080 desktop, restore,
close/relaunch), two vkmark runs (1280×720, vertex and texture for 15 seconds
each), and four concurrent hardware GLES RGB/persistent-buffer checks. No WSI
debug flags were supplied to these Vulkan apps. Serial evidence records 44
validated direct allocation imports. Vkmark reports 15 FPS for each scene in
both runs; this is not yet a comparison with staging on the same compositor.

All nine tracked command groups drain complete stdout/stderr and descendants.
The guest rejects 17 malformed/unsupported capture requests without host image
import. Final blob/aperture/quarantine/failed-retirement counts are zero; all 12
backing mappings and 4,325,376 bytes balance. Heap integrity, checked shutdown,
launcher/QEMU exits and all four offline filesystem checks pass. All 12 document
fixtures and saved profile trees match the checked seed. All 57 host tests pass,
including real scene-service queued reservations, cancellation/admission rollback,
worker import failure and exactly-once completion with a live source lease.

Earlier private failures remain preserved: an initial bridge epoch timeout shut
down cleanly before any app launch; a repeat direct run hit a retained-scene
failure and required a forced stop; the first negative probe used an unsupported
raw shareable allocation and failed before WSI. None qualify as acceptance.
The corrected direct candidate above uses fresh clones of the checked seed.

Reproduction uses `scripts/workloads/stage_vulkan_nix.py --wayland PACKAGE` to
install and hash-check the selected server on a new offline image, followed by
`scripts/workloads/vulkan_wayland_check.py --presentation direct --composite gpu
--vkmark build/vulkan-vkmark-wayland-result` with explicit checked seed, Nix seed,
output and matching control arguments. Private images and raw logs stay outside
git. Automatic policy/fallback and the controlled staged comparison were still
open at this checkpoint; the final acceptance below completes them.

![Direct Vulkan cube on Brook](images/brook_vulkan_direct_1080.png)

![Direct Vulkan vkmark on Brook](images/brook_vulkan_direct_vkmark_1080.png)

## Desktop policy and final acceptance

Ordinary launches inherit a pinned Venus ICD and a presentation policy from the
kernel's desktop environment. GPU composition selects direct presentation only
when context initialization, blob resources, host-visible memory, the Venus
capset and the image-import operation are available. Otherwise it selects
`MESA_VK_WSI_DEBUG=sw,noshm` before creating a swapchain. The two environment
arrays are immutable; Terminal, launcher and bridge-spawned descendants use the
same defaults. Explicit caller overrides remain available for diagnostics.
Tests remove each required capability independently. A CPU-composition boot
exercises the fallback in an actual guest, with empty per-app environment
requests. Missing optional Mesa synchronization ioctls are never used as a
fallback mechanism.

A visible SDL3/GLES2 Wayland regression exposed a valid XRGB view of canonical
BGRA storage. Capture now keeps the canonical storage format for the copy and
applies opacity to the destination view. It rejects incompatible channel order,
alpha views of opaque storage and invalid formats. The acceptance probe checks
60 RGB frames, rejects Wayland protocol errors even if SDL reports successful
swaps, and verifies that its scenes, queued jobs and snapshots retire.

Completed sync-file exports now publish with a slot pin installed under the FD
table lock. Each export adopts that pin through copyout and rollback, releasing
it exactly once. A sibling close cannot reuse the slot until cleanup finishes,
including replacement with an alias of the same object. The production FD-table
regression covers deferred close, blocked replacement, successful copyout,
failed copyout and full-table publication failure.

Final hardware synchronization run:
`build/workloads/vulkan-desktop-final-20261019/candidate3/`, kernel SHA-256
`a913ceccb7849c8e64ff358f4a6e02a79f0e3517c92c08c2bd8d4e5435549626`,
all 12 modules ABI 32. All eight command groups complete with full output and
no descendants: raw context/query/blob/sync-file contracts, vulkaninfo, three
256-dispatch Vulkan readback rounds with exported queue semaphore sync files,
hardware persistent GL and visible Wayland GL. Triangle rasterization and
readback before ordinary Vulkan fence waits pass. Renderer diagnostics, heap,
resource retirement, checked shutdown, four offline filesystem checks and
12 saved-document fixtures plus profile trees pass. All 57 host tests and both
compiled entry audits pass.

Final comparison: `build/workloads/vulkan-desktop-policy3-20261019/`, using the
same final kernel, all 12 module hashes, pinned host packages and loaded renderer
hash. Each configuration passes two cube lifecycle cycles and two vkmark runs,
with four overlapping hardware GL RGB/persistent-buffer probes. Both GPU cases
also pass visible SDL3/GL rendering and 17 malformed-image requests. The direct
run records 46 actual Venus image imports; both staged cases record zero.

| Presentation / composition | Vertex FPS, runs 1 / 2 | Texture FPS, runs 1 / 2 |
| --- | --- | --- |
| Automatic direct / GPU | 15 / 15 | 15 / 15 |
| Forced staging / GPU | 17 / 17 | 16 / 17 |
| Automatic staging / CPU | 53 / 52 | 53 / 53 |

These are small acceptance samples using identical vkmark 1280×720 vertex and
texture scenes, 15 seconds each. Direct and staged GPU runs are comparable;
CPU composition is a separate configuration. Direct import is currently slower
than staging with GPU composition. No performance improvement is claimed.

All 11 tracked groups in each GPU run and all nine in the CPU fallback drain
complete stdout/stderr and descendants. Final windows, GPU blobs, aperture
leases, quarantines and failed retirements are zero. Each GPU run balances
16 backing mappings and 5,402,624 bytes. Each configuration passes heap checks,
checked shutdown, launcher/QEMU zero exits and four offline filesystem checks.
The 12 document fixtures retain their bytes and UID/GID; saved configuration,
Firefox, Codium and Wine profile trees match the checked seed in all three
runs. Comparison, preservation and requirement audit receipts are saved beside
the private evidence. Failed earlier runs remain preserved and are excluded
from these results.

To reproduce the comparison, run `vulkan_wayland_check.py` inside `nix-shell`
from the repository root with distinct new output directories and the same
checked seed, staged Nix seed, control package and frozen artifacts:

```sh
python scripts/workloads/vulkan_wayland_check.py --output NEW_DIRECT \
  --seed CHECKED_SEED --nix-seed STAGED_NIX_IMAGE --control CONTROL \
  --artifacts FROZEN_BUILD --vkmark VKMARK_PACKAGE --presentation auto --composite gpu
python scripts/workloads/vulkan_wayland_check.py --output NEW_STAGED \
  --seed CHECKED_SEED --nix-seed STAGED_NIX_IMAGE --control CONTROL \
  --artifacts FROZEN_BUILD --vkmark VKMARK_PACKAGE --presentation staged --composite gpu
python scripts/workloads/vulkan_wayland_check.py --output NEW_FALLBACK \
  --seed CHECKED_SEED --nix-seed STAGED_NIX_IMAGE --control CONTROL \
  --artifacts FROZEN_BUILD --vkmark VKMARK_PACKAGE --presentation auto --composite cpu
python scripts/workloads/vulkan_desktop_compare.py --direct NEW_DIRECT \
  --staged NEW_STAGED --fallback NEW_FALLBACK --output NEW_COMPARISON.json
```

Run cases sequentially, after the preceding VM's checked stop. The comparator
requires matching kernel/module/host-stack hashes, workloads, dimensions, drained
commands, renderer diagnostics and retirement. Document/profile preservation is
an additional offline audit; the comparator does not claim to establish it.
Only the tested Intel UHD 630 configuration is accepted. NVIDIA and other
render nodes are untested. Exported fences remain the bounded completed-only
subset described above; arbitrary asynchronous fence merging and GL writes
from other importing contexts are unsupported. Direct presentation still copies
to an immutable retained compositor image; it does not imply zero-copy scanout.
DXVK, Proton and broader performance optimization remain outside this work.
