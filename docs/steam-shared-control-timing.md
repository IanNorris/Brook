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
