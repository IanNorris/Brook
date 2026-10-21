# Saturating large timeouts

Structurally valid large timeouts now saturate instead of failing with EINVAL.
A common converter validates seconds and nanoseconds before conversion, then
caps at signed 64-bit nanoseconds. This applies to nanosleep, clock_nanosleep,
futex waits, rt_sigtimedwait, POSIX timers and timerfd. Existing ppoll/pselect
millisecond saturation remains unchanged.

The conversion follows Linux's seconds threshold: seconds at or above
`INT64_MAX / 1000000000` produce `INT64_MAX` nanoseconds, including values
whose exact arithmetic result would still fit. Negative seconds or nanoseconds,
and nanoseconds at least one billion, still fail validation. See Linux's
[ktime conversion](https://github.com/torvalds/linux/blob/v6.12/include/linux/ktime.h)
and [safe deadline addition](https://github.com/torvalds/linux/blob/v6.12/kernel/time/hrtimer.c).

Relative deadline addition also caps without wrapping. A saturated futex wait
remains finite; a missing timeout retains the separate infinite-wait sentinel.
Interrupted relative sleeps report the remaining capped deadline, rather than
the unrepresentably large original duration. Absolute sleep interruptions leave
the remainder argument untouched.

Periodic POSIX timers and timerfd forward safely to a capped future deadline.
At or beyond the signed horizon, one terminal refresh accounts for expirations
and disarms the timer, preventing duplicate notifications on repeated refresh.
A refresh can account for multiple missed periods; it does not imply exactly
one expiration. Interval, pending-count and overrun saturation remain distinct.
This terminal behavior is an explicit Brook policy, not a claim of exact Linux
behavior for clocks outside its signed time domain.

## Verification

`scripts/workloads/timeout_boundaries.c` is a finite, static syscall probe. Each
of its 24 child cases has an alarm and a parent watchdog; all children are reaped.
The identical executable passes all cases on host Linux, fails 13 cases with
EINVAL on the original Brook kernel, and passes all 24 after the fix. It covers
relative and absolute waits/timers, caught-signal interruption, malformed input,
a finite futex timeout, and zero/past sleep controls. Separate evidence checks
verify positive normalized relative remainders bounded by INT64_MAX nanoseconds
and unchanged absolute remainder sentinels.

Executable SHA-256:
`1f9378ebb3dcfc18786de23b537696172860b3c31c4c0dbe12ca1947bb46edac`.
Baseline kernel:
`ee4310ff055494f647dcf417e633e9a05becb1d31e2048b066caf73d0dd7aebd`.
Fixed kernel:
`08f01b0230131355154e7e131fc7c0553a91518eb7001862f595f796c1b19e08`.

Hosted tests capture failures before implementation in both affected suites;
all 73 suites pass afterward. Boundary tables exercise the conversion threshold,
invalid fields, finite futex identity, relative addition, periodic forwarding,
terminal refreshes and counter/overrun behavior. The native fixed run also passes
the unchanged eighteen-case timer and thirteen-case signal/restart probes.

Native reviews are `unattended-timeout-boundaries-{baseline,fixed}-mlfq-a-20261021`,
using eight vCPUs/eight GiB, MLFQ/blocking accounting and the original thirteen
ABI-34 modules. Extra timing instrumentation and coalescing are off. All eight
original guest jobs retain complete identity, empty-group and output/EOF
receipts, including the expected baseline probe exit one. Both original VM
owners/controllers exit zero; all eight filesystem checks and runtime,
policy-ledger/ready-count, heap and IRQ gates pass. Evidence is retained in
`build/workloads/timing-scheduler-20261021/timeout-boundaries/verification.json`.

This closes the tested huge-timeout compatibility gap. It does not establish
physical/unstable-clock fallback, arbitrary SMP races, workload latency or a
fix for historical playback acceleration. Scheduler and audio defaults remain
unchanged; broader browser/game runtime acceptance remains open.
