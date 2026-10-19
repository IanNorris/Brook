# Vulkan bring-up through Venus

Started: 19 October 2026, based on `fc46b6d4` (the accepted Wine
checkpoint, now on main). Hardware Vulkan compute/readback now passes on Brook; graphical presentation remains unaccepted.
Work is isolated on `wip/vulkan-bringup-20261019`; main is unchanged.

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

V0 is complete. The next implementation gate is typed context/capability handling,
followed by host-visible blob ownership, mapping and completion. Brook Vulkan
execution and presentation remain unaccepted.

## V0 results: 19 October 2026

The Intel path passed on this host's Linux **6.12.109**. The documented 6.13
baseline remains useful guidance, but a host upgrade is not a prerequisite for
this measured configuration. This does not establish NVIDIA support.

| Control | Result |
| --- | --- |
| Host, pinned Mesa 26.0.6 Intel ICD | UHD 630 (`8086:9bc5`), hardware compute/readback passed. |
| Diskless Linux 6.18.26 guest, pinned QEMU 10.2.2 / virglrenderer 1.3.0 | Venus driver ID 22, `Virtio-GPU Venus (Intel(R) UHD Graphics 630 (CML GT2))`; three rounds of 16,384 integer words matched exactly. Normal poweroff, no renderer errors. |
| Brook, accepted ABI 29 kernel, private copies of the Wine checkpoint | Expected prerequisite failure: blob=0, host-visible=0, capset mask=6 (VirGL/VirGL2). Context initialization was **skipped**, so `context_errno=0` in this initial raw probe is not a successful Venus context. |

The probe exercises mapped upload, GPU buffer copy/fill, compute, GPU readback,
barriers and fence waits. Both tested memory types reported coherent flags 7;
noncoherent memory behavior and graphical presentation are not established.

The first Linux attempt exposed a host launch omission: virglrenderer's helper
could not load `libvulkan.so`. Adding the pinned Vulkan loader to its library
path made the same control pass. The Brook launcher now resolves that dependency
only for `BROOK_GPU=venus`; `BROOK_GPU_VULKAN` freezes its package path and
`BROOK_GPU_HOST_ICD` selects the host ICD explicitly. Its launch was verified
by the Brook baseline, without changing the GL path.

Reproduce the diskless reference from the repository root:

```sh
nix-build tools/vulkan-linux-control -o build/vulkan-linux-control-result
python scripts/vulkan_linux_control.py \
  --output build/workloads/vulkan-control-NEW \
  --host-icd "$(python -c 'import json; print(json.load(open("build/vulkan-linux-control-result/manifest.json"))["mesa"])')/share/vulkan/icd.d/intel_icd.x86_64.json"
```

The output directory must be new. The launcher records exact packages, GPU,
kernel/initrd hashes, environment, arguments and complete serial/renderer logs.
It requires an idle host VM slot, KVM/render-node access and a normal host
permission context. It attaches no disks or network and sends no input to the
host desktop. A timeout is recorded as failure, with only its owned diskless
QEMU process stopped.

Local evidence: `build/workloads/vulkan-v0-20261019/`. The acceptance audit
preserves ten existing documents, the mapped-write fixture and Wine Notepad
document, plus identical recursive contents of `.config`, `.mozilla`,
`.vscode-oss` and the Wine prefix. Brook heap integrity passed; checked shutdown,
launcher/QEMU exits and all four offline filesystem checks passed. Raw images,
profiles and logs remain outside git.

The exact Mesa source narrows the first implementation: this build simulates
DRM sync objects/submission internally. Initial Venus shared rings and mapped
Vulkan allocations need HOST3D mappable blobs, 64 rings, ring-index submission
and an outgoing completion FD. Full DRM syncobj/input-fence support is not a
prerequisite for this initial lane. Renderer command consumption and Vulkan GPU
completion must remain distinct.

## V1 context checkpoint

Context configuration now travels explicitly from the DRM file to the driver.
Capability reporting intersects implemented app support with enumerated host
capsets; unsupported capsets and versions fail before host submission. Venus
capset version zero is recognized correctly, while Venus/blob capabilities
remain hidden until their transport exists. All drivers were rebuilt for ABI 30.

Eleven raw context cases pass in Linux and Brook, including explicit VirGL2,
default initialization, repeated initialization (`EEXIST`), invalid/duplicate
parameters, bounds and bad pointers. This is a supported subset: debug names and
nonzero poll masks remain unavailable; accepting a ring count does not establish
multiring submission. EXECBUFFER ring/completion semantics remain V1c work.

All 53 host suites and both compiled entry audits pass. Private guest checks
pass full/partial/independent-offset transfers, PRIME ownership and shared fork,
stale mmap tokens, and canonical capture formats. A stale PRIME test expected a
byte buffer to pass display-source validation; it now correctly expects EINVAL,
paired with the separate ARGB/XRGB acceptance cases.

An actual pinned Mesa EGL/GLES surfaceless test selects hardware VirGL and reads
back exact red, green and blue pixels. Its guest closure is explicitly staged:
the Wine seed did not contain the pinned Mesa package. The first missing-package
attempt is retained separately, not counted as a GL pass. This environment lacks
`/dev/fuse`; the private image was updated with offline debugfs and checked by
fsck instead. The aperture physical base is confirmed as `0x7000000000`, length
1 GiB, above existing direct/identity mappings. Host cached and write-combining
policies still require distinct user leaf encodings and borrowed ownership.

Evidence lives under `build/workloads/vulkan-v1-context-20261019/`. Context,
GL transfer/lifetime and Mesa readback runs use isolated images and checked
shutdown. This context-only checkpoint predates the compute result recorded below.

