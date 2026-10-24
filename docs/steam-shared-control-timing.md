# Steam shared-control timing

Steam login remains an acceptance requirement; passing the synthetic tests or
improving memory throughput alone does not establish a usable client.

## Controls and timing lead

`tools/at-syscall-probe/sharedcontrolcheck.c` passes on Linux, natively in Brook,
and inside Brook's actual SteamRT container. An independently exec'd helper
opens the same 8,208-byte object through a separate descriptor. The test checks
read/write and read-only aliases, the final 16-byte partial page, descriptor
closure, unlink/recreate separation and 64 shared-futex exchanges. The stronger
version forces different virtual bases and requires both a successful blocked
wait and a nonzero wake result. All tested groups drain with exit zero. This
weakens basic mapping and shared-futex hypotheses; it does not model Steam's
initialization, notification or owner-liveness protocol completely.

The low-volume lifetime probe records object identity and mapping lengths,
offsets, protections, flags, results, close/unmap order and Unix listening.
It exports no application memory, literal names, raw pointers or received
SCM_RIGHTS descriptor values. Linux reaches visually verified QR login with
127 metadata rows. Brook review O stays black with 1,082 rows, maximum sequence
821 below its 1,024-row per-process cap, and no lost mapping-tracker event.
Both sides successfully map the same small objects with length 8,208,
MAP_SHARED, read/write protection and offset zero. Both helper listeners succeed.

On Linux the first helper large mapping to the second takes 44 ms; the second
to releasing its reply control takes 46 ms. The parent releases its reply after
93 ms, following successful acquisition of both large objects. On Brook those
helper intervals are 1.078 seconds and 941 ms. The parent releases its first
reply after 502 ms, before the second pool is even mapped. Across 136 captured
parent small-object lifetimes, the median is 505 ms. This supports an apparent
500 ms handshake budget; the trace does not establish exactly which intervening
application instructions consume that time.

O restores the wrapper, drains tracked groups, passes heap integrity and checked
sync, and its original QEMU owner exits zero. Serial acceptance and four offline
filesystem checks pass. Its application exit 137 follows agent termination,
not a successful Steam shutdown. The Linux service, restored wrapper, sync,
normal powerdown and read-only offline filesystem check all pass.

## Uninstrumented pool benchmark

`scripts/workloads/shared_pool_benchmark.c` separates sizing, mapping, first
full write and warm write using known synthetic data. It covers 8,208 bytes,
8 MiB and the exact 26,214,416-byte pool, plus an anonymous mapping and two
simultaneously live shared-file pools. Both timed writes are retained and data
is checked. Build static musl with `-std=c11 -O2 -Wall -Wextra -Werror`; run as
the desktop user with writable `/dev/shm`. `--repeat` runs four rounds. A
30-second watchdog bounds it. Output is fixed numeric JSON and correctness
failures; no performance threshold is encoded as a correctness assertion.

Fresh Brook review P runs no Steam or preload. Shared-file cold writes take
63 ms at 8 MiB and 562 ms at the large size; warm writes take about 1 ms.
Anonymous first write at the large size takes 28 ms. The two-live-pool cold
writes take 538 and 611 ms. A Linux **host** control using the same binary takes
about 8 ms per shared pool; that is not a normalized Linux-VM performance ratio.

A separate 15-second synthetic CPU/context-switch capture contains 11,475
samples and 72,585 switches, with matching reported counts but 43,131 producer
drops. Its IRQ-masked sampling limits and heavy loss prohibit precise time-share
claims. Page faults and tmpfs writeback are visible. Source inspection also
finds that tmpfs backing-page lookup linearly scans every materialized page,
consistent with the strongly superlinear uninstrumented timings. P completes
all jobs, heap/sync, original-owner exit, serial and four offline checks.

## Indexed tmpfs candidate

The candidate keeps the ownership list for sparse truncation and final teardown,
and adds a hash index from 32 materialized pages. Bucket arrays grow with the
materialized count, not EOF, and saturate at 65,536 buckets. Optional allocation
is attempted only once per 32 insertions. Failed initial/replacement allocation
retains the old index or list; successful rebuilding performs no further
fallible operation before publishing. Both insertion paths share the helper.
Truncation rebuilds without allocation or frees the small-file index; final
release frees the list and bucket allocation once without adding a sleeping lock.
The existing filesystem mutex protects both lookup views.

Page links and bucket arrays are honestly quota-charged metadata. They can reduce
headroom, including a canonical-frame reservation after backing preparation.
The change does not promise identical quota capacity. Tests explicitly force
that canonical failure after index installation, verify retained data/retry and
complete reclamation, and cover sparse high indices, failed initial/replacement
index allocations, partial truncation, low-water truncation and regrowth.
All 102 host tests and both compiled entry/start mutation audits pass in their
supported scope. Astra's read-only review reports no actionable correctness gap.

The first native candidate measurement in review Q has two-live-pool cold writes
of 137 and 158 ms, versus 538 and 611 ms in P. The single large shared-file case
falls from 562 to 162 ms. The strong native shared-control regression passes.
The strong regression also passes inside the real SteamRT container. Q restores
the wrapper, drains tracked groups, passes heap/sync, original-owner exit,
serial acceptance and four offline filesystem checks. These benchmark results
alone are not a Steam login pass.

