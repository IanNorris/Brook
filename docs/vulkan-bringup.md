# Vulkan bring-up through Venus

Planning checkpoint: 19 October 2026, based on `fc46b6d4` (the accepted Wine
checkpoint, now on main). No Vulkan workload has been accepted on Brook.
Implementation starts separately on `wip/vulkan-bringup-20261019`.

## Target and boundaries

Run unmodified Linux Vulkan applications through Mesa's Venus driver and
Brook's virtio-gpu DRM compatibility layer. First prove GPU results without a
window; then establish Wayland presentation and a small real workload. Retain
the working VirGL desktop and GL application path.

This work does not include native Intel/NVIDIA kernel drivers, PCI device
assignment, a Vulkan compositor rewrite, DXVK, Proton, Android or performance
claims based only on device enumeration. Software Vulkan is a useful reference,
but does not pass the hardware-acceleration gate.

The intended route is:

```text
Linux Vulkan application
  -> Vulkan loader + Mesa Venus ICD
  -> Brook DRM ioctls, mappings, completion and resource ownership
  -> virtio-gpu Venus context
  -> QEMU + virglrenderer
  -> selected host Vulkan driver/GPU
```

## What already exists

| Layer | Verified in source at this checkpoint | Remaining work |
| --- | --- | --- |
| Launcher | `BROOK_GPU=venus` requests blobs, host memory and shared memfd machine RAM. | Validate the actual host stack, use reproducible package paths and record the selected renderer. Flags alone are not acceptance. |
| Device transport | Negotiates blob/context-init feature bits, enumerates capsets, discovers the host-visible PCI shared-memory region. | Blob create/map/unmap, aperture allocation, mapping attributes and a usable Venus context. |
| DRM capabilities | Reports VirGL/VirGL2; reports blob and host-visible support as zero. | Derive capability reporting from negotiated and implemented support. Keep Venus unavailable until its required path works. |
| Contexts and submission | App contexts select VirGL; submission validates and chunks VirGL command streams. | Parse context parameters and select Venus per DRM file; a Venus submission must not pass through the VirGL packet parser/chunker. |
| Memory and lifetime | Stable mmap tokens, canonical GEM/PRIME references, retained VMAs and fenced retirement. | Extend these ownership rules to blobs and host aperture mappings; ordinary ext2 shared pages are a different backing type. |
| Completion | Existing GL fence barrier; outgoing fence FD is an eventfd after completion. | Audit the pinned Venus driver's actual rings, input/output dependencies, wait and sync-file/syncobj requirements. Existing GL behavior is not the complete Linux synchronization ABI. |
| Wayland | Working GL/PRIME presentation and CPU surfaces. | Verify Vulkan WSI export/import, formats/modifiers and synchronization across the application and compositor. |

Relevant code: [launcher](../scripts/run-qemu.sh),
[DRM compatibility](../src/kernel/src/syscall.cpp),
[GPU operations](../src/kernel/src/gpu_app.h),
[virtio driver](../src/drivers/virtio_gpu/virtio_gpu_mod.cpp) and
[resource ownership](gpu-resource-lifetime.md).

## Host gate: establish this before kernel implementation

Read-only inventory on 19 October found Linux `6.12.109`, Intel i915 at
`0000:00:02.0` (`8086:9bc5`, currently `renderD128`) and NVIDIA at
`0000:01:00.0` (`10de:13bc`, currently `renderD129`), driver `580.173.02`.
Render-node numbering must be resolved and recorded again for each run.

Evaluating the repository's [Nix pin](../nix/nixpkgs.nix) reports Mesa `26.0.6`,
virglrenderer `1.3.0`, QEMU `10.2.2`, and Vulkan loader/tools `1.4.341.0`.
The launcher currently resolves host acceleration packages from `nixpkgs#...`,
which may differ from that pin. Supply frozen paths through `BROOK_GPU_QEMU`,
`BROOK_GPU_MESA` and `BROOK_GPU_GLVND`; record the host Vulkan ICD separately.
Verify the guest Mesa output actually contains the Venus ICD and its closure.

