# Default priority query ABI checkpoint

Scheduler source review found `sys_getpriority` returning zero as the default
nice value. Linux's successful syscall encoding is `20 - nice`; libc converts
it back to a signed nice value. Returning raw zero therefore made both libc and
Python report nice **20**, outside the supported -20..19 range, instead of zero.
See the Linux manual's
[kernel/libc distinction](https://man7.org/linux/man-pages/man2/getpriority.2.html).

The fix returns encoded **20** for Brook's existing fixed nice zero. It does not
alter policy selection, weights, request length or task priority. The new
read-only `scripts/workloads/probe_priority_abi.py` queries the calling thread
through `libc.syscall`, libc `getpriority` and Python `os.getpriority`, checking
the encoded range, conversion and optional expected nice value.

| Environment | Successful syscall value | libc nice | Python nice | Encoding check |
| --- | ---: | ---: | ---: | --- |
| Host Linux reference | 20 | 0 | 0 | pass |
| Brook before | 0 | 20 | 20 | fail, as expected |
| Brook after | 20 | 0 | 0 | pass |

Both Brook observations use fresh private eight-vCPU/six-GiB EEVDF VMs and the
same unprivileged UID 1000 probe, with no priority writes. Before-kernel SHA:
`531113d815a2e61b80350c8b08d023a1726f3d0443f709efdb40bf56fbed3a90`.
After-kernel SHA:
`d3a2f64ea1a6436296e84188904b8b9e1862fa14d5c90ca7b496d9eeafd6e50f`.
All thirteen modules are byte-identical. Import/export validation passes with
106 exports; the after-kernel entry audit passes twelve supported templates and
56 mutations, retaining four unsupported paths. Both runs pass heap integrity,
runtime checks, all-eight-CPU clock coverage, original launcher/router exits,
four filesystem checks and saved-document/profile preservation.

The before controller initially missed creating its job directory and stopped
before sending a command. Recovery retained that same VM and issued the first
probe once. Its expected exit 1 then triggered the generic tracked-command
helper's failure check; shutdown reused that completed negative probe's saved
evidence without rerunning it. The after controller creates its directory and
completes normally. Ignored `priority-abi` evidence retains those dispositions,
original/recovery controllers, exact probe/job outputs, kernel identities,
source hashes and lifecycle checks. Both VMs and their private services stop.

This is a correction to the default query's encoding, not complete priority
support. `setpriority`, `sched_setparam` and `sched_setscheduler` still report
success without implementing requested changes; selection/existence/error
semantics also remain incomplete. The internal `schedPriority` hint is separate
from Linux nice, and EEVDF still starts tasks at equal weight. No realtime
scheduling guarantee follows from those stubs. Browser performance after this
metadata correction needs a new matched workload run; the recorded handoff
comparisons deliberately use the unchanged before-kernel capsule.
