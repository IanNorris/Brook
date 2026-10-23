# Steam allocation stack checkpoint

The targeted allocation capture identifies a reproducible compatibility gap;
Steam still does not reach login. No allocator quota or application error checks
were changed for this measurement.

An agent-written delegating preload probe records bounded large-file
`ftruncate`, `mmap` and allocation calls, numeric outcomes, caller module names,
relative instruction offsets and available exported symbols. It preserves
delegated results and errno and does not export mapped contents. A synthetic
control inside the actual SteamRT container verifies a 26,214,416-byte allocation
and mapping, plus a deliberate EBADF result and its caller stack. These controls
are necessary: successful application execution alone does not prove telemetry
is active. Proprietary payloads, raw client logs and probe binaries remain in
ignored private artifacts.

## Observations

Two fresh review VMs use the unchanged accepted kernel
`eaf81ea15a3cc498bc9b650de49cb6b7edeb5085ea781ae0408b980c5b4c7e6a`,
ABI 50, eight CPUs, 8 GiB and MLFQ. The probe is enabled in the main client and
its web helper after runtime construction. The helper wrapper is restored to
its original SHA256 before shutdown.

The first capture sees ten 25 MiB plus 16-byte pools from one web-helper process.
Their truncate and mapping calls succeed. The recurring allocation offsets are
`steamwebhelper+0x482ffb` and `steamwebhelper+0x483051`, through
`0x485960` and two branches at `0x40ac62` / `0x40acc5`. Both worker-thread and
CEF-callback stacks reach this path. This locates the allocator, but does not
establish whether every pool represents a retry or a legitimate connection.

A separate failure appears three times in the first capture and six times in
the second: `libcef.so+0x67b524e` calls `ftruncate64`, which returns EBUSY.
The second probe adds a pre-call fstat and shows that every failed operation
attempts growth from **8,192 to 270,336 bytes**. This is not mapped shrink.

A small independent control confirms that Linux permits same-size, growth and
shrink requests on mapped temporary files. Brook returns EBUSY for all three on
both tmpfs and the data filesystem. `VfsTruncateLocked` currently rejects every
mapped vnode identity before reaching its backend. Memfd has a different policy:
its mapped-shrink rejection does not prohibit growth.

The sizes match Chromium's documented block cache layout: a mapped 8 KiB header
and growth in 1,024-block increments, including 256-byte blocks. This is a
**candidate subsystem**, not a symbolized identification of the captured caller.
See [Chromium's disk cache design](https://new.chromium.org/developers/design-documents/network-stack/disk-cache/)
and [the block-file growth implementation](https://chromium.googlesource.com/chromium/src/+/a5146dcda451263c595074850bd62037ac37d8ce/net/disk_cache/blockfile/block_files.cc).

## Limits and next step

In the first timeline the resize failures precede the last four pools, but
follow the first six. The first instrumented run has no new persistent-allocator
corruption message; the second has one and launches multiple helper processes.
Both ultimately exit 135. An earlier syscall failure causing secondary retries
or corruption remains a hypothesis, rather than a demonstrated causal chain.

The next compatibility target is safe mapped growth, with canonical pages and
existing aliases retained. Do not remove the mapped-resize guard wholesale:
shrink, partial-page EOF tails, concurrent mutation and backend error recovery
have separate obligations. Renew the same client measurement after any fix and
compare pool ownership, failure timing and newly appended log bytes.

Private evidence: `build/reviews/steam-allocation-stacks-off-gpu-1ms-fixed-mlfq-{b,c}-20261023`;
probe sources and tracked job helpers:
`build/workloads/steam-allocation-stacks-20261023/`.
