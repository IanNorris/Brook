# GPU resource ownership and retirement

## Current acceptance and known failures

The GPU transfer fix and scheduler startup repair are separate from resource
retirement. Host resources are still not unreferenced on GEM close or compositor
texture destruction. Do not reset their counters or recycle IDs until the
ownership and completion rules below are implemented.

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
Import/export copyout failure now removes newly inserted metadata handles or
FDs, preserving pre-existing handles. Those copyout rollback branches have not
been fault-injected. Pinning metadata does not establish backing lifetime or
safe exporter death. Current duplicate lookup compares unrecycled host IDs;
replace this with shared object identity before enabling ID reuse.

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

Existing disk images need both the rebuilt kernel and rebuilt `waylandd`:
the kernel now returns shared handles, and the server counts their buffer owners.
The prepared private user review preserves the prior user packages/profiles;
its launcher is `build/reviews/prime-import-20261018/launch.sh`. Copyrighted game
assets stay in ignored private images and must not be committed or redistributed.

## Required ownership conversion

Use one shared resource object, independent of recyclable GEM table slots. It
owns immutable geometry, backing allocation and driver identity/incarnation.
GEM handles, PRIME capabilities, imported handles, active operations and VMAs
hold references. Do not retain a whole DRM context from its own GEM objects:
that creates ownership cycles, including cross-importing contexts.

Make creation transactional. Check host backing attachment, and roll back
creation, GEM insertion, FD publication and copyout in dependency order. An
ambiguous host timeout cannot justify freeing pages the device may still use.
Quarantine uncertain resources until their retirement can be established.

For mappings, acquire the resource reference under `DrmCtx::mappingLock` while
resolving the GEM. Store it in `DeviceMappingBacking` and release it from that
mapping backing's final callback. Existing `MappingBacking` ownership then
carries the resource through preparation, fork, splitting and unmapping. This
also protects the interval before physical-page pins are installed. Give mmap
tokens stable object identity so an old token cannot map a recycled GEM slot.

## Presentation completion

The current `WM_PRESENT_GRES` stores a raw ID for later compositor sampling,
while Wayland immediately sends `wl_buffer.release`. A retained resource
reference alone does not make that valid: the client can overwrite a still-live
buffer once it receives release.

Resolve presentation through an owned DRM/PRIME capability. Prefer a synchronous
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

Final reference release enqueues retirement rather than issuing GPU commands
from arbitrary mapping or FD callbacks. A worker outside those locks must:

1. Complete outstanding work on relevant producer/compositor contexts.
2. Detach context and backing associations and verify host resource unref.
3. Free guest backing and recycle the resource slot only after completion.

Retire compositor textures as well as application resources. The CPU reader
epoch protects existing CPU readers; it does not establish host GPU completion.
`AppPresentBarrier` currently fences app contexts only. Add compositor-context
completion without recursively acquiring `GpuCmdGuard`.

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
