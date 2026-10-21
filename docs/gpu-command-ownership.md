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
