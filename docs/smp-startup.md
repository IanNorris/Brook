# SMP startup admission and late CPUs

## Failure evidence

The failed native application review `17a539b49090473980023e6fe524ec2f`
recorded CPU6 timing out, then reporting arrival during CPU7 startup. CPUs0–5
and7 were accepted. The old shared boolean acknowledgement could be set by a
previous, late CPU. Startup also rewrote the shared trampoline/temporary stack
after timeout and subsequently unmapped them.

Several consumers use `SmpGetCpuCount()` as the logical-index limit, including
TLB shootdowns, physical-frame retirement barriers and profiling. With seven
accepted CPUs and a hole at6, CPU7 was omitted. This is a memory-safety defect;
the missing CPU7 profile samples are consistent with that indexing error.
Actual shared-stack corruption was not established by this recording. The
separate zero-free-page failures during STK/Firefox startup remain independently
recorded; this fix does not establish a cause for those failures or solve live
memory pressure.

## Admission contract

The production topology helper orders the BSP at logical CPU0 and removes
duplicate APIC identities before any AP starts. Logical indices are never
compacted after an AP has cached its index. Unknown APIC IDs park rather than
being treated as the BSP.

Accepted CPU indices are a dense prefix `[0, onlineCount)`. Startup stops at
the first stack-allocation, IPI-delivery or acknowledgement failure. It never
starts a later CPU after that failure. Acknowledgements identify the expected
logical CPU, and are published only after the AP has consumed the trampoline
and switched to its reserved private high stack; that stack range is checked.

On ambiguous startup failure, the unchanged trampoline, temporary-stack identity
mappings and attempted AP's private stack are retained. The late AP can arrive
and park, but its activation flag stays clear. Successful startup releases the
low identity mappings. Preparation allocation failure panics instead of leaving
an accepted CPU with a null environment or syscall stack. Prefix membership and
complete preparation are checked before activation.

This is a bounded repair for the existing serial startup design. It may leave
fewer processors available after a timeout. It does not implement parallel AP
startup, hotplug, retry or reclaim the late CPU's reserved bootstrap storage.

## Panic delivery

A parked AP has not loaded a per-CPU TSS/IST and GS environment. Panic handling
therefore targets only accepted CPUs that have published NMI readiness, excluding
the caller. Readiness publication and the subsequent panic-flag check use full
StoreLoad ordering: either the panic scanner targets the AP or it observes panic
and parks before scheduler entry. The BSP publishes readiness after its own setup.

Targeted panic sends check LAPIC availability and bound the ICR-idle wait. A
failed send can still leave a processor running; the returned acknowledgement
count is not a guarantee that every target halted. Device failure and the exact
readiness/panic interleaving have not been fault-injected.

## Validation

All 48 host suites and the supported compiled entry/startup audits with mutation
controls passed. The production helper suite covers reordered/sparse APIC IDs,
duplicates, capacity, wrong acknowledgements and a permanently stopped suffix.

Four checked guest runs used eight configured CPUs, 4 GiB, muted hardware GL and
private disks. Each passed 1,024 dynamic program launches with eight workers,
stale mmap/fork tests, PRIME backing/exporter-close tests, boot TLB self-tests and
profiler coverage for every accepted CPU. All tracked jobs exited zero with full
output/EOF and no live lineage. Checked shutdown, owned launcher exit zero,
offline data/Nix/FAT checks and nine UID1000/GID100 document fixtures passed.

| Run | Accepted logical CPUs | Kernel SHA-256 |
| --- | --- | --- |
| Default | 0–7 | `2f1578d84645697b21f29af648271694f057b339c68394c0d481074799cc7c31` |
| Trampoline gate | 0–2 | `9edc46cf93b20b43d830ed6285904a8a0ce805af9c95edbc70b451c19e31380e` |
| Private-stack gate | 0–2 | `83ae8246d898d941314d451b8d47b9bbe1f795a3a48dd44771fbec6fd2c8c4b7` |
| Restored production | 0–7 | `51e2d03828b07e4df1818ef45b007bb8961bcafe853597bec7ef193812601d90` |

The first gate holds AP3 in stackless assembly before its temporary-stack
`lretq` and private `stack64` load. After timeout, the test checks the original
bootstrap input, releases the AP and verifies its arrival on its own reserved
stack. The second gate delays AP3 after that switch but before acknowledgement.
Both require exactly three accepted CPUs, no AP4–7 startup and no AP3 activation.

A separate private-stack-gated VM deliberately panicked after a checked sync.
Direct saved-memory inspection showed exactly CPUs1 and2 NMI-halted, while the
panicking CPU0 rendered its diagnostic and AP3 remained in the entry parking
loop. No secondary exception occurred. Register/memory artifacts and the cached
panic framebuffer were captured through the bridge. This fault VM was explicitly
force-stopped after evidence capture; it could not sync after panic. Its owned
launcher exited zero, all offline filesystem checks passed, and document
fixtures were preserved. It is separate from the checked-shutdown acceptance runs.

Private evidence is under `build/workloads/smp-prefix-20261019/`. Preparation
allocation failures, IPI delivery failures, arrival exactly on the timeout boundary
and arbitrary user register/interrupt-return paths remain unverified by these
specific tests. Mixed real-application acceptance and fresh full bootstrap runs
still need renewal.

## Reproducing the fault hooks

`BROOK_SMP_BOOT_TEST_STAGE` is a CMake cache setting at both project levels:

- `0`: normal production build, the default; no gate code or gate data.
- `1`: AP3 gate in the trampoline before stack consumption.
- `2`: AP3 gate in C entry after private-stack verification.

Build variants only while every QEMU is stopped, freeze the matching kernel,
symbols and modules, then boot isolated disks. Test modes require at least four
configured CPUs. They emit `SMP_BOOT_TEST_PASS` only after observing the intended
timeout, retained bootstrap inputs and late private-stack handoff. Restore the
cache to `0` after fault testing; the tested worktree's production cache is0.