## Mapping cache preparation

The next memory preparation checkpoint initializes an unused PAT slot4 as WC
on the BSP and each AP before CPU admission. It preserves existing slots,
rejects unfamiliar firmware layouts, disables interrupts during programming,
verifies readback and invalidates local translations. A sticky CPU failure
keeps write-combining unavailable. This boot-time contract relies on Brook's
own initial page tables not using slot4; it is not a runtime PAT reconfiguration
API.

Eager borrowed device mapping requests can carry checked cache flags. Fork now
preserves PAT/PWT/PCD; actual VMM tests verify WB/UC/WC leaf encodings through
fork, PROT_NONE/restoration, fixed remap and final borrowed backing release.
Managed mappings cannot request a nondefault cache type.

All 54 host suites passed; focused PAT/VMM suites and compiled entry audits
passed again after interrupt hardening. The final eight-vCPU private boot
confirmed all eight PAT values and repeated hardware Mesa RGB readback, with
checked shutdown and offline filesystem checks. Evidence:
`build/workloads/vulkan-pat-20261019/`. This does **not** yet establish a mapped
Venus blob or GPU/CPU coherence of WC memory; that is the following blob gate.

## Hardware compute checkpoint

Brook now executes the generated Vulkan compute/transfer/readback probe through
**Venus on the Intel UHD 630**, driver ID 22. Three rounds of 16,384 integer
results match exactly. `vulkaninfo --summary` enumerates the same hardware-backed
device. CPU upload/readback allocations report coherent flags 7 and their actual
host mappings return WC (`map_info=3`); shared renderer rings return CACHED.
This establishes the tested cached/WC path, not noncoherent Vulkan memory.

The ABI 31 implementation adds HOST3D blob creation, mapped host aperture leases,
borrowed user mappings, typed Venus ring submission and checked completion.
Canonical GEM/PRIME/VMA references retain the resource and producer context.
Retirement verifies renderer consumption, UNMAP, context detach and UNREF before
reusing its aperture extent. Uncertain completion retains/quarantines the extent.
The initial failing run exercised that conservative path: an overly strict
response check required ring fields which QEMU 10.2.2 does not echo. Validation
now uses the echoed context and globally unique fence ID of the owned request.

Mmap offsets reserve the rounded resource span and never wrap/recycle. Both
Linux's DRM core and Brook reject interior offset tokens; this prevents such an
offset from accidentally identifying another resource. Oversized mappings fail.
The shared raw probe verifies two-page CPU sharing, PRIME ownership, fork,
PROT_NONE/restoration, remap, close while mappings remain, aperture exhaustion
and subsequent reuse. Brook reserved all four 256 MiB extents in its 1 GiB BAR.

Enabling blobs also enables Mesa's persistent VirGL buffers. That path now
accepts the bundled PIPE_BUFFER creation command with checked geometry and
matching rounded size, attaches its resource to the context, and fences GL
submissions synchronously while the context owns host blobs. NOWAIT only reports
idle under that completion guarantee. Normal GL-only launches retain their
existing guest-backed path. Actual Mesa GLES tests pass RGB image readback and
a 16 KiB persistent coherent CPU-to-GPU buffer copy/readback.

### Host mapping-size correction

The pinned renderer could return a PIPE/opaque Vulkan allocation whose extent
differed from the blob's declared size. Before QEMU installs a MemoryRegion,
[the bounded renderer](../tools/venus-renderer-pkg/default.nix) now rejects that
mismatch and rolls back through the matching backend. The test compiles the
actual renderer map/unmap functions with instrumented backends: over/undersize,
unchanged output arguments, cleanup, retry, SHM/DMABUF and legacy behavior pass;
a mutant without the check fails. The Linux control also passes with this DSO,
and records its actual loaded path and SHA-256 from `/proc`.

`BROOK_GPU=venus` resolves this repository's renderer by default. Freeze it with
`BROOK_GPU_RENDERER` alongside QEMU, Mesa, GLVND, Vulkan loader and the host ICD.
This changes the experiment's library path, not host-installed drivers. The
Linux control manifest selects the same bounded renderer. The guest kernel must
not attempt to infer opaque allocation bounds from untrusted Vulkan commands.

### Evidence and limits

Accepted private run: `build/workloads/vulkan-blob-20261019/candidate2/`.
Linux control: `build/workloads/vulkan-blob-20261019/linux-bounded2/`.
Kernel SHA-256:
`8dfe72c42830036f1e2da4620dc32be19c71c28e5b262ead53bac5334eaac0fc`.
All 12 modules use ABI 31. The final run returns host blob resources, aperture
leases/bytes and quarantines to zero, balances legacy backing pages, passes heap
integrity, and completes checked shutdown plus four offline filesystem checks.
Existing documents, browser/editor settings and the Wine prefix are retained.

This is the initial pinned-Mesa path, not the complete Linux virtgpu ABI.
Guest-backed blobs, CROSS_DEVICE, debug names, nonzero poll masks, input fences,
DRM syncobj ioctls and full sync_file inspection/import remain unsupported.
Outgoing pollable FDs represent an already verified selected-timeline completion;
they are not general sync_file objects. The pinned Venus build simulates the
sync objects needed by these tests. GPU readback proves actual queue execution;
renderer ring0 acknowledgement by itself does not. Sizes above the current
32-bit resource-info limit are rejected. Native Wayland Vulkan WSI, triangle
rasterization and real Vulkan workloads are subsequent gates.

## Active goal

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