QEMU's current documentation lists Linux 6.13+ for Vulkan passthrough and
requires Venus to be enabled with blobs and a host-memory window. The present
host is below that documented baseline. Establish whether its maintenance
kernel/backports support the required mappings, or use a suitable reference
host; do not diagnose a host mapping failure as a Brook syscall defect.
[QEMU device and host requirements](https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html#virtio-gpu-virglrenderer).

Mesa documents host-driver external-memory requirements and important
host-visible mapping/cache-coherency assumptions. It also identifies newer
kernel/QEMU/PAT requirements for some CPU/GPU combinations. Check the selected
hardware against those details; a working host GL renderer is insufficient.
Start with Intel for the existing path, and compare NVIDIA after a working
baseline. This plan does not change host kernels or drivers.
[Mesa Venus requirements](https://docs.mesa3d.org/drivers/venus.html#requirements),
[host-visible memory](https://docs.mesa3d.org/drivers/venus.html#vk-memory-property-host-visible-bit).

## Milestones and acceptance gates

| Gate | Deliverable | Evidence required |
| --- | --- | --- |
| V0: reproducible host/control | Host Vulkan inventory, frozen packages and a Linux reference guest using the same QEMU/render node. | Hardware Vulkan device identified; Venus enumeration plus a mapped-memory/submit/readback test works in the Linux guest. A host-only or vtest result does not establish QEMU/KVM blob mappings. If this fails, preserve logs and resolve the host boundary first. |
| V1a: context and capability contract | Small raw-DRM probe compared against Linux, followed by typed context/capability plumbing. | Correct supported-capset mask, context parameters and negative cases. Preserve VirGL behavior. Do not advertise unimplemented features to make Mesa continue. |
| V1b: blob memory | Guest-backed and host-visible blob operations required by the pinned driver. | Checked size/alignment/overflow, mmap offsets, CPU/GPU visibility, aperture bounds and exhaustion, unsupported modes, map/unmap and close/fork/PRIME lifetimes. Test failure rollback and delayed retirement. |
| V1c: submission and completion | Venus submission/rings and the synchronization subset actually requested by Mesa. | Input dependency ordering, GPU completion, bounded waits/timeouts, repeated submits, concurrent contexts and close during pending work. Queue acknowledgement is not GPU completion. Reject unsupported flags instead of ignoring them. |
| V2: offscreen Vulkan | `vulkaninfo --summary`, a generated compute/readback probe and a small triangle render. | Force the guest Venus ICD; record physical-device/driver properties and host backing. Compare exact integer compute results and selected pixels in a known image. Exercise memory mapping and applicable flush/invalidate behavior. Software fallback is an explicit failure for the accelerated lane. |
| V3: Wayland presentation | `vkcube` through the ordinary Wayland path. | Legible capture at 720p then 1080p; resize, minimize/restore, close/relaunch and repeated frames without stale surfaces or host renderer errors. A CPU copy of GPU-rendered readback may be a separate diagnostic presenter, but does not pass native Vulkan WSI. |
| V4: workload and coexistence | A bounded `vkmark` scene plus a generated GL workload. | Correct visible output, stable concurrent GL/Vulkan use, complete command logs, clean teardown and conserved resource counts over repeated cycles. Record timings and memory; tune only after correctness. |

For the exact ioctl layouts and dependency fields, compare the pinned Mesa and
libdrm call sites with the [Linux virtgpu UAPI](https://raw.githubusercontent.com/torvalds/linux/master/include/uapi/drm/virtgpu_drm.h).
Choose the minimum correct synchronization path from that trace. Do not promise
all timeline, external-fence or cross-device features before observing which are
required and implementing their semantics.

## Implementation rules

- Keep capset/context identity explicit in DRM objects and driver operations.
  Native compositor calls continue to use their existing VirGL context. App
  GPU registration must not require successful accelerated composition when
  testing Venus with CPU composition.
- Add blob operations without treating host BAR pages as PMM-owned RAM. Define
  cache attributes, aliases, backing ownership and retirement before publication.
  Release a mapping/aperture extent only after the last relevant owner and
  verified completion; retain/quarantine it if completion is uncertain.
- Preserve stable resource incarnations, checked user copies and failure
  rollback. Bump the module ABI when the driver/kernel operation contract changes
  and rebuild all modules. Do not build the kernel while a VM is alive.
- Validate capability-negative configurations: no 3D, VirGL without host memory,
  missing Venus capset and an unavailable requested render node. Existing GL
  workloads must still work with Venus disabled.
- Extend the existing MCP GPU statistics with blob bytes, aperture usage,
  pending completions, wait timeouts and quarantines when those resources exist.
  Take CPU/context-switch profiles only where they answer a specific stall.

## Test and evidence discipline

Use one isolated VM at a time, initially eight vCPUs, 4 GiB, CPU composition and
silent audio. Set an explicit host-memory aperture and record it independently
of guest RAM. Keep a memory floor and bounded iteration count; do not run the
heavy browser/game collection while establishing Vulkan correctness.

Build small generated probes and install the loader, Venus ICD, tools and test
clients through the existing Nix path. Freeze the closure, environment, module
ABIs, kernel hash and source revision in each run manifest. Keep observations
separate from claims: a capset log, `vulkaninfo`, an image and verified GPU
execution establish different things.

Each accepted checkpoint needs full output/lineage drainage, resource and heap
checks, checked shutdown, owned QEMU/launcher exits, offline filesystem checks
and unchanged existing document/profile fixtures. Keep the Wine prefix and
Notepad document for regression checks. Raw profiles, private images and licensed
game data remain outside git. Add host tests for actual new memory/sync logic
and guest/Linux regressions for observable ABI behavior.

The first implementation session should deliver V0 and a recorded failing raw
Brook probe with its next concrete ABI gap. Reassess scope at that point before
committing to V1–V4. No implementation or VM launch is part of this planning
checkpoint.

## Proposed next goal

> Bring up hardware-backed Vulkan on Brook through Mesa Venus and virtio-gpu.
> First establish a reproducible host/Linux-guest control on the selected GPU,
> then implement and test the required capability/context, blob-memory and
> synchronization semantics. Pass deterministic offscreen compute/render
> readback, then native Wayland vkcube and a bounded real Vulkan workload while
> preserving GL behavior. Use isolated disks, complete command evidence, checked
> shutdown and offline filesystem checks; preserve existing documents/profiles.
> Record screenshots, renderer identity, remaining gaps and resource cleanup.
> Keep Proton/DXVK, native hardware drivers and compositor rewrites out of scope.
> Commit tested checkpoints to the Vulkan branch; merge only after approval.
