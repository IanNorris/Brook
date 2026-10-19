# GPU resource ownership and retirement

## Current acceptance and known failures

The GPU transfer fix and scheduler startup repair are separate from resource
retirement. Private DRM resources now retire after their last owning reference
and verified GPU completion. Exported display sources remain quarantined;
compositor texture destruction and resource-ID recycling remain open. Do not
reset counters or recycle IDs until the remaining completion rules below are
implemented.

Two bounded guest probes distinguish different exhaustion paths:

- `scripts/workloads/gpu_resource_cycle_check.py --brook-guest` creates and
  closes tiny buffers, with one GEM live at a time. The recorded run completed
  4,095 of 4,500 allocations; allocation 4,096 failed with ENOMEM, and a new DRM
  context could not allocate either. Evidence is in
  `build/workloads/window-surface-20261018/resource-stress/`. This is the
  unresolved cumulative host-resource-ID limit, not a simultaneous-GEM budget.
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

Both `ResourceCreate3D` and native `BufferUpload` need one live/retiring/free ID
allocator. Their independent upward/downward counters can collide. Either keep
an old context namespace reserved while exported objects survive, or use a
context-independent resource namespace with owner generations. Remove or
incarnation-tag the numeric `g_compExternalAttached` cache before reuse.
Synchronize the module ABI/version when extending GPU operations.

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
