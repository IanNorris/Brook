# GPU command ownership and preparation overlap

The control queue now gives each request persistent storage and a generation
token. The first integration retains synchronous submission, one outstanding
request and the existing GPU command mutex. It prepares for independent compose
packets; it does not yet overlap preparation or introduce another queued frame.

`BeginCommand` reserves ownership before publishing descriptors and captures the
original completion budget. `FinishCommand` accepts only the current storage and
generation, validates the exact used-ring advance, descriptor identity, response
length and expected fence/context, then consumes the completion once. A timeout
or ambiguous completion quarantines the queue and original pages. Legacy builders
redirect to scratch storage rather than overwrite pages still owned by the host.
Storage lifetime, disjoint backing and generation continuity remain caller
requirements. A ready completion wins at the timeout boundary; unavailable clocks
use bounded polling, which does not bound arbitrary preparation time.

The panic presentation path refuses submission while a reservation, unvalidated
completion or poisoned queue exists. This guard assumes the other CPUs have
already stopped. Failed vertex uploads now prevent drawing stale vertices.

Hosted tests compile the actual control submission and panic admission bodies.
They cover delayed and malformed completions, stale/duplicate tokens, distinct
preparation bytes, elapsed preparation budgets, ring wrap, quarantine and late
completion. Release build and all 79 hosted suites pass. These fixtures do not
prove device DMA or concurrent SMP behavior.

Native evidence is retained privately under
`build/reviews/unattended-command-ownership-gpu-1ms-fixed-mlfq-a-20261021`.
The checked kernel hash is
`2e880c556a476db4ff244130b5c20da982e3e9daf9471de5e0c878beb7a55ab6`;
the GPU module hash is
`f539092fc0aa4830aa5275460790edb88cbb7ee7c8c2bbbffe4326b66a279111`.
With eight vCPUs, 8 GiB, MLFQ, the private 1 ms host fence poll and 1920×1080 GPU
composition, boot clear/copy/blit/scanout/compositor readbacks and owned capture
self-tests pass. An off/on/off empty-desktop sequence retains 256 reconciled
frames: body p50 2.354 ms, p95 4.815 ms; start interval p50 18.011 ms,
p95 21.015 ms. The prior recorder baseline has p50 interval 18.005 ms;
this single ordered comparison establishes no performance improvement.

The original controller and VM owner both exit successfully. Heap integrity,
runtime accounting checks and all four offline filesystem checks pass. App
rendering, input responsiveness and GL/Vulkan performance acceptance remain open.
Next, allocate disjoint persistent compose storage, construct its packet while
the vertex transfer is outstanding, and validate that transfer before publishing
the fenced draw. Preserve the render fence before display flush. Additional
frames in flight require separate vertex/resource/display ownership contracts.

## Independent compose storage and within-frame preparation

The next implementation allocates another persistent contiguous 64 KiB request
and response buffer during pipeline setup. Each frame constructs its compose
packet there after publishing the vertex transfer in legacy storage. The vertex
backing, original request, response pages and descriptor pair remain untouched
until the transfer completion is validated. Only then is the compose packet
published and fenced; only a successful draw completion permits display flush.
There is still one outstanding control request and one GPU frame at a time.
No per-frame allocation, shared command-pointer redirection or asynchronous
scanout is introduced.

Preparation failure still drains the published transfer. Valid transfer or draw
errors suppress presentation and permit a later frame to rebuild; an ambiguous
completion retains the queue quarantine. The actual frame-ordering body is
shared with hosted failure tests, and the actual packet builder is shared with
guarded boundary tests covering maximum quads, changing shader/blend/sampler
state, maximum-strength blur barriers, command framing and rejected capacity.
The request capacity now subtracts the submit header, and blur reservations
conservatively cover all emitted passes. Oversized construction fails rather
than publishing a truncated scene. Build and all 80 hosted suites pass.

`command_preparation_ns` records the subset of exclusive `command_build` time
inside a control-call scope. Thus inclusive control intervals reconcile with
queue setup, completion waits and this preparation subset. It is not added to
frame elapsed time, and it proves neither simultaneous device execution nor
time saved: the transfer may already have completed before CPU construction.
The model test and 11 analyzer tests check this accounting.

The private native review is
`build/reviews/unattended-preparation-overlap-gpu-1ms-fixed-mlfq-a-20261021`.
Kernel SHA-256 is
`ee2170484ef363761018dbce47604d323733480f4bf890e1c0cd7c295ff1e250`;
GPU module SHA-256 is
`81ab428a1b8f19d2978231343241f4b972012cc2784b26b6f00ebd36d157379b`.
The same native configuration retains 256 exactly reconciled frames, with no
invalid frames, clock failures or recording contention. Preparation while the
transfer remains unconsumed has p50 0.953 µs and p95 1.279 µs. Frame body has
p50 2.339 ms and p95 3.598 ms; start interval has p50 18.007 ms and p95
19.021 ms. Against the preceding ownership-only run's p50 18.011 ms, this is
no demonstrated throughput improvement. The earlier measurement correctly
predicted a very small construction opportunity.

Native GPU readback/capture self-tests, heap/runtime checks, original controller
and VM owner exits, and all four offline filesystem checks pass. The raw
off/on/off capture, analyzer source and verification receipt remain in that
review. GL/Vulkan app performance, input response and full presentation
correctness remain unverified. Fence observation and additive post-frame pacing
remain the larger measured targets; this result does not close the goal.
