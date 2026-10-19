# GPU resource ownership and retirement

## Current acceptance and known failures

The GPU transfer fix and scheduler startup repair are separate from resource
retirement. Private DRM resources now retire after their last owning reference
and verified GPU completion. Exported display sources now use that same path:
the compositor retains completed, owned captures rather than their raw resource
IDs. Uncertain completion still retains its source lease or quarantines the
resource. Legacy CPU-backed texture destruction remains open; resource IDs
remain non-recycling. The ABI 24 checkpoint below records the reclamation tests.

Two historical bounded guest probes distinguished different exhaustion paths:

- `scripts/workloads/gpu_resource_cycle_check.py --brook-guest` creates and
  closes tiny buffers, with one GEM live at a time. The recorded run completed
  4,095 of 4,500 allocations; allocation 4,096 failed with ENOMEM, and a new DRM
  context could not allocate either. Evidence is in
  `build/workloads/window-surface-20261018/resource-stress/`. This is the
  former cumulative host-resource-ID limit, not a simultaneous-GEM budget. The
  non-recycling namespace repair below replaces that limit.
- `scripts/workloads/gpu_prime_import_check.py --brook-guest` imports one
  128-byte resource 3,000 times. The baseline consumed 2,048 different GEM
  handles and then failed with ENOMEM. The candidate returns one handle across
  all 3,000 imports, separately exported PRIME FDs and self-import. It also
  checks reimport after GEM close and 256 concurrent close/import iterations.
  The candidate observed 29 imports with valid pinned metadata and 227 clean
  rejections after close, with no errors. Baseline/candidate evidence is under
  `build/workloads/gpu-prime-20261018/`.