The completed Q trace changes the diagnosis. Its first pool-pair gap is 546 ms,
down from O's 1.078 seconds, but still misses the first approximately 502 ms
parent reply lifetime. Later attempts improve enough that the parent actually
maps both large pools about four seconds after its first control mapping.
There are 228 metadata rows, maximum sequence 125, and no observed tracker loss.
The final frame remains black with small red artifacts along its top edge.
Thus slow allocation explains early retries, but does not establish the cause
of the remaining post-handoff failure.

A clean five-second profile taken after this handoff contains 3,983 samples and
31,056 context switches, with zero reported producer drops and matching counts.
Of those samples, 3,850 have IdleLoop leaves; the visible FAT-writing stack is
profiler output, not evidence of Steam disk activity. This capture is not aligned
with pool initialization. It does not justify calling canonical-page lookup the
active post-handoff bottleneck. Astra recommends separating the initialization
cost from the subsequent liveness/rendering failure before another kernel fix.

Private diagnostic sources/receipts remain under
`build/workloads/steam-control-lifetime-20261024`, `steam-pool-cost-20261024` and
`steam-tmpfs-index-20261024`, plus reviews O/P/Q. Proprietary payloads, profiles,
raw application logs, login images and VM disks remain outside git.

## Post-handoff diagnosis

Review S repeats Q's indexed-pool controls and reaches parent pool acquisition.
The network service remains live, while errno 97 repeats on its network thread.
A targeted twelve-second NET trace confirms rejected AF_INET6 sockets followed
by successful AF_INET socket creation, then an IPv4 connection refused result.
Serial connection metadata identifies repeated loopback destinations, not a
failure to fall back from IPv6. This after-return trace has global repeated-error
suppression and does not establish syscall frequencies or blocked-call absence.

Direct guest X11 protocol snapshots verify window classes and map state. The
browser's 200x200 window is unmapped. A later 700x350 mapped window extends past
the 800x600 X screen; cropping to its visible 464x333 portion avoids GetImage
BadMatch and produces all-black pixels, as does the root drawable. This is not
evidence of a correctly rendered login UI being lost only during composition.
Zygote-derived children retain inherited cmdline information, so argv alone
cannot establish renderer identity. Proc stat start-time/exit-code placeholders
also cannot prove child termination status.

A separate known-data TCP listener bound to 127.0.0.1:19001 reproduces connection
refused on the old kernel. Its early blocking accept also returns EAGAIN; this
is a distinct existing accept behavior gap, so the stronger routing regression
waits for listener readiness before accepting. Source inspection finds local
delivery only for the NIC's own address, with no 127/8 route. The next candidate
adds local delivery with packet-origin distinction, correct TCP source/checksum
and passive-open addresses, and state publication before synchronous ACKs.
`loopbackcheck.c` is public synthetic acceptance. Linux and Brook review T
pass, including parent and child watchdogs; the checked static helper SHA-256
is `f408159649d2db9e880b7cd1a97f68d8fc397aacfa17399523a40d52874c60be`.
The stronger test transfers 8,193 known bytes in both directions, checks EOF,
and reports close durations of about 8 ms without an encoded latency threshold.
The tracked guest group drains with exit zero. Steam retry remains pending. Full blocking-accept behavior and broader UDP
demultiplexing remain separate acceptance gaps, not implied by this probe.

S restores wrappers, drains jobs, passes heap/sync, original-owner exit, serial
acceptance and four offline filesystem checks. Candidate review T uses 12 GiB
instead of 16 GiB to preserve host headroom; its measurements must not be treated
as a normalized performance comparison with Q/S.

T establishes the Steam loopback connections that S repeatedly refused. A
bounded local HTTP root probe connects to all three local listeners but receives
no response within one second; their protocols are not established by that probe.
No new fixed shared-memory-corruption or EnsureConnected category appears in
the appended CEF log window. The client still does not present usable login.

An independent X11 paint control reveals another layer: a known green background
reads back black on the accelerated Xwayland display, which subsequently exits
with signal 6 after its fatal segmentation-fault report. The corresponding
explicit rectangle control on a separate `--glamor off` Xwayland display reads
back all 12,800 expected pixels with no X errors. The retry on that software display also completes the pool handoff but
does not produce usable login. Its browser control window remains unmapped
at the paired snapshot. This separates the independently reproduced graphics
problem from the remaining startup/service problem. The user also observed transient red top-edge
artifacts absent from approximately contemporaneous bridge images. The user
confirms the captures were at approximately the same time; the discrepancy
is retained as unresolved and must not be dismissed as merely a time difference. QEMU screendump
returns `no surface` in this GL display mode, so an independent screenshot-path
comparison remains unavailable. Neither observation establishes final scanout
correctness or a usable Steam renderer.

T finishes both client attempts, restores the pristine wrapper, drains all tracked
groups, passes heap integrity and a checked guest sync, and its original QEMU
owner exits zero. Serial acceptance and all four offline filesystem checks pass.
The accelerated Xwayland crash and failed first stop command remain recorded as
failures; neither is relabeled successful. The software client exits 137 after
agent termination, not a normal Steam exit. The remaining bounded lead is local
service acceptance/response, including the independently observed premature
EAGAIN from blocking accept. There is no login acceptance yet.
