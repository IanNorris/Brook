# TCP receive copies and the mixed-workload hang

The GPU/scheduler experiment exposed a TCP receive-copy defect. The private
`gm-e0` VM stopped making progress near 72 guest seconds. QMP showed CPU 0
(`debug_tcp`, interrupts masked) and CPU 4 (`net_poll`) spinning in `HandleTcp`
on the same socket ticket lock. Six vCPUs were idle. Frozen task/stack snapshots
showed a Firefox receiver blocked in mapping-gate acquisition from a kernel
page fault under `sys_recvfrom`; timer sleepers could not wake while the BSP
remained stuck. The code passed the raw userspace destination into `SockRecv`,
whose ring-buffer `memcpy` runs with that socket lock held. CoW or a concurrent
mapping change can make this copy fault and block while retaining the lock.

Evidence is retained at
`build/workloads/timing-scheduler-20261020/gpu-mutex-browser/gm-e0/hang-evidence/`:
eight vCPU register sets, bounded Process records and stack tails, exact layout
offsets, running-task stacks and the forced-stop receipt. ProcessState is one
byte; the initially decoded state word is retained separately. The guest could
not cleanly shut down. Its disks remain failure evidence. A zero QEMU/launcher
exit after forced termination is not clean-shutdown or workload acceptance.

## Repair and scope

Both userspace `SockRecv` call sites, TCP `read` and `recvfrom`, now use
`ReceiveTcpToUser`. It checks writability outside the socket lock, allocates a
resident kernel staging buffer, receives into it, then performs fault-aware
`CopyToUser` after `SockRecv` releases the lock. The staging lifetime is protected
against cancellation. One call stages at most 64 KiB; larger reads may return
a legal short prefix rather than narrowing a 64-bit count to a wrapped value.
TCP zero-length reads return zero without touching a destination or consuming
data. The kernel API now documents its resident-destination requirement.

This does not pin userspace mappings through the entire operation. If a sibling
unmaps the destination after preflight, the final copy can fail after bytes
have been dequeued. UDP direct-buffer paths and the separate INET `recvmsg`
implementation are not covered by this repair. Their broader copy/scatter and
concurrency semantics remain follow-up work; this is not an all-socket audit.

## Native checks

`scripts/workloads/tcp_receive_copy.py` uses a generated private HTTP payload.
It forks children to receive into inherited CoW mappings using both `read` and
`recvfrom`. Two rounds exercise CoW, invalid pointers, read-only mappings and a
protected second page. Each rejected destination returns EFAULT and a subsequent
valid receive still obtains the queued prefix. Parent mappings remain unchanged.
All sixteen cases pass on the fresh private RR VM `tc-qr0`.

The same VM runs the Quake II GL3/720p demo and exits zero. The measured
15.156811-second marker interval is close to the nominal 15.1 seconds, including
log-reader scheduling and screenshot overhead. This does not establish that
the user's earlier intermittent 1.3× playback problem has been reproduced.
The game screenshot and all licensed data stay in ignored local evidence.

A separate fresh `tc-be0` EEVDF VM completes the previously hanging buffered
Firefox/24-worker/input/audio cohort. Its eight-second capture has 353,152
frames over 8.00363 host seconds, with no silent 10-ms blocks. Periodic p95 is
3.081 ms and worst 135.035 ms. The exact sources and evidence are under
`tcp-receive-acceptance/` and `tcp-fixed-browser/` in the timing workload tree.
Both application lineages drain, heaps pass, owned launchers exit zero after
guest shutdown, four filesystems pass offline checks, and twelve saved documents
plus four original profile trees preserve checked bytes and ownership.

The build passes 62 host suites and the established compiled entry/start
mutation audits. The entry audit still has four explicitly unsupported paths;
an extra `--require-all` experiment failed solely on this unchanged set and is
retained, rather than represented as full static verification.

**Timing acceptance remains open.** RR's runtime diagnostic rises from zero to
seven backward nanosecond samples during the native run; their magnitude was
not recorded. Other runtime error counters are zero. Investigate these samples
before accepting the CPU-accounting/clock gate or attributing the intermittent
speedup to them. The candidate also contains the separately documented GPU
mutex/polling experiment, so these are combined-kernel safety checks, not an
isolated performance comparison of the TCP repair. RR remains the default.