Returning the existing GEM handle for the same memory object follows the
[Linux DRM userspace contract](https://www.kernel.org/doc/html/latest/gpu/drm-uapi.html#drm-ioctl-prime-fd-to-handle).
Userspace must count its own owners of that handle. Wayland now keeps a shared
import entry per GEM handle and closes it only when its last `wl_buffer` owner
is destroyed. Without that change, destroying A1 could close A2's shared
handle, permit B to reuse its number, and let A2's later destruction close B.

`tools/waylandd-pkg/dmabuf-import-test.c` includes the production server, mocks
DRM and allocation failures, and creates real Wayland buffer resources. It
checks alias ownership, numeric handle reuse after the final close, resource
info failure, cache allocation failure and client-disconnect cleanup. It runs
alongside the package's existing tests. The `wl_resource_create` failure unwind
is checked by inspection, not an injected failure in this test.

The kernel import path pins the PRIME FD while reading its capability metadata.
Import/export copyout failure removes newly inserted handles or FDs, preserving
pre-existing handles. Those copyout rollback branches have not been fault-injected.
The subsequent ownership conversion uses shared resource identity for duplicate
lookup and retains backing/context ownership through PRIME capabilities and VMAs;
its bounded exporter-close check is described below.

## Combined application checkpoint

The paired kernel and Wayland server were checked in session
`da1dc8e279294abb8fa4580d490ec954`, using eight CPUs, 4 GiB and muted EGL
hardware GL. Kernel SHA-256:
`c55a87ce3477c395d244a526421143757224d4ce4d3bbd6ebad016aec7f7b87a`.
The server came from
`/nix/store/7w3pazv6kn7c5djf8b0bpcmd5w87g25p-waylandd-brook-0.1-brook`;
the guest executable hash matched the built package.

Quake GL3 at 1280×720 transitioned from its startup demo to `map base1`, survived
OpenTTD opening and closing, then exited normally. Gallery rendered its own
content in Quake's reused window slot. Frames were inspected; the post-OpenTTD
Quake frame was paused, so this does not establish resumed playback performance.
All three launch lineages drained and final audio streams were empty. The host
sink was disabled; no audible timing claim is made. The package's four hosted
Wayland suites, all 45 kernel host suites and supported compiled entry/startup
audits passed.

The baseline, candidate and integrated VMs have owned launcher exit zero,
completed checked shutdown, offline data/Nix/FAT checks returning zero, and nine
unchanged UID1000/GID100 document fixtures. See
`build/workloads/gpu-prime-20261018/acceptance-audit.json`.
This is one controlled Quake/OpenTTD cycle; repeated Quake/STK acceptance and
host resource reclamation remain open.

The subsequent native review (`c9adad6cc3624730aabf0530cf34d30f`, same kernel,
eight CPUs and 4 GiB) did not pass mixed-application acceptance. Ian reported
STK crashing and Chromium never finishing loading. Serial output records
physical-page exhaustion, failed 64 MiB GPU backing allocations and user page
faults. Chromium and STK had exited by the later process census. This does not
establish which allocations caused the peak, or that host ID exhaustion caused
these failures. A captured Firefox surface was blank and Gallery reported a
truncated NAR download; their relationship to the exhaustion is unproven.

After handback, Firefox and Gallery launch lineages drained and audio streams
were empty. Checked shutdown completed and QEMU's supervisor recorded exit zero;
the user-owned outer launcher's exit was not independently observed. Offline
data/Nix/FAT checks passed and nine document hashes and UID1000/GID100 ownership
were unchanged. Private evidence is under
`build/workloads/prime-review-failure-20261019/`. Retain this as failed workload
acceptance despite the clean shutdown and filesystem checks.

## Isolated memory attribution

Separate eight-CPU, 4 GiB hardware-GL boots used identical starting packages and
profiles, the validated mmap-token kernel and silent host audio. These measured
normal-launcher startup, not gameplay or video playback. STK remained on its
loading screen when the configured 1,536 MiB free-memory floor triggered close;
Chromium showed a blank new tab. Rolling diagnostic snapshots reported:

| Guest used memory (MiB) | Baseline | Before close | After full lineage exit |
|---|---:|---:|---:|
| STK | 215.0 | 2,694.8 | 2,481.1 |
| Chromium | 215.0 | 928.7 | 291.3 |

STK reached the sampled floor after about 21 seconds. Its user-address-space
census accounted for only 221.6 MiB near close, and returned to 18.9 MiB after
close. Kernel heap use also returned near baseline. These measurements alone
did not distinguish DRM allocations from caches or other kernel memory.

The subsequent diagnostic kernel adds `mem drm`, also available through
`vm_stats(gpu_details=true)`. It counts successful kernel backing-mapping
allocations, releases at final DRM-FD teardown, mappings discarded at GEM close,
and failed allocation attempts. Counts are cumulative and independently sampled;
use quiescent endpoints for conservation checks. Releasing a kernel mapping does
not prove physical-page release while userspace mappings still pin its pages.
The counters exclude host VRAM, driver upload buffers and compositor allocations.
Those historical GEM-close discard fields describe the diagnostic kernel before
shared ownership. The current wire fields are `gem_close_backing_aliases` and
`gem_close_backing_alias_page_bytes`: they count alias-close events, can count
shared backing repeatedly, and must not be interpreted as retained bytes. Current
quiescent retained backing is allocated minus released; separate quarantine
counters identify permanently retained resources and backing. `failed_retirements`
excludes intentional exported-source quarantine.
GEM-close discards are already part of allocated-minus-released; do not subtract
them a second time. Older guests report `gpu_backing.supported=false`.

On that diagnostic kernel, the completed tiny mapping/fork probe verified exact deltas of three allocations
(12,288 bytes), two releases (8,192 bytes), one GEM-close discard (4,096 bytes)
and no allocation failures. The assertion waits for the full process lineage
and output EOF, because the probe prints its result before its final FD closes.

In the repeated STK run, all processes exited normally but **2,285,068,288 bytes
above baseline** remained in backing mappings discarded by GEM close. This is
2.13 GiB, accounting for 96.5% of the 2,367,795,200-byte retained guest-memory
growth. No allocation failure occurred in the bounded diagnostic run. This
identifies the dominant retention in that run; the diagnostic does not repair
it. Freeing those pages immediately at GEM close would violate outstanding
mapping/export/GPU ownership. Implement the shared ownership and completion
rules below before reclaiming them.

Evidence: `build/workloads/memory-isolated-20261019/acceptance-audit.json` and
`stk-diagnostics/` beneath that directory. Each run has checked shutdown, owned
launcher exit zero, offline data/Nix/FAT checks returning zero and nine unchanged
document fixtures. The diagnostic build passed 46 host suites and supported
compiled entry/startup audits; the bridge run completed 254 tests with five skips.

Existing disk images need both the rebuilt kernel and rebuilt `waylandd`:
the kernel now returns shared handles, and the server counts their buffer owners.
The prepared private user review preserves the prior user packages/profiles;
start it from the repository root with:

```sh
scripts/run_review.sh build/reviews/prime-import-20261018
```

This entry point creates the review's host temporary directory before Nix
starts, then delegates to its frozen `launch.sh` configuration. It does not
prepare images or rebuild artifacts. Set `BROOK_AUDIODEV=none` to mute the host
sink. Copyrighted game assets stay in ignored private images and must not be
committed or redistributed.

## Required ownership conversion

### Stable mmap offsets

MAP offsets now come from a global monotonic page-aligned allocator, stored in
the live GEM incarnation. An offset cannot resolve a reused GEM slot or a
different DRM file. Repeated MAP on the same live GEM returns its issued offset.
The allocator saturates rather than wrapping; it does not recycle host IDs.

The old kernel reproduced slot/file aliasing in session
`3a02d8cfe57349568324b496609c7f1a`: all three MAPs returned `0x100000000`.
Candidate session `7344a308ce474cb983a90a2e5d773966`, kernel SHA-256
`e24878bbb930b5509435297e1f0f479f0e0572e04744754545dad408a2ba1d0f`,
returned distinct offsets and rejected stale offsets. Existing mappings retained
their contents through GEM close, final DRM descriptor close, fork sharing and
parent unmap while the child retained its mapping. The Python probe explicitly
uses `trackfd=False`; otherwise Python's hidden duplicated descriptor would
prevent testing final DRM descriptor close.

The candidate also passed four byte-transfer cases and the 3,000-import PRIME
probe, including 256 concurrent close/import attempts (14 pinned imports, 242
clean rejections, no errors). Both runs have complete tracked command output,
drained job lineages, checked shutdown, owned launcher exit zero, offline
data/Nix/FAT checks returning zero and nine unchanged document fixtures. See
`build/workloads/drm-map-token-20261019/acceptance-audit.json`. All 46 kernel
host suites and supported compiled entry/startup audits passed.

The offset repair established identity only. The subsequent ownership conversion
below additionally pins resources during mmap preparation and shares PRIME backing.

### Shared resources and mappings

One shared resource object now owns geometry, backing allocation and a small GPU
context lease. GEM handles, PRIME capabilities, imported handles and device
mapping backings retain it through the existing `MappingBacking` reference API.
The lease is separate from the DRM file and GEM table, avoiding a resource-to-file
ownership cycle. Imported transfers use the resource's original GPU context.

Creation allocates owning metadata before requesting a host resource. Backing
attachment is checked, and creation/copyout failure drops the new GEM reference.
Ambiguous attachment keeps its backing until verified retirement or quarantine.
Failed driver creation after host allocation can still retain a host resource;
this path has not been fault-injected or made reclaimable.

The mmap path acquires the resource reference under `DrmCtx::mappingLock` before
installing page pins. `DeviceMappingBacking` carries it through preparation,
fork, splitting and unmapping. Final release only queues work; the retirement
worker performs GPU waits and page release outside mapping and FD cleanup locks.

The ABI-22 candidate, kernel SHA-256
`fbd205b0bbe55b34d0aa327ec184efbe2a5c53ab7a3a88c838158fe9ab16b19a`,
passed the stale-offset/fork probe, four byte-exact transfer cases, 3,000 imports
using one GEM handle, and 256 close/import races (18 pinned imports, 238 clean
rejections). A new `gpu_prime_lifetime_check.py` verifies shared mapped contents
and writes, exporter/GEM/PRIME-FD close, host upload/readback after exporter close,
remaining VMAs after all descriptor/handle closure, and fork sharing. It also
checks presentation rejects unowned or unexported resources and oversized geometry.

Protocol session `5b7cb0b8853d47a0bb0bdaffe0c2d0f1` allocated seven backing
mappings (28,672 bytes), reclaimed five (20,480 bytes), and intentionally
quarantined two exported sources (8,192 bytes), with no allocation or retirement
failure. Every job exited zero with complete output and no remaining lineage.
Checked shutdown, owned launcher exit zero, offline data/Nix/FAT checks and all
nine document hashes/UID1000/GID100 ownership passed. See
`build/workloads/drm-ownership-20261019/protocol-v2/`. The candidate passed all
47 host suites and supported compiled entry/startup audits. Failure-phase host
tests cover descriptor identity, response bounds/fence identity and stopping at
every failed retirement phase; real-device timeout/unref fault injection remains
open.

### STK memory recovery checkpoint

The same candidate ran STK on eight CPUs, 4 GiB, muted EGL hardware GL and
private copies of the same prepared image. Session
`6cbba9591de149588a0a0da8dea55eff` reached the main menu in the captured frame.
The free-memory safety floor still triggered after about 20 seconds, so gameplay
and mixed-app acceptance remain open.

At the quiescent post-close snapshot, all launch tasks/windows had drained and
all audio streams were gone. Of 2,297,008,128 bytes of allocated DRM backing,
2,289,324,032 were reclaimed. The remaining 7,684,096 bytes were exactly the
four quarantined exported buffers; there were zero failed allocations or
retirements. Guest memory fell from 2,550.6 MiB before close to 307.9 MiB after
close, compared with 2,479.5 MiB retained by the earlier diagnostic kernel.
This establishes private-backing cleanup, not a reduction in STK's live peak or
complete host/compositor-resource reclamation.

The owned launcher exited zero after checked shutdown; data/Nix/FAT checks
passed and nine document hashes and ownership were unchanged. Evidence is in
`build/workloads/drm-ownership-20261019/stk/`. Exported-source capture, context
slot recovery and safe ID recycling are still required before repeated-launch
acceptance can be completed.

A second cold run (`c1e65e30479f40408e95d80702906e48`, same configuration and
kernel) closed STK and then launched Chromium without restarting the VM.
Memory returned to 306.1 MiB after STK. Chromium rendered the local fixture in
the reused window slot, delivered its JavaScript readiness report and decoded
the 640×360 clip's first frame. The captured page showed its own content with
no stale STK surface. The clip remained paused; this is not playback/audio or
public-site acceptance. Guest use was 962.3 MiB with Chromium open and 331.4 MiB
after its full launch lineage drained. The remaining 5,763,072 backing bytes
were exactly three quarantined STK exports, with no allocation or retirement
failures. Both apps' lineages drained, audio was empty, and checked shutdown,
owned launcher exit zero, offline filesystem checks and nine document fixtures
passed. See `build/workloads/drm-ownership-20261019/stk-recovery/acceptance-audit.json`.

### Subsequent mixed-application review failed

Native review `17a539b49090473980023e6fe524ec2f`, kernel `fbd205b0…`, did
not pass acceptance. Chromium was open before Quake; both slowed down, and
Quake improved after Chromium was minimized. After Quake closed, STK crashed
and Firefox did not finish starting. Serial logs show zero free physical pages,
failed process-stack and DRM backing allocations, and user faults. The Quake
wrapper selected its default GL1 renderer in this run; it was not a GL3 repeat.

After STK exited, allocated-minus-released DRM backing was 31,567,872 bytes,
exactly the ten quarantined exported resources, with zero failed retirements.
The private-backing cleanup is still observed, but it does not bound live
application memory demand. The subsequent 1.5-second capture reported 914
samples, 5,715 switches and no producer drops; it was taken after exhaustion and
cannot establish the healthy concurrent-render bottleneck. Its CPU coverage is
also incomplete: the startup log shows a timed-out CPU6 followed by CPU7 online,
and the profiler iterates online cardinality as an index bound, omitting CPU7.
The same sparse-index error affects TLB invalidation and PMM retirement barriers.
The separate startup safety defect is addressed by the [dense-prefix admission
repair](smp-startup.md); fresh mixed-application acceptance remains open.

After handback, all recorded application launch lineages drained, audio streams
were empty, checked shutdown completed, QEMU's supervisor recorded exit zero,
and offline data/Nix/FAT checks plus nine document hashes/ownership passed.
The user-owned outer launcher's wait result was not independently observed.
Failure disks remain private under `build/reviews/gpu-ownership-20261019`;
evidence is `build/workloads/drm-ownership-20261019/native-review-failure/`.
Main remains unchanged.

## Presentation completion

The current `WM_PRESENT_GRES` stores a raw ID for later compositor sampling,
while Wayland immediately sends `wl_buffer.release`. A retained resource
reference alone does not make that valid: the client can overwrite a still-live
buffer once it receives release.

The legacy syscall now resolves a held GEM capability, requires that it has been
exported, checks geometry and pins it through publication. Exported resources
remain quarantined after their last reference, preserving their context namespace
and backing while a window might retain the numeric ID. This is deliberately
conservative and can exhaust the 16 context slots over repeated app launches.
It does not solve source overwrite after `wl_buffer.release`.

Complete presentation through an owned capture. Prefer a synchronous
capture request serviced by the compositor thread, which owns `g_winTex`:

1. Acquire the source resource reference and window incarnation.
2. Queue an owned request with explicit completion and cancellation state.
3. Process captures before visibility/minimization filtering.
4. Copy into retained compositor-owned storage, complete the producer and
   compositor-context fences, then acknowledge capture.
5. Return success to Wayland, which may now release the source buffer.

Serialize captures per window initially. Revalidate incarnation after waits;
resize, destruction, disconnect and cancellation must complete every waiter.
Retain captured pixels until replacement/destruction, without allocating a new
host texture every frame. CPU VFB publication must replace captured GL content.
Take snapshot references under window locks and release them outside those
locks. GPU waits must not hold window, GEM-table or address-space locks.

## Host retirement and ID allocation

Private-resource final reference release now enqueues retirement rather than
issuing GPU commands from mapping or FD callbacks. The worker:

1. Complete outstanding work on relevant producer/compositor contexts.
2. Detach context and backing associations and verify host resource unref.
3. Frees guest backing only after all steps succeed; failures retain the resource.

IDs are not recycled yet. Control completions validate descriptor identity,
response bounds and requested fence identity. Timeout or invalid completion
quarantines the control queue, preventing subsequent responses from authorizing
retirement. General device recovery and all existing staging-buffer failure
paths still need work; this is not a full DMA-timeout recovery implementation.

Retire compositor textures as well as application resources. The CPU reader
epoch protects existing CPU readers; it does not establish host GPU completion.
Retirement fences both the producer and compositor contexts through a helper
that runs under the existing `GpuCmdGuard` without recursively acquiring it.

`ResourceCreate3D` and native `BufferUpload` now share a non-recycling namespace
allocator; their former independent upward/downward counters could collide.
Recycling remains separate work. Keep an old context reserved while exported
objects survive, and remove or incarnation-tag the numeric
`g_compExternalAttached` cache before reuse. Synchronize the module ABI/version
when extending GPU operations.

## Non-recycling resource namespace

The explicit 1280×720 GL3 demo reproduced ID exhaustion alongside Chromium in
session `43758751355440cc913157e830092e32`. The log records
`app resource ids exhausted ctx=256 pid=99 allocations=4096`, repeated failed
buffer creation and then a null-pointer fault in Mesa's `dri2_query_image`.
This is distinct from the earlier physical-memory exhaustion. The attempted
visible/minimized comparison is invalid because Quake exited during it;
later profiles captured Chromium alone. See
`build/workloads/mixed-render-20261019/4g-configure/` and the separate
[configure-handshake investigation](wayland-configure.md).

`resource_ids.h` supplies the production namespace. Application IDs occupy
`0x10000..0x7fffffff`, remain positive through the signed module ABI, and encode
one of sixteen context slots in their low bits. Each slot's sequence increases
monotonically across context destruction and failed host attempts. A successful
new context records a sequence watermark, rejecting earlier incarnations' IDs.
Both app creation and legacy upload allocate through the same instance under
`GpuCmdGuard`. No ID is reused; arithmetic saturates before wraparound. The
protocol's finite signed-ID space still bounds the lifetime number of IDs.

Namespace validation establishes a tag and issued interval, not that creation
succeeded or that a resource remains live. Canonical DRM references enforce
resource ownership and lifetime. The exported-resource quarantine and its
sixteen-context limit remain unchanged. This repair does not establish owned
presentation captures or enable exported-resource reclamation.

Dynamic compositor textures use `0x1000..0xffff`, excluding both app IDs and
fixed boot/self-test IDs (including 64/65 and 200–213). This allocator also
saturates without recycling. Compositor texture retirement remains unfinished.

The production-helper host test models more than 4,096 mixed allocation callers,
all sixteen context tags, burned failed attempts, context incarnation changes
and boundary saturation. Five isolated mutation controls reject wraparound,
counter reset, missing tags, saturation failure and fixed-resource overlap.
The full 49 host suites and supported compiled entry/startup audits pass.
These helper tests do not execute the legacy upload driver entry point.

The guest boundary probe `gpu_resource_ids_check.py` creates/closes 8,192 tiny
private resources through one render FD and performs host roundtrips immediately
before, across and after the former limit. In candidate session
`702795d0531d400a841518be4b781a8c` it completed in 3.542 seconds with 8,192 unique
IDs. Allocated-minus-released DRM backing returned to zero; no allocation or
retirement failed. Stale mmap tokens, retained mappings/fork, PRIME exporter
close and byte-offset transfers also passed. The PRIME test intentionally
retained one exported 4-KiB quarantine. Evidence is under
`build/workloads/gpu-resource-ids-20261019/`; longer app acceptance and the
checked endpoint are separate from these probe results.

That candidate subsequently ran Chromium local video alongside Quake GL3 at
1280×720. Both exact launch lineages remained live across the paired profiles
and thirteen liveness/resource snapshots spanning 130 seconds. No allocation
failure, retirement failure or user fault occurred. However, the final
`mixed-stable.png` shows Quake **PAUSED** after the focus-changing experiment.
This verifies liveness, not continuous demo playback; do not use these profiles
as a controlled moving-demo comparison. Rerun without focus changes and verify
actual demo progression. The short samples contain substantial GPU submission
wait and lock contention, but do not establish a matched-scene FPS benefit.

Both application lineages then drained, audio streams became empty, checked
guest sync/shutdown and owned launcher exit 0 completed, all three offline
filesystem checks returned 0 and all nine document fixtures were unchanged.
The complete endpoint is `candidate/acceptance-audit.json`. Final DRM retained
backing was **328,093,696 bytes**, exactly its intentional exported quarantine,
with zero failed retirements. This is still material retained memory, not a
return to the initial memory baseline. The private 8,192-allocation test alone
returned its retained backing to zero. Owned compositor captures and exported
retirement remain necessary; increasing VM memory will not fix that retention.

## Uninterrupted demo and focus-pause correction

The next fresh 8-CPU/4-GiB hardware-GL guest, session
`957e85593c9d49ac9083079f066857a0`, ran Quake GL3 at 1280×720 without input or
window-focus changes. Seven spaced frames over 161 seconds show different
rooms, weapons and world states without a pause overlay. Serial demo-boundary
messages advanced and the resource counter passed 8,192. The captures are in
`build/workloads/gpu-unpaused-20261019/baseline/uninterrupted/`. Readback briefly
perturbs the guest; this is progression evidence, not a matched-scene FPS test
or proof that no transient artifact occurred between captures.

The pinned Yamagi 8.60 SDL3 input implementation explains the earlier pause:
`vid_pauseonfocuslost=0` pauses on focus loss but does not automatically unpause
on focus gain; `1` disables focus-loss pausing, while `2` automatically resumes.
The opt-in `yquake2-benchmark.cfg` now sets `vid_pauseonfocuslost 1` and
`paused 0`. Normal launch wrappers are unchanged. This is an archived game
setting, so use a private acceptance profile when running the benchmark.

After the first game lineage drained, the revised benchmark was installed by
a tracked guest command and a second game was launched. Chromium local video
then played while focus changed between the applications. Boot-guarded images
`focus-check/game-1.png`, `game-2.png` and `game-3.png` show the game advancing
without the former pause overlay; both audio stream completion counters
advanced with the host sink muted. Audible quality and OST acceptance remain
unverified. `game-3.png` contains a questionable large brown polygon; additional
frames were saved, but it has not been isolated as corruption versus scene
geometry. This does not close full graphics acceptance.

During the uninterrupted interval, retained DRM backing grew from 209,932,288
to 409,391,104 bytes. The complete two-launch/browser run ended with
825,753,600 bytes retained, exactly its exported quarantine. No allocation or
retirement failed. All three application lineages drained, audio streams were
empty, checked sync/shutdown and owned launcher exit 0 completed, all three
offline filesystem checks returned 0 and all nine document fixtures were
preserved. Evidence: `baseline/acceptance-audit.json` beneath the directory above.
This strengthens the case for fixing exported presentation ownership rather
than treating a larger VM as the memory repair.

### Capture implementation constraints

The source review with Astra identified requirements beyond retaining a GEM:

- The current external BLIT is unfenced and caches its source attachment.
  Owned capture needs a producer fence, source attachment, BLIT, compositor
  completion fence and verified source detachment before successful return.
  Remove indefinite external-source attachment caching from this path.
- A retained destination must own its storage. Current `CreateTexture` attaches
  VFB guest pages and `DestroyTexture` only frees a metadata slot. Add host-only
  capture textures with verified teardown and creation-failure rollback; also
  preserve the backing lifetime of existing CPU-upload textures. A CPU reader
  epoch alone does not establish GPU DMA completion.
- Service a bounded capture queue on the compositor thread, before visibility
  filtering. Pin the canonical source and its actual producer context, not the
  importing Wayland context. Track window creation identity and publication
  generation across resize, slot reuse and newer SHM commits.
- Give queued work independent heap ownership and protect syscall wait/unwind
  through the existing kernel-operation mechanism. Do not wait for the GPU
  under window, GEM, address-space or queue locks. Complete each waiter once.
- Cancelling publication does not cancel an in-flight GPU read. Keep its source
  lease through verified completion; ambiguous completion requires quarantine.
  Wayland currently releases even after presentation failure and must distinguish
  a safely discarded frame from an ambiguous read before sending release.

Keep exported quarantine until all deferred numeric-source paths are replaced
and these completion/failure rules are tested. Private-retirement batching may
save fence round trips later, but it cannot repair the exported-buffer contract.

## Acceptance gates for reclamation

- More than 4,095 create/close iterations with bounded live objects and backing,
  including allocation after context recreation and mixed upload/create paths.
- Exporter close/death with PRIME FDs, in-flight SCM_RIGHTS, imported GEMs and
  mappings still alive; cross-imports must not create permanent cycles.
- Concurrent mmap/close, stale map tokens, fork/split/unmap ordering.
- Present A, receive buffer release, overwrite or destroy the source immediately:
  retained output must remain A. Repeat while minimized and across resize,
  destruction, slot reuse and capture cancellation.
- Creation, backing attachment, copyout, fence and unref failures; uncertain
  completion must not free backing or make an ID reusable.
- Real Quake/OpenTTD/STK transitions plus clean/forced app exits, checked shutdown,
  offline filesystem checks and unchanged profile/document fixtures.

The PRIME identity repair is a prerequisite. It does not complete these gates
or establish that the intermittent Quake/STK crashes are fixed.

## Compositor-owned capture prerequisites

The ABI 23 driver adds host-only destination creation, fenced external capture,
and verified destination teardown. This is a prerequisite, not the Wayland
publication change: existing exported-resource quarantine remains enabled and
no application-buffer reclamation or mixed-application acceptance is claimed.

Capture currently supports opaque XRGB only. Alpha creation is rejected before
sending commands. The caller must hold the canonical source/producer lease and
validate its dimensions. Calls run on the compositor thread outside frame
assembly. Capture orders producer fence, compositor source attachment, GPU
copy, compositor fence and source detachment. Only complete success authorizes
source release. Any uncertain operation quarantines the destination; callers
must retain the source lease. Owned teardown unbinds and destroys the sampler
view with a fence before detachment and unref. The legacy destruction API
refuses owned textures. Sampler-view IDs saturate instead of wrapping.

A pre-publication hardware self-test gates registration of both GPU compositor
and application operations. Failure retains uncertain objects and leaves the
already-registered 2D display driver active. It repeats three times: create and capture
a green source, overwrite it blue and independently read back the blue source,
verify retained green before and after source retirement, clear and verify the
scanout black, draw the captured view, verify green, then destroy the sampled
view and retire the test resources. Pages are freed only after verified
retirement and context destruction. Source surface objects remain until that
context destruction; the earlier post-retirement readback does not prove that
all host GL references have already vanished.

The initial test exposed the pinned renderer's `COPY_TRANSFER3D` requirement
that even a read-from-host source has guest backing. A host-only texture cannot
use that route. The corrected test blits into a backed staging texture and
transfers that texture to the guest. This staging is only for verification;
capture itself remains GPU-to-GPU.

Host tests exercise short-circuit failure at each capture and teardown phase.
Mutation controls reject omitted fences/detaches/unref and ignored copy failure.
They do not inject real queue timeouts or host GPU failures. Alpha, publication
cancellation, minimized windows, window-slot reuse and early Wayland buffer
release still require integration tests before removing exported quarantine.

Final paired-build session `4975e87dc5164ce8a8937d888c43c06d` passed the
three-cycle hardware test, 8,192 private resource creations/closures (net backing
zero), mmap lifetime checks, PRIME lifetime checks and full/partial/independent
buffer transfers. It used eight CPUs, 4 GiB, hardware EGL GL and muted audio.
All 50 host suites and the entry/startup audits passed; seven negative mutation
controls were detected. Command output completed with no gaps and no remaining
command tasks. Checked shutdown returned launcher/QEMU exit zero, all three
offline filesystem checks passed, and nine UID/GID/document hashes were
preserved. Evidence: `build/workloads/gpu-owned-capture-20261019/verified/acceptance-audit.json`.
The final failed-test 2D fallback registration branch is inspected, not exercised
by a real injected GPU failure. Main and mixed-app acceptance remain unchanged.

## Capture queue and publication

`WM_PRESENT_GRES` now retains the canonical DRM source and producer through a
bounded compositor request. The caller keeps kernel-operation cancellation
protection while waiting; the request owns an independent mapping-backing
reference. No GEM-table or window lock is held during GPU commands. The
compositor drains requests before visibility filtering and frame assembly,
including requests for minimized windows.

Window reservations carry both the slot incarnation and a publication
reservation generation. Reserving another capture leaves the last published
frame valid. SHM publication, resize and destruction invalidate older pending
work. A completed copy publishes only if its reservation remains current;
rendering selects the matching incarnation, content kind and published
generation. Dirty acknowledgement cannot clear a newer SHM publication.

Normal operation uses a published texture and a staging texture per window.
Only staging receives the next copy, preserving the published image during
cancellation or allocation failure. After successful publication, compatible
old published textures become staging. Dead/reused/SHM window slots retire
owned textures even when minimized. Destination retirement failure leaves the
driver's quarantined metadata intact.

The queue is limited to 64 live/uncertain requests. Queued cancellation removes
the request before any GPU read. Running cancellation suppresses publication
and waits for the bounded driver operation to finish; it does not return a
reuse-authorizing timeout while the GPU may still read. Uncertain completion
retains both the source lease and queue capacity until device reset. Wayland
ends that client connection without sending `wl_buffer.release`. It preserves
`errno` immediately after the syscall so logging cannot conceal an uncertain
libc-style `-1/EIO` result. Published and safely discarded copies may release.

The threaded host test includes the production compositor queue and checks
bounded staging reuse, allocation failure preserving the prior frame, queue
capacity, SHM/slot-reuse races, running cancellation, queued timeout and uncertain
source retention. Window tests exercise real publication entry points and dirty
acknowledgement. The Wayland package's capture-release test inspects real wire
events, including a logging failure that changes errno. These host tests do not
replace hardware fault injection or simultaneous-application acceptance.

`scripts/workloads/gpu_capture_lifetime_check.py` is the guest half of an
isolated framebuffer test. Its host counterpart must inspect and acknowledge
each named phase. It independently reads back the overwritten source, closes
all source mappings/GEM/PRIME/render FDs before observation, switches GL→SHM→GL,
and captures a new frame while minimized. Evidence must include the host pixel
checks; a guest completion marker alone cannot establish correct presentation.

Exported resource reclamation remains disabled in this checkpoint. Legacy
CPU-backed texture retirement and mixed-application capacity remain open.

Hardware session `f95f102b828d4ca894dc2fcb9280d820` used the paired ABI 23 kernel
and tested Wayland package on eight CPUs and 4 GiB with muted hardware EGL GL.
Two complete pixel sequences passed; the second independently verified the
producer overwrite and reused the former window slot. Quake II GL3 then
advanced through seven spaced captures over 163 seconds, crossed 8,192 resource
allocations and two demo boundaries, and showed no pause overlay or visible
corruption in those samples. This does not establish between-sample correctness,
framerate improvement or audible quality.

Quarantined backing stayed at 3,784,704 bytes in all seven demo observations.
For context, the earlier focused, unpaused demo run recorded growth from
165,888,000 to 364,953,600 bytes over 161 seconds. No paired FPS comparison is
claimed. Final backing after application closure and the ownership probes was
11,161,600 bytes, exactly the remaining exported quarantine; no private backing
remained. Reclamation of that exported backing is still a separate step.

All 51 host suites passed, followed by the strengthened targeted queue/window
tests. All six Wayland package checks passed. Eighteen guest probe/advance jobs
completed with full output and zero live tasks; an initial Python-path error
failed before process creation and is recorded separately. The Quake launch
lineage drained, audio streams emptied, owned launcher/QEMU exited zero,
all three offline filesystem checks passed, and nine documents retained their
hashes and UID/GID. Evidence:
`build/workloads/gpu-capture-queue-20261019/candidate/acceptance-audit.json`.
STK/browser simultaneous-use acceptance and main approval remain open.

## Exported backing retirement (ABI 24)

ABI 24 removes the unused, unleased `BlitExternalToTexture` operation and its
attachment cache. With window presentation using owned captures, the retirement
worker can retire exported resources after the final GEM, PRIME, VMA and capture
reference disappears. The sticky export flag continues to authorize numeric
presentation; it no longer imposes permanent quarantine. A failed retirement
still quarantines backing and increments the failure counter. Uncertain captures
retain their source reference and cannot enter last-reference retirement.

Session `d97e7ead9e43436ca62387483d811c12` used eight CPUs, 4 GiB and muted
hardware EGL GL. The retained-image test independently verified producer
overwrite, closed every source reference, and checked zero outstanding backing
before inspecting each published color. GL→SHM→GL and capture while minimized
passed. The 8,192-resource, mmap/fork, PRIME lifetime and buffer-transfer probes
each returned to zero outstanding backing with no quarantine or retirement
failure.

Quake GL3 and Chromium's local video ran together; sampled images showed both
rendering. Quake closed normally, followed by STK opening its main menu alongside
Chromium. STK used about 2.1 GiB of live backing; closing it returned the DRM
backing counters to equality, and Gallery subsequently displayed its own grid.
This is menu/normal-exit coverage, not a STK race or a paired FPS benchmark.
After all applications closed, all 20,014 backing mappings were released:
2,586,910,720 bytes allocated and released, with zero quarantine, failed
allocations or failed retirements. These counters measure guest backing;
host GL objects may retain host storage until their objects or contexts retire.

Firefox's local page reported readiness but its hardware-mode window remained
transparent. This is not accepted as rendered video or audio playback. Static
inspection found that dmabuf subsurfaces skip capture and release their buffer
without retaining pixels, unlike SHM subsurfaces. At this checkpoint, a trace
associating Firefox's specific committing surface with its parent was missing;
the diagnostic follow-up below supplies it. The test image
also predates the promoted Firefox audio package. Neither issue is resolved by
the backing-retirement change.

All 51 host suites passed. Twelve guest commands completed with full output and
no live descendants; all five application launch lineages drained and audio
streams emptied. Checked guest shutdown, owned launcher/QEMU exit zero, three
offline filesystem checks and nine unchanged document hashes/owners passed.
Evidence: `build/workloads/gpu-export-retirement-20261019/candidate/acceptance-audit.json`.
Broader application acceptance and main approval remain open.

## Hardware Firefox: retained surface-tree work

The diagnostic follow-up, session `58ba1b34b0914b1296fe60ce6020f7bb`, confirms
the association: Firefox's surface 33 is a dmabuf subsurface of root surface 38,
at offset `(26,23)`, with an 800×600 buffer. Its root app ID is `firefox`.
The first child commit precedes creation of the 852×652 root kernel window.
The root later shows only a transparent outline while the local page reports
media readiness. This proves the server skips Firefox's actual child content;
it does not prove that this is the only issue remaining in hardware Firefox.
Evidence is under `build/workloads/gpu-firefox-subsurface-20261019/candidate/`,
including `surface-association.log` and `firefox.png`.

The server reports this unsupported path once per surface. The diagnostic
does not change presentation, buffer release or application configuration.

The implementation direction is retained images per Wayland surface and GPU
composition into a retained root texture. The tree itself is not implemented;
the format prerequisite below is now verified:

- Add canonical resource-format metadata and format-aware owned capture.
  ARGB capture must preserve alpha; XRGB supplies alpha one. Reject unsupported
  formats before submitting commands. Do not infer source format from the
  destination or from the client's unchecked dmabuf declaration. ABI 25 supplies
  this prerequisite for BGRA/XRGB 2D sources.
- Add explicit premultiplied composition: `ONE, ONE_MINUS_SRC_ALPHA` for RGB
  and alpha, and scale both RGB and alpha by window opacity. Existing straight
  alpha chrome drawing keeps its separate mode. ABI 26 supplies the driver
  operation; scene ownership/publication and Wayland integration remain.
- A provider-owned scene capability binds to a window incarnation. Layers have
  generation-tagged identities and completed versions. Dmabuf updates retain
  canonical source leases through verified capture; SHM updates copy into owned
  upload storage. Closing the capability queues cleanup rather than waiting
  for the GPU under FD cleanup locks.
- Serialize layer updates and scene composition so active/staging textures stay
  bounded and a published or queued version cannot be overwritten. Composition
  validates and pins a complete bounded draw list, clears root staging to
  transparent, draws it, fences, then revalidates and atomically publishes.
  Failure preserves the previous complete root. Desktop repaint samples the
  root without revisiting clients or recomposing an unchanged tree.
- Wayland retains the tree semantics: committed versus cached state, child
  ordering, nested signed offsets, clipping, detach and destruction. Mixed
  SHM/dmabuf trees work in both directions. Root CPU repaint must not overwrite
  an active GPU scene. Offscreen work runs before desktop frame assembly and
  explicitly restores framebuffer, viewport, shader and blend state.

The first pixel fixture must include a transparent SHM root, offset dmabuf
child, overlapping translucent sibling, nested negative offsets, parent-only
repaint, GL↔SHM child transitions, source overwrite after release, detach, resize
and reused window slots. Test-only readback verifies pixels; thousands of
updates must retain bounded allocations. Real Firefox rendering and audio are
subsequent acceptance gates. A CPU readback loop or redirecting the child into
the entire root window would not satisfy this design.

## Format-aware capture prerequisite (ABI 25)

DRM now retains the creation target and virgl format in the canonical shared
resource. PRIME aliases preserve that metadata after the exporter and PRIME FD
close. Numeric window presentation accepts only 2D B8G8R8A8_UNORM and
B8G8R8X8_UNORM sources. Unsupported formats/targets reject before reservation or
GPU capture. The queue carries the canonical format through to the driver;
the BLIT explicitly encodes both source and destination formats.

Owned ARGB textures now preserve alpha. The prepublication hardware self-test
adds an ARGB cycle: capture half-alpha green (`0x80008000`), overwrite the
producer with independently verified blue/quarter alpha, retire the source,
and read back all four retained channels before and after retirement. Its
surface-clear views use the actual source format. This is capture correctness,
not premultiplied blending or transparent-window acceptance. The current
full-window queue deliberately continues to publish opaque XRGB destinations.

All 51 host suites and the compiled entry/startup audits passed. The production
queue test verifies ARGB format forwarding and unsupported-format rejection
without allocation or leasing. `gpu_capture_format_check.py` checks the same
admission boundary on imported resources after exporter closure, with valid
ARGB/XRGB textures, an unsupported float texture and a byte buffer. The
retained-pixel probe accepts `--format argb` as well as its XRGB default.

Hardware candidate `3144bc67c5a24d319aa7476dab0d372f` passed both visible
source-format sequences, including source overwrite/close, GL→SHM→GL and
minimized updates, with zero outstanding backing at every visible phase.
The imported-format probe passed all eight cases and returned to zero backing.
Quake GL3 at 1280×720 advanced in three inspected demo samples and closed
normally. That is a short rendering regression, not performance/music acceptance.
Review then corrected an XRGB clear-view declaration in the ARGB self-test;
the final driver was rebuilt and its corrected hardware self-test passed in
session `3fdab49e291c4f9bb57dd700d60df424`. That final driver also passed the
ARGB visible-pixel/minimized sequence and all imported-format cases. Its eight
guest commands drained with full output, and all seven DRM backing mappings
(65,536 bytes) were released with zero quarantine or failures. Both runs have
checked shutdown, owned launcher/QEMU exit zero, three clean offline filesystem
checks and nine preserved document hashes/owners. Final endpoint evidence:
`build/workloads/gpu-format-capture-20261019/verified/acceptance-audit.json`.

## Offscreen premultiplied composition prerequisite (ABI 26)

`ComposeOwnedLayers` accepts at most 64 retained owned images and writes a
distinct owned ARGB staging target. It validates the complete list before
changing vertex backing or submitting commands. The flattened descriptors
carry integer source crops, signed destination positions, scaling, root-relative
clips and opacity. Clipping retains rational UVs, so scaled/cropped images keep
their sampling positions. An empty list clears the target to transparent.
Source/target aliasing and malformed geometry reject without changing it.

The driver uses premultiplied source-over for both RGB and alpha, with opacity
applied to every channel. Render and desktop-state restoration each complete a
verified fence. Offscreen work has a dedicated vertex buffer; uncertain
completion disables its reuse and quarantines the target. The caller must keep
all input versions and their lifetimes on uncertainty. This API is currently a
kernel/driver prerequisite, not an exposed userspace scene interface.

Owned textures now have render-surface objects as well as sampler views.
Verified destruction unbinds framebuffer and sampler references, destroys both
objects, fences, detaches and unreferences. Object IDs share a non-wrapping
allocator. The new desktop draw operation selects a separate premultiplied
shader/blend state; existing straight-alpha drawing keeps its prior semantics.

Viewport half-dimensions use an exact integer-to-float-bit conversion for odd
and even sizes. Reusing the normalized UV fixed-point helper here would have
overflowed at ordinary display sizes; review caught this before hardware boot.
Host checks cover actual float bits for every dimension from 0 to 16,384,
10,000 randomized clipping/scaling cases against an independent pixel-center
reference, signed extremes and all three composition/restore failure phases.
All 52 host suites and supported compiled entry/startup audits passed.

Hardware session `d7212bb194804f5e801f7cae584f7396` passed a prepublication GPU
readback test using a 5×3 ARGB root and two captured gradient images. It checked
negative positioning, explicit clipping, scaling, both layer orders, partial
opacity, 64 layers, transparent clearing, rejection preserving the previous
image, premultiplied root drawing, switching back to the legacy opacity shader,
and fenced texture teardown. Production composition stays on the GPU; readback
is confined to verification.

The retained-source overwrite/close, GL→SHM→GL and minimized-update pixel probe
also passed with zero outstanding backing at every inspected phase. Quake GL3
at 1280×720 advanced through three inspected demo frames and exited normally.
This is a short rendering regression, not a performance/music or mixed-app soak
claim. Seven guest jobs drained with full output, the Quake launch lineage
drained, backing returned to zero without quarantine or failures, and audio
streams emptied. Checked shutdown, owned launcher/QEMU exit zero, three offline
filesystem checks and nine preserved document hashes/owners passed. Evidence:
`build/workloads/gpu-scene-20261019/candidate/acceptance-audit.json`.

Still required: verified SHM uploads into owned images; provider-owned scene
capabilities and generation-tagged layer versions; serialized updates and atomic
root publication; Wayland tree ordering, cached/synchronized state, detach and
disconnect cleanup; and real Firefox rendering/audio acceptance. Layer versions
selected by a queued composition must remain immutable; replacing a cached
child must not overwrite a currently committed version. Long-run bounded-memory
testing and injected hardware completion failures remain unverified.

## Temporary-backing owned uploads (ABI 27)

`UploadOwnedTexture` uploads tightly packed pixels from a page-aligned,
kernel-owned VMM snapshot into an owned staging image. The caller retains the
snapshot through the operation. Success requires backing attach, transfer,
completion fence, backing detach and a final fence. The resulting image has no
guest backing dependency, so the snapshot can be overwritten and freed.

The backing walker reports whether it actually submitted an attach. Unmapped
pages or excessive fragmentation detected during preflight reject without
commands or quarantine. Once an attach has been submitted, any failure is
uncertain: the target is quarantined and the caller must retain the snapshot.
Texture destruction rejects outstanding upload backing. No cleanup guesses
whether a failed attach/detach acknowledgement took effect.

The hosted capture test checks all five upload phases and short-circuit failures.
The hardware self-test uploads a 19×67 image spanning two pages, poisons the
original snapshot, frees it, allocates/poisons new pages, and checks every output
pixel. It repeats three uploads per destination for both ARGB and XRGB; arbitrary
input X bytes resolve to alpha one. A guarded mapping also exercises a real
partial IOV walk followed by an unmapped page, then verifies a valid upload to
the same destination succeeds. Guard-page allocators are exported to the driver
for that fixture. The mixed composition test now combines an uploaded SHM image
with a captured GPU image, reusing the SHM snapshot before rendering.

Hardware session `e19dbdfd6c12476d831589a86e5049c3` passed those tests and the
existing capture/composition gates. The physical-page reuse counter was **0/12**:
same-physical-page reuse is not established by this run. The deterministic
overwrite of the original snapshot before freeing is independently covered.
All 52 host suites and supported compiled entry/startup audits passed.

The retained-source pixel probe passed GL→SHM→GL and minimized updates after
the producer resources were closed. Quake GL3 at 1280×720 advanced through three
inspected demo frames and closed normally. Seven guest jobs drained with complete
output, and the Quake launch lineage drained. Tracked DRM backing was fully
released, with zero quarantine or allocation/retirement failures; audio streams
emptied. Checked shutdown, owned launcher/QEMU exit zero, three offline filesystem
checks, and nine preserved document hashes/owners passed. This is a short
regression, not an audio-quality, performance or mixed-app soak result. Evidence:
`build/workloads/gpu-owned-upload-20261019/candidate/acceptance-audit.json`.


The scene API must expose immutable versions rather than a single mutable image
per layer. A synchronized child can have committed A, cached B, and incoming C;
failure to create C must preserve A and B. Three textures can be necessary
temporarily. The shared 128-texture driver table therefore cannot promise 64
layers with arbitrary retained versions across several windows. Reserve and
report version/texture capacity, preserve existing images on exhaustion, and
reuse retired textures only after public and queued references disappear and
GPU completion is verified. Keep Wayland committed/cached state outside the
kernel, and pin exact image versions for each queued draw list.

Still required: scene descriptors, version lifetime/capacity management, owned
snapshot copying/admission, cancellation-safe queued work, root publication and
Wayland integration. This driver API alone does not make Firefox render.
Long-run allocation bounds and injected hardware failures remain unverified.

Separate follow-up: the older native `AppBufferUpload` path frees bounce pages
on partial failure even if backing attachment succeeded. It needs per-resource
verified retirement and a cleanup audit; this upload prerequisite does not
change that path. The normal DRM/PRIME image path uses its separate canonical
resource lifetime machinery.

## Immutable scene versions (integration in progress)

`gpu_scene_versions.h` now supplies the kernel-private image ownership table.
At checkpoint `31d00a28` it was exercised by the hosted `test_gpu_scene` suite
without live compositor or Wayland integration. The scene service described
below now uses it; Wayland adoption and Firefox acceptance remain incomplete.

Each reservation gets a fresh positive scene-scoped 63-bit ID. A verified upload
or capture can publish it; a failed incoming C leaves committed A and cached B
unchanged. Queued draws pin the exact versions in their copied draw list. Public
release removes lookup permission while existing pins preserve the allocation.
Only the last completed reference makes the texture eligible for reuse. Matching
idle allocations can be recycled without creating a new driver resource every
frame, but always receive a fresh version ID.

Reservations account for both slots and logical pixel bytes before GPU work.
The configured limits and current counts are observable through `Stats`; pixel
bytes include reservations, idle images and quarantine, not measured host VRAM
or driver overhead. A full table or byte budget rejects admission without
modifying existing versions. Eviction runs in two phases: mark an idle image,
drop the scene lock for driver retirement, then release budget only after
verified destruction. Its completion checks both slot and nonrepeating ticket.
Failed retirement quarantines the allocation and retains its budget.

Close rejects new reservations and pins, drops public ownership, and leaves
queued pins and running reservations intact. An upload completing after close
returns `Discarded` rather than `Published`, retaining the finished image for
worker cleanup. Uncertain GPU work cannot be recycled through close, release,
unpin or eviction. Its external snapshots/source leases must remain retained by
the job; metadata alone does not own those allocations.

The scene owner must serialize metadata operations and perform all GPU work
outside its lock. `GetPublished` only observes metadata; it does not pin an image
for asynchronous work. Public ownership is singular: Wayland must count or
coalesce references when committed and cached state hold the same version ID,
and call `Release` only after both are finished. Pin/unpin batches validate
atomically, including repeated IDs. A batch containing a quarantined version
fails without releasing any pins; cancellation must retain the whole job or
separately release verified healthy inputs.

Host coverage includes A/B/C preservation, out-of-order reservations, duplicate
pins, invalid batches, close during upload/eviction, stale completion after
reuse, eviction-ticket ABA, failed-retirement quarantine and 20,000 modeled
lifetime transitions. Six mutation controls exercise premature reuse, version
ID recycling, publication after close, uncertain-retirement reuse, stale
eviction completion and premature budget release. Sanitizers and the complete
52-suite host run are recorded under
`build/workloads/gpu-scene-versions-20261019/`.

The next integration must provide a close-on-exec scene descriptor with correct
dup/fork/SCM ownership; bounded kernel-owned SHM snapshots; capture through an
explicit DRM FD/GEM capability; copied/pinned draw lists; worker cancellation and
deferred cleanup; and atomic premultiplied root publication using the window's
incarnation/generation token. Closing an old scene must not erase newer SHM or a
replacement scene's content. Root textures belong to the compositor, independently
of the layer versions. After this, Wayland must preserve synchronized cached
state, committed state, tree order and detach/disconnect semantics, then pass
real Firefox rendering/audio and mixed-app lifetime tests. None of those
integration requirements is established by these metadata tests.


## Scene descriptor service (Wayland adoption pending)

The kernel now provides `SCENE_CREATE` (`0xB06`) and `SCENE_CTL` (`0xB07`), with
versioned wire structures in `gpu_scene_abi.h`. Creation requires an owned WM
window and returns a close-on-exec FD. Control operations report limits/stats,
upload a SHM snapshot, capture an explicit DRM FD/GEM capability, present a
copied ordered version list, and release an image version. Upload/capture return
a positive version ID directly, avoiding a post-completion output-copy rollback.
Backend texture handles are never exposed through this ABI.

Uploads validate dimensions, stride and byte extent, reserve capacity, then copy
rows once into a tightly packed kernel-owned VMM snapshot. Capture pins the
canonical DRM resource independently of descriptor/GEM lifetime and forwards its
actual source format. The requested destination format controls retained alpha.
Both operations run on the compositor worker and publish only verified completed
images. Present validates and pins every exact image before queueing. It composes
an owned ARGB staging root, waits for completion and desktop-state restoration,
then rechecks scene binding and WM incarnation/generation before swapping roots.
The desktop draws these roots with the premultiplied shader. Legacy capture roots
retain their XRGB format and drawing mode.

Per-scene limits are 64 version slots and 256 MiB of logical pixel storage.
Global admission allows 32 scenes, 64 jobs and 256 MiB of upload snapshots. The
job count includes preparation, execution, completed jobs awaiting caller release
and quarantine; it is not just queued requests. Capacity is reported by the stats
operation. The driver's shared texture table can reject allocation before these
per-scene limits; failed creation preserves existing images. Matching idle images
are reused; bounded worker eviction handles other idle sizes and closed scenes.

FD duplication, fork and Unix rights transfer retain the shared close handle.
Final handle teardown only atomically requests close and wakes the worker; it
never acquires the sleeping scene mutex or waits for the GPU under a socket/FD
lock. A short atomic publication claim orders publication against final teardown.
As with other pinned FD operations, logical `close(fd)` can return while an
already pinned operation is still in flight: this API does not promise that
close returning cancels that operation. Queued timeouts can cancel before GPU
work; running cancellation retains the operation until completion and suppresses
publication. Uncertain operations retain the job, source/snapshot, image pins and
admission budget until device reset. Jobs whose input was quarantined by earlier
work also retain their complete input set and never submit another draw.

A scene replacement changes the active binding under the scene lock. Old scenes
cannot publish into a reused slot or replace the new scene's content. Scene close
retires layer images asynchronously and leaves the window-owned last completed
root intact until replacement or window destruction. Constructor rollback pins
and verifies the exact FD handle before closing, avoiding a concurrent FD-reuse
race. No GPU operations run under the scene metadata lock.

The production-service host fixture uses controlled GPU, window and memory
operations to exercise actual queue/ownership code: snapshot packing and source
overwrite, independent capture leases, public release while queued, close before
and during GPU work, final-teardown/publication ordering, queued terminal/timeout
cancellation, scene replacement, window incarnation and content-generation
invalidation, allocation failure and uncertain upload retention. These are
component tests, not hardware failure injection or proof of every syscall race.

The first hardware run passed retained-source and premultiplied pixel checks,
then exposed an existing Unix compatibility gap: Python could not wrap a
`socketpair()` FD because `getsockname()` returned `EBADF` for Unix sockets.
That checked-stop diagnostic is preserved in
`build/workloads/gpu-scene-service-20261019/candidate/diagnostic-audit.json`.
Unix sockets now preserve their local bind address independently of the routing
path, report anonymous/local/accepted names through bounded user copies, and
support `SO_DOMAIN`/`SO_PROTOCOL` queries. This does not establish complete Unix
abstract-name, autobind or peer-name support.

Hardware session `0c24bcc9712848c8a3813da6c12c2978` passed the complete
UID1000 scene probe. Native screenshots verified retained red/green/blue, exact
premultiplied green-over-red `(127,128,0)`, and yellow surviving replacement and
final close. The probe covered producer overwrite/readback and GEM/DRM closure,
FD dup/fork, close-on-exec behavior after a real exec, SCM rights surviving closure
of sender aliases, anonymous/truncated/error/local/accepted Unix name queries,
and 128 updates with fresh IDs. The final layer storage was one 16 KiB idle image,
with zero pins, snapshots, reservations, published versions or quarantine.

The existing GL→SHM→GL and minimized-update pixel regression also passed. Quake
GL3 at 1280×720 advanced through three inspected demo frames and closed normally;
this short run makes no comparative performance or audible-music claim. Fourteen
guest jobs drained with complete output and the Quake launch lineage drained.
All 2,330 tracked DRM backing mappings (66,105,344 bytes) were released, without
allocation/retirement failure or quarantine, and audio streams emptied. Checked
shutdown, owned launcher/QEMU exit zero, three offline filesystem checks and nine
preserved document hashes/owners passed. All 52 host suites, production-service
sanitizers and supported compiled entry/startup audits passed. Additional host
cases directly cover shared-image quarantine, mixed-input cancellation and
pre-enqueue close without another GPU draw. Hardware completion fault injection
and exhaustive FD rollback race execution remain unverified. Evidence:
`build/workloads/gpu-scene-service-20261019/verified/acceptance-audit.json`.

Wayland still needs to create scene descriptors, retain/coalesce committed and
cached versions, flatten tree order and clipping, preserve synchronized commits,
and handle detach/disconnect cleanup. This service alone does not establish
Firefox hardware rendering/audio or browser mixed-workload acceptance.

### Wayland dependency snapshots: component checkpoint

`tools/waylandd-pkg/surface-tree.h` adds a single-event-loop immutable commit
snapshot engine. This is a tested component, **not yet used by `waylandd.c`**.
The existing renderer's synchronized-subsurface and hardware child-surface gaps
therefore remain open; this checkpoint makes no new browser acceptance claim.

Each parent commit retains the exact child states it captured, including nested
synchronized commits. Newer child updates remain cached until an appropriate
parent commit captures them. Positions and below/above-parent stacking belong to
the parent's pending state. Association generations reject old snapshots after
unlink/reparent, and snapshots retain node/state lifetimes without parent cycles.
Applying and freeing snapshots use iterative traversal, including deep trees.

Frame callbacks must be owned by the frontend surface, independently of image
snapshot lifetime, and tagged with the node's monotonic commit sequence. Applying
a snapshot makes callbacks through its commit number eligible; actual delivery
remains the frontend's responsibility. This preserves callbacks when replacing
an unapplied parent snapshot discards an older child image, while keeping newer
child callbacks ineligible when a grandparent still holds the older dependency.
The adapter must cancel all callback queues at protocol surface destruction,
even if retained snapshots defer final node memory reclamation.

Commit allocation failure preserves sequence, current/latest state and child
caches. Desynchronizing has a deliberately different failure contract: it may
apply earlier descendants before a later path allocation fails. That failure
requires fatal client OOM teardown; callers must not retry assuming rollback.
Sequence and association exhaustion reject further operations without wrapping.

The package check phase includes dependency, synchronization, stacking, callback,
allocation-failure and 4,096-level lifetime regressions. The component also passes
Clang AddressSanitizer/UndefinedBehaviorSanitizer checks. Astra reviewed the
callback replacement and shared older-dependency cases with no remaining blocker.
Pending integration still includes buffer lifetimes, permanent surface roles,
scene-backed rendering, cross-scene migration and end-to-end Firefox validation.

### Retained-image migration between scenes

Scene control operation 5 accepts a 16-byte `SceneCloneDesc` containing a source
scene FD, reserved zero and a source version. It returns a new public version in
the destination scene. The source must still have its public ownership claim
when the call is admitted. This copies the retained immutable image; it never
reopens a producer GEM or reads pixels back through the CPU. The original client
buffer may already have been released, overwritten and destroyed.

Admission reserves the destination and pins the exact source under the scene
mutex. The job independently retains both scenes, including two balanced job
references for a same-scene copy. GPU composition targets ARGB with full opacity;
verified completion publishes the new version. Source version/FD closure after
admission cannot release the in-flight input. Destination close/cancellation
suppresses publication. Uncertain completion retains both images, scene jobs and
queue capacity; an already quarantined source is never submitted again.

The production-service host fixture covers ordinary and same-scene copying,
source close while queued, destination close before/during execution, allocation
failure, rejected GPU work, terminal cancellation and uncertain completion. It
passes memory/undefined-behaviour sanitizers. Astra found no lifetime or ABI
blocker in review. These fixtures do not inject hardware failures.

Hardware session `e31c22b9d5a746d390b87f2109115b96` verified a captured green
image copied after producer GEM/DRM destruction, then presented after source-scene
closure. Same-scene ARGB copying retained the exact premultiplied blend
`(127,128,0)`; XRGB copying produced opaque blue even with zero source alpha
bytes. All nine native screenshots have 44,800 verified interior pixels each.
The existing upload/capture, descriptor transfer/exec and replacement/close probe
also passed. After 128 further updates, storage stayed bounded at five idle
images (81,920 bytes), with zero public versions, reservations, pins, snapshots,
in-flight jobs or quarantine before final scene close.

The first diagnostic run stopped on an invalid argument in the probe script,
before the new copy checks. Its completed failed job, orderly VM exit, three
filesystem checks and nine preserved documents are recorded separately in
`build/workloads/gpu-scene-clone-20261019/candidate/diagnostic-audit.json`.
The corrected acceptance uses fresh private disks. This API is a prerequisite
for Wayland reparent/remap; the frontend does not use it yet, and browser/mixed-app
acceptance remains open.

The corrected run drained all ten UID1000 jobs with full output and zero live
job tasks. The producer's one 16 KiB tracked backing was released with zero
allocation/retirement failures or quarantine; audio was empty. Checked shutdown,
owned launcher/QEMU exit zero, all three offline filesystem checks and nine
preserved document hashes/owners passed. All 52 host suites and supported
compiled entry/startup audits passed. Evidence:
`build/workloads/gpu-scene-clone-20261019/verified/acceptance-audit.json`.

### Wayland attachment storage outlives protocol buffers

The production frontend now retains pending buffer storage independently of the
`wl_buffer` resource. A SHM attachment uses libwayland's buffer reference, keeping
its pool mapping valid even after both buffer and pool protocol destruction. A
dmabuf attachment retains its imported buffer object and the existing shared GEM
import-cache ownership. A destroy listener clears only the release-event target.
Replacement acquires its new attachment before dropping the old one; commit,
cursor, error and surface destruction consume the retained attachment safely.

Release events are sent only to a still-live buffer resource. Uncertain GPU
capture still sends no release; dropping the frontend attachment on that fatal
path is safe because the kernel retains its independent source lease. This
change removes a borrowed-resource lifetime hazard but does not yet integrate
scene images or synchronized tree rendering.

The real-libwayland fixtures verify SHM buffer/pool destruction before commit,
dmabuf destruction before commit/replacement, import non-reuse while attached,
both pending-surface/buffer destruction orders, and uncertain capture with and
without a surviving protocol resource. All seven package checks pass, including
memory/undefined-behaviour sanitizers with leak detection. The sanitizer run also
caught and removed an existing test fixture's undeleted client proxies. Astra
found no remaining attachment-lifetime blocker.

In isolated GPU session `904f2ad5644042b799bc9616a85eed03`, Quake GL3 at
1280×720 advanced through three captured demo frames, and Mousepad rendered its
SHM editor window. Both UI launch lineages drained after close. The inspected
screenshots show intact content; this short run makes no comparative-performance
or audible-music claim. The final package differs from the staged package only
in test fixtures, with identical deployed/tested `waylandd` binary hashes.

All 2,332 tracked backing mappings (66,125,824 bytes) were released, without
allocation/retirement failure or quarantine; audio streams emptied. Checked
shutdown, owned launcher/QEMU exit zero, all three offline filesystem checks and
nine preserved document hashes/owners passed. Evidence:
`build/workloads/wayland-owned-buffer-20261019/candidate/acceptance-audit.json`.
