# Timing and scheduler audit — 20 October 2026

## Current evidence and open work

The chronological entries below retain their original experimental scope.
Later evidence supersedes several early leads:

- An [actual-body audio commitment discriminator](audio-commit-horizon.md)
  reproduces slow simultaneous sources without scheduler contention. Its
  156-case hosted matrix separates late fragment refills/driver reservations
  from asynchronous empty-tail commitment with zero central reservations.
  A guarded host-only ten-ms batch cap fails twenty of forty-eight regular
  candidate cases; it is not promoted to a guest feature. Exact PCM/drains and
  all 75 hosted suites pass, while real mixed playback and historical runtime
  failures remain open. The next mixer direction needs both a bounded global
  commit horizon and sufficient refill slack, not fictitious silence credit.

- [Shared IRQ registration](shared-irq-routing.md) has confirmed vector-collision,
  duplicate-dispatch and empty-line reactivation defects. Actual-body host tests
  go from four failures to passing, all 75 hosted suites pass, and the corrected
  native boot assigns IRQ 10/11 distinct vectors with no duplicate disk callback.
  A matched diagnostic cohort completes with clean original jobs, filesystem,
  HDA and current runtime checks. Selected BSP gap maxima fall from 77.7/50.5 ms
  to 14.2/14.2 ms and retained slow worker episodes disappear, while default-path
  mixed audio remains underfed. This is a routing repair, not full playback
  acceptance or a scheduler/audio default change.
  A subsequent eight-phase comparison with all eight heavy probes disabled
  finds no consistent overall performance improvement. Default-path concurrent
  audio remains about 0.61/0.53 nominal; Firefox alone is about 1.0. Both arms
  complete lifecycle/media/filesystem checks, but the fixed arm fails runtime
  acceptance with one 13.463-μs clamp. The [reduced-probe results](shared-irq-routing.md#reduced-probe-control)
  retain the non-improving phases and instrumentation limits.
  A separate [zero-status NIC IRQ filter](shared-irq-routing.md#separate-network-wake-filter)
  removes network-worker wakes caused solely by other devices sharing the line.
  Actual-body tests reproduce 257 failures before the guard and pass afterward;
  all 75 hosted suites and one fresh native mixed cohort's lifecycle, media,
  filesystem, HDA and runtime checks pass. That cohort does not materially
  change default mixed audio completion (~0.61/0.53); general performance and
  historical runtime failures remain open.

- [Targeted wake joins](audio-delivery-diagnostics.md#separating-deadline-ready-dispatch-and-worker-resumption)
  match seventeen slow audio-worker cycles to ready records and BSP timer scans.
  Deadlines are 21.62–91.09 ms overdue at handler arrival, followed by only
  4.30–13.95 μs queued and 1.97–22.33 μs to worker resumption. Ten associated
  reservations lie inside the audio-rate brackets. This locates these delays
  before timer-handler arrival, without distinguishing interrupt masking,
  delivery or host delay. Two unmatched cycles stay unknown. Runtime acceptance
  fails two clamps; no lead/default changes.

- [Positive native debt history](runtime-debt-observation.md#first-complete-positive-native-history)
  finally captures one 17.89-μs episode across three clamps and two owner changes,
  with retained catchup and exact independent replay. A checked-in actual-ledger
  regression preserves its intermediate debts and 44,619-ns final credit.
  Runtime acceptance remains failed; the mechanism capture does not prove true
  per-task host-steal attribution or waive historical gates.

- [Guarded shared-prefix batching](audio-shared-prefix.md) has a narrow positive
  off/on/off control: enabled finite-source completion is about 0.95 nominal
  versus 0.91/0.86 disabled, with reduced short tails/quiet gaps, exact drains
  and clean current accounting/lifecycle gates. The first real Firefox/Quake
  off/on/off sequence improves concurrent audio completion from 0.63/0.54 to
  0.85/0.70, reversing to 0.62/0.54 when disabled. Video/frame-time improvement
  is unproven, HDA reservations persist, and the final browser/reopen window
  reports a media-read error. An identical fresh repeat reproduces the audio
  reversal but records a 16.7-μs runtime clamp during the first Quake launch,
  before batching is enabled. A reversed on/off/on cohort with observed source
  drains repeats the audio effect (0.84/0.69→0.63/0.54→0.84/0.68), with clean
  current runtime gates and a verified full Firefox media loop. Its explicit
  longer server-write bound and byte/transfer checks are a separate fixture
  cohort; older timeout/media errors remain retained. HDA reservations persist,
  so full audio acceptance and default promotion remain withheld. An early-history
  diagnostic records 12.12 million events without reproducing the clamp; it
  does not clear positive failures. Phase variance and capture granularity stay
  explicit. Review-driven retry/completion tests and the corrected fail-fast
  full build pass all 74 hosted suites.

- [Accepted mix contributions](audio-mix-contributions.md) now classify actual
  receipt prefixes without changing PCM policy. In two native independent-writer
  windows, short positive tails account for 94–99% of missing frames, all with
  substantial in-flight occupancy. All 74 hosted suites and narrow native gates
  pass. This motivates a headroom-gated shared-prefix experiment, not another
  empty-queue wait or a scheduler-default change.

- [Current mixed MLFQ/EEVDF pair](mixed-policy-comparison.md) reproduces nearly
  identical concurrent audio slowdown on both policies, with close/reopen
  recovery and clean current runtime/lifecycle gates. Neither fixes the issue
  and prior failures remain open. Independent producer capacity/refill phase
  is tested in a [configured-barrier capacity control](audio-capacity-control.md);
  phase variance remains and larger buffers are not a fix. Accepted-contribution
  categories are the next discriminator; no default or waiting-policy change.

- [Runtime correction debt visibility](runtime-debt-observation.md) now separates
  known outstanding debt, valid CPU baselines, catchup, discarded epochs and
  ownership crossings without changing charges or acceptance. All 73 hosted
  suites and a bounded native clock/host-pressure test pass. About 4.2 million
  native samples produce no correction; positive native history and all older
  failed gates remain open.

- [Large timeout saturation](timeout-saturation.md) fixes thirteen confirmed
  EINVAL failures for valid huge waits/timers. The same executable passes all
  24 cases on Linux and fixed Brook; all 73 hosted suites and the earlier
  eighteen timer/thirteen restart cases pass. Original job, VM, filesystem and
  runtime gates are clean. This is correctness evidence, not an audio or
  scheduler performance fix.

- [POSIX timer/block admission](posix-timer-wakeups.md) now executes the actual
  timer, block and unblock bodies in arranged hosted interleavings. The same
  eighteen-case ELF passes on Linux and Brook, including masked/ignored
  notifications, overruns, timerfd rearming and thread targeting, with clean
  original job/VM/filesystem/runtime gates. No production change is needed for
  those cases. Large timeout saturation is addressed above; arbitrary load and
  fallback clock acceptance remain separate.

- [Mixed real applications](mixed-desktop-timing.md) complete Firefox, two
  finite GL3 attract runs and eight guest CPU workers with clean original task,
  VM and filesystem lifecycles. Concurrent stream completion falls near
  0.62/0.54 nominal, returns to 1.00 for Firefox after Quake closes and repeats
  on reopen; CPU load changes it little. Two clamps retain a failed runtime
  gate. [Coordinated channel-separated OSS sources](audio-mix-timing.md) sustain both
  near 0.96–0.98 nominal with exact drains and clean gates; independent writers
  fall near 0.82–0.92 without graphics, with complementary channel gaps and no
  HDA reservations. A [bounded refill-coalescing experiment](audio-refill-coalescing.md)
  exercises actual recovery opportunities with clean narrow gates, but its
  off/on/off comparison has no consistent progress benefit. It remains off;
  source phase and short positive queues need discrimination before broadening
  the policy. The independent debt-reference tests do not waive existing failures.

- [Signal interruption of timed waits](signal-wait-restart.md) fixes confirmed
  SA_RESTART replay of poll/sleeps/timed futex waits and the ordinary-signal
  block-admission race. The unchanged thirteen-case executable passes on host
  Linux, fails eight pre-fix Brook cases, and passes thirteen with the final
  reviewed kernel. All 72 hosted suites pass. Original VM/group/EOF and four
  filesystem gates pass per native arm. Stopped-state handling retains priority;
  the [special after-block SIGCONT follow-up](sigcont-wakeup.md) subsequently
  fixes a missed infinite-futex wake and four spurious read interruptions.
  Its identical eleven-case probe goes from five baseline failures to eleven
  passes; the earlier thirteen-case probe and all 73 hosted suites pass. Final
  original job/VM/filesystem/runtime gates are clean. Wider timer races remain
  follow-up work.
  These correctness results do not establish an audio or frame-pacing fix.
  The matched browser follow-up records two runtime clamps under MLFQ after
  its second pre-close snapshot. Its lifecycle is clean but runtime acceptance
  fails, reinforcing older MLFQ/RR and later EEVDF clamp evidence. A further
  8.59-million-event history capture is negative and does not clear those gates.

- [Bounded owner-directed yield](ready-lock-owner-yield.md) follows positive
  owner-qualified preemption observations. Its matched native pair exercises
  19 guest calls and 15 successful host yields in the separately bracketed
  QMP interval, with clean accounting/IF/lifecycle checks, but loaded audio
  remains around 0.05 nominal versus 0.10 with hints off. It stays default off;
  native yield success is not useful playback or a causal performance estimate.

- [Ready-lock owner observations](ready-lock-owner-observations.md) add a
  default-off advisory descriptor after actual ticket ownership and clear it
  before release. All 71 host suites and a same-kernel native off/on pair pass
  narrow accounting/ownership gates, while audio remains badly underfed. The
  enabled interval records 82 owner-qualified preempted hints; counts span
  setup/load/drain and do not establish a dominant cause. Missing publications
  remain ambiguous. No yield, lock protocol or scheduler default changes.

- [Timer ready-lock site attribution](irq-ready-lock-sites.md) retains complete
  9–18-ms initial-accounting wait observations inside a loaded direct-audio
  interval, mostly reported steal time. The bounded idle-dispatch records fall
  outside that interval and cannot exclude earlier waits. Refined context labels
  subsequently identify a complete 19.981-ms periodic AP-idle accounting wait
  inside poor audio progression. A default-off idle-accounting coalescing
  candidate preserves the later blocking dispatch acquisition and mandatory
  ownership-boundary flush. All 70 host suites and the native build pass;
  same-kernel eager/coalesced/eager pressure remains badly underfed and does
  not justify promotion. A separate branch/lifecycle control exercises both
  next/no-next outcomes and shared AP wake scans with clean accounting and
  ownership gates. Defaults stay unchanged.

- [Direct OSS eight/one/eight-vCPU controls](direct-oss-smp-pressure.md)
  reproduce poor loaded audio progress without Quake rendering. One vCPU
  improves completion to roughly 0.89 nominal per guest second; the eight-vCPU
  reversal falls to 0.23. This supports an SMP-dependent contribution, without
  identifying a lock. Initial timer accounting and idle dispatch acquire the
  ready lock separately and need distinct trace attribution before changing
  their admission behaviour.

- [MLFQ under restricted host contention](mlfq-host-pressure.md) retains
  approximately twenty-second Quake swap stalls with blocking and try AP
  accounting. Try avoids the observed discontinuity but does not restore
  useful audio delivery or rendering. The next discriminator is direct OSS
  without Quake rendering under the same host pressure, with exact progress,
  drain and final runtime gates.

- [Normal-placement policy repeats](scheduler-normal-placement-comparison.md)
  keep original HDA, host placement and the kernel/module capsule fixed. Two
  MLFQ cohorts maintain roughly 10.6–11.6 video callbacks/s under eight guest
  CPU workers, versus 0.13/s on two RR cohorts, with clean runtime gates.
  EEVDF also improves loaded playback, but both cohorts record service clamps
  during a second browser launch and remain unaccepted. The next diagnostic
  must retain those failures rather than infer safety from clean shutdowns.

- [Historical-build live bisect](scheduler-lock-attribution.md) now reproduces
  fast Quake/Firefox playback under the historical RR, HDA-timer and 200-ms
  host-buffer settings. The user measures 30 seconds of YouTube progression
  in 24 wall seconds (approximately 1.25×; manual uncertainty unquantified).
  On two intermediate revisions, cached device completion advances about
  1.28–1.30× nominal while separately bracketed native clocks remain near 1×.
  Forced syscall/libc clock checks on one reproduce near-normal clock rates.
  This is not yet a causal application-level HDA-discard trace. Rendering
  regression bisect and fast-playback classification remain separate.

- [Per-CPU pvclock](pvclock-percpu.md) repairs pause/resume backwards samples;
  elapsed KVM time remains distinct from periodic LAPIC interrupt delivery.
- [HDA near-empty publication repair](hda-near-empty-write-lead.md) fixes the
  publication lead found after the initial diagnostics. Callback HDA avoids
  the independently demonstrated timer-mode source discards; neither repair
  proves that the historical roughly 1.3× browser/game acceleration is resolved.
  A later normal-placement control changes only HDA's write lead from 12 to
  8 KiB: Quake application-audio completion rises from about 0.843 to 0.999
  per guest second on both quiet launches, and from 0.832 to 0.955 during
  eight finite guest CPU workers. The default remains unchanged pending the
  remaining gates. Earlier severe host-contention failures stay unresolved.
- [BSP timer/wakeup measurements](bsp-timer-wakeup-timing.md) separate late
  interrupt entry from wake-scan execution. Independent AP arrivals demonstrate
  opportunities during some long BSP gaps. Host traces support QEMU device/BQL
  blocking as a contributor, without identifying the lock owner or explaining
  every long gap.
- [Optional wake-scan handoff](wake-scan-handoff.md) reduces deadline-to-ready
  lateness. It remains opt-in: repeat EEVDF browser results regress even though
  wake availability improves.
- [Targeted queue activity](targeted-queue-activity.md) shows that some long
  audio ready waits coexist with user execution and other task dispatches.
  Counters are observations, not CPU service or proof of policy eligibility.
- [MLFQ wakeup preemption](mlfq-wakeup-preemption.md) preserves service budgets
  across early interruptions. Four matched native boots under shared scanning
  and four under default BSP scanning improve periodic latency and browser
  presentation at a CPU-throughput cost; audio continuity and broader desktop
  acceptance remain unproven. Exact loaded-event correlations locate several
  long audio delays before ready publication, inside BSP interrupt gaps.
- [Priority query encoding](scheduler-priority-abi.md) now returns Linux's raw
  encoding for default nice zero. Actual per-task nice/RT control remains absent.
- [Small-queue audio controls](audio-small-queue-pacing.md) reproduce slower
  source progression with synthetic tones through Quake and direct OSS, with
  unchanged pitch. Frequent short silence and queue-size interventions support
  a blocking-refill/HDA-lead interaction. Available-prefix blocking writes now
  improve the direct reproduction and pass eight native control/fault cases.
  Producer-triggered native delivery further reduces small-queue silence and
  modestly improves Quake's synthetic-track capture, but four mixed-load boots
  do not establish a consistent desktop benefit. That candidate is preserved
  as an experiment and removed from the default path. Quake's residual gaps
  remain, separate from historical acceleration.
- [HDA delivery diagnostics](audio-delivery-diagnostics.md) now account for
  reserved silence separately from application bytes. In the 4,096-frame
  direct control, 3.981 s of reservations explains nearly all the 4.023 s of
  excess completion time. Quake also shows repeated physical silence gaps.
  The probe is off by default; publication lead, FIFO trailer and guards stay
  unchanged. Observer-effect and representative host-contention controls remain
  necessary before a lead experiment. Four matched observer controls retain
  the same roughly four-second stretching with recording off. Combined host
  and guest CPU load reproduces a genuine sample-gap stop and multi-second
  mixer delays; its empty capture is explicitly rejected as a headroom test.
  Host-only and guest-only isolation now precedes any lead reduction.

- [Physical-output lead controls](hda-near-empty-write-lead.md) now alternate
  12/8/8/12-KiB publication targets on one frozen kernel. They reproduce roughly
  0.84/1.00/1.00/0.84 nominal Quake completion per guest-clock second, consistent
  with separately recorded reserved silence. The smaller target is still an
  experiment: genuine sampling faults occur under verified host contention,
  and runtime floor corrections remain explicit failed gates in some controls.
  A launch-affinity correction invalidates earlier full-VM CPU-reservation
  claims, without discarding their recorded functional results.
- [Verified timer/ready attribution](targeted-wakeup-timing.md) correlates
  multi-second mixer stalls both after dispatch bookkeeping and in the queue.
  Existing per-CPU probes show repeated no-switch timer-handler chains with
  long initial ready-lock-acquisition wall brackets under host pressure.
  A blocking/try/blocking accounting-admission sequence measures worst mixer
  lateness of 7.136/0.176/9.969 seconds, with native audio faults only in the
  two blocking runs. Application delivery remains poor even in the try run;
  a lighter-probe comparison and broader acceptance remain required. This is
  mechanism attribution, not RR quantum tuning or a default-policy change.
  [GPU poll controls](scheduler-lock-attribution.md) separately fail with both
  PAUSE and NOP; zero KVM PV-yield counters do not exclude PLE-driven yields.
- [Per-VM PAUSE-exit controls](kvm-pause-exit-control.md) now isolate suppression
  without changing guest PAUSE loops or host-global settings. A same-private-
  QEMU four-cell experiment gives vCPUs much more host execution when exits are
  suppressed, but audio still faults with blocking accounting and remains badly
  underfed with try accounting. This is not a production workaround. All four
  runtime/lifecycle/filesystem gates pass; application audio acceptance does not.

RR remains the default, with no further RR tuning planned. MLFQ and EEVDF are
optional. Remaining acceptance includes default-path and broader desktop/game
workloads, direct EEVDF eligibility/debt evidence, timing-path retry/signal/load
coverage, and a controlled reproduction of the historical accelerated playback.
Passing one workload or handler-template audit does not close those items.

## Earlier investigation and source map

Later workload follow-up: [TCP receive-copy deadlock](tcp-receive-copy.md)
records a confirmed socket-lock/page-fault dependency, native CoW/EFAULT checks,
the Quake II timing smoke run, and seven backward nanosecond samples. The subsequent
[per-vCPU clock repair](pvclock-percpu.md) localizes those samples to VM
pause/resume and records the replacement clock layout and native checks. [EEVDF experiments](eevdf-experiment.md) records the optional policy,
Linux reference comparison, corrected silent-audio routes and GPU-wait trials.

The [Linux buffer controls](linux-audio-buffer-control.md) reproduce
the SDL slowdown on Linux and improve both guests with a smaller requested
buffer; those experiments left defaults unchanged. The earlier [backend pacing controls](audio-backend-pacing.md) retain a recovered
SDL acceleration lead and a separate SDL callback half-rate discrepancy.

The [native packet-coded controls](audio-packet-code-controls.md) reproduce
roughly 10–12% accelerated source progression without a fitted pitch increase
in timer mode, while callback captures progress near 1×. The subsequent
[timestamped HDA traces](hda-timer-discard-control.md) strongly identify codec
buffer discards as the source of that native control's acceleration. The
launcher now defaults to callback HDA with a 20-ms requested host buffer; RR
remains the scheduler default. The historical larger application factor and
intermittent native discontinuity remain open.

The [six scheduler repeats with the new audio defaults](scheduler-small-buffer-comparison.md)
retain the latency/presentation tradeoff: EEVDF wakes periodic tasks sooner,
MLFQ presents more video, and RR shows browser audio-producer starvation.

The [desktop/gaming scheduler direction](desktop-scheduler-tuning.md) makes
input, frame cadence and continuous audio the primary tuning criteria, stops
further RR investigation, and verifies live Linux 2.8-ms requests against
Brook EEVDF's 4-ms normal baseline. It also documents the isolated build-time
request control and its model/module validation.

[Publication diagnostics and short profiles](hda-publish-and-scheduler-profiles.md)
retain the intermittent HDA fault as unresolved, improve its failure evidence,
and identify address-space validation mutex waits and GPU completion polling
in loss-free MLFQ/EEVDF captures. These counts do not establish wait durations.

[Mapping preflight timings](mapping-preflight-timing.md) now separate initial
validation acquisition wait and check-hold brackets with a runtime probe off by
default. Native EEVDF/MLFQ measurements show sub-microsecond typical checks and
long waiting tails; other holders and grant-to-run delays remain unmeasured.

The [shared-validation prototype](shared-mapping-experiment.md) lowers measured
waiting without establishing a desktop gain, so the working implementation
returns to the original exclusive mutex/ABI 34. The experiment also retains a
real HDA sample gap of about 202 ms and an unresolved sanitizer lock-stress
timeout that also appears with isolated pre-experiment source.

This is an active investigation. The baseline is main `7d005ae0`, kernel SHA-256
`f932600c7ef0ac9606057f8e976e47b3f397695aae69383dc965a6d342e2f1b3`.
The native review initially estimated roughly 1.5× playback in Quake II and
Firefox. A later subjective comparison with the original music suggested
approximately 1.3×; neither estimate is a measured playback factor.
No repair has yet been shown to resolve the playback speedup, and no scheduler
default change has been made.
Later [local audio alignment of the historical recording](playback-recording-rate.md)
supports roughly 1.3× playback with a slower later section. Subsequent live
historical-settings comparisons reproduce fast playback as described above;
the earlier recording analysis and new application observations are separate
evidence with different uncertainty.

## Clocksource versus interrupt delivery

Brook now enables KVM's `MSR_KVM_SYSTEM_TIME_NEW` (`0x4b564d01`) separately
on each CPU, using 64-byte slots in one pinned, sanitized page. Kernel readers
select the current CPU's record; the vDSO uses `RDTSCP` and Brook's CPU index
in `IA32_TSC_AUX`, validates migration and the snapshot version, and falls back
to a syscall if the public format or CPU feature is unavailable.
`KvmClockReadStableNs` additionally checks the advertised stable-clock feature,
per-snapshot stable flag and arithmetic. The vDSO fast path remains MONOTONIC
only. The earlier BSP-only implementation was unsafe across VM pause/resume;
see the [repair and evidence](pvclock-percpu.md).

The KVM clocksource is not an interrupt timer. Per-CPU periodic LAPIC timers
still generate vector 32. Brook calibrates their reload count against PIT
channel 2. The baseline later changes that count using raw delivered BSP
interrupts versus coarse CMOS seconds. The elapsed-clock candidate below
disables that controller on stable-pvclock hosts; fallback hosts retain it.
No TSC-deadline timer programming is implemented.
See the upstream [KVM clock ABI](https://docs.kernel.org/virt/kvm/x86/msr.html)
and [x86 timekeeping explanation](https://docs.kernel.org/virt/kvm/x86/timekeeping.html).

The reviewed boot logged `KvmClock: enabled`, stable flag `0x1`, and LAPIC
calibration 62,591 ticks/ms. Subsequent reload adjustments ranged from 55,705
to 62,591 in the inspected log; adjustment samples included 745 and 1,129
interrupts/second. Those observations are not proof of a 1.5× clocksource error.
A short host-bracketed audio-clock sample advanced 9.037 guest seconds in 9.033
host seconds. It does not exclude intermittent errors or another timing path.

## Source map

Paths below are relative to the repository. The selected source is the KVM path
unless explicitly marked as fallback. This table describes the current
elapsed-clock candidate; historical baseline results follow below.

| Consumer | Source / delivery | Code | Remaining concern |
| --- | --- | --- | --- |
| Syscall MONOTONIC, RAW, COARSE, BOOTTIME | `KvmClockReadNs`; monotonic `ApicTickCount()` fallback | `src/kernel/src/syscall.cpp`, `ClockNowNs` | These elapsed IDs still alias; RAW semantics need separate validation |
| CPU-time clock IDs 2/3 | Generation-keyed task/group running intervals minus available KVM steal | `runtime_accounting.h`, `scheduler.cpp`, `sys_clock_gettime` | Includes kernel/IRQ time; remote commits may lag. Dynamic encoded clocks/TAI reject with EINVAL; precise user/system split remains open |
| libc MONOTONIC fast path | read-only pvclock/TSC vDSO | `src/kernel/vdso/clock.c` | Compare forced syscall, libc, migration and version-update races |
| REALTIME, gettimeofday, RTC display | immutable CMOS epoch plus pvclock elapsed | `src/kernel/src/rtc.cpp`, `RtcNowNs` | CMOS precision is one second; physical/unstable-clock fallback unaccepted |
| timerfd expiry | selected nanosecond clock, lazy readiness refresh | `syscall.cpp`, `timerfd_state.h` | Delivery depends on readiness/wakeup paths; existing timerfd tests are not scheduler latency tests |
| POSIX timer expiry | pvclock; REALTIME uses `RtcNowNs()` | `src/kernel/src/posix_timer.cpp`, `Now` | Epoch/fraction mismatch repaired; actual selector/expiry and arranged admission tests plus eighteen native cases pass; load/fallback and huge-timeout semantics remain |
| Relative nanosleep | monotonic deadline rechecked after each wake | `syscall.cpp`, `sys_nanosleep` | Wake hints use the elapsed-millisecond domain; actual deadline recheck remains authoritative |
| Absolute clock_nanosleep | selected-clock deadline, elapsed-millisecond wake hints | `syscall.cpp`, `sys_clock_nanosleep` | Recheck and request-overflow repair passes short guest regression; signal/load coverage remains |
| futex, poll/select/epoll waits | nanosecond deadlines plus elapsed-millisecond wake hints in several paths | `syscall.cpp`, deadline conversion and waiter loops | Audit each retry, clock selection, signal and overflow path individually |
| alarm and scheduler sleep | `ApicTickCount()`: stable pvclock with preserved boot origin, IRQ fallback otherwise | `scheduler.cpp`, `syscall.cpp` | Delivery still depends on BSP interrupt/wakeup scans; fallback is not a precise wall clock |
| Scheduler slice expiry / policy aging | `ApicTickCount()` refreshed by each CPU; periodic local IRQ delivery | `scheduler.cpp`, `SchedulerTimerTick` | Stable time removes CMOS rebases; kernel non-preemptibility and IRQ delivery still bound responsiveness |
| Legacy user/sys/idle CPU statistics | one charge per delivered local IRQ | `scheduler.cpp`, `SchedulerTimerTick` | Existing ticks/proc/rusage/times remain approximate and distinct from the new running-interval ledger |
| Running-interval ledger | stable KVM elapsed minus paired per-vCPU steal reads; explicit coarse fallback | `runtime_accounting.h`, `RuntimeNow` | EEVDF consumes charged service through policy ABI 2; host/IRQ/fallback limits and native validation are recorded below |
| Ready-queue latency diagnostic | stable KVM clock, incarnation-guarded episodes | `scheduler.cpp`, `ReadyLatencyNow` | Optional; does not measure time waiting for device completion |
| HDA completion | LPIB modulo ring position; stable pvclock bounds ambiguity | `intel_hda_mod.cpp`, `audio_dma_clock.h` | Check native 44.1-kHz progress against host audio capture, underruns and position discontinuities |
| Mixer stream completion | driver receipts attributed by identity/epoch | `audio.cpp`, `Reconcile` | Acceptance/completion are separate; stateless resampling remains a separate concern |
| QEMU host output | selected PipeWire/SDL/none backend | `scripts/run-qemu.sh` | Compare native review backend with silent controlled backend without changing game settings |

### Residual low-level clock reads checked on 21 October

A fresh source search finds no direct consumers of `g_lapicTickCount` outside
the APIC implementation and its compatibility symbol export. `g_lapicRawTickCount`
remains in the APIC IRQ counter and RTC fallback/calibration, distinct from
KVM elapsed time. Raw RDTSC in `process.cpp` and `syscall.cpp` seeds fallback
random generation; `panic.cpp` and `idt.cpp` retain raw crash metadata. The
heap's serialized TSC reads measure optional diagnostic cycles, not elapsed
application time. Kernel pvclock conversion itself samples serialized TSC as
part of the versioned KVM record protocol. These categories are source findings,
not a claim that arbitrary userspace RDTSC consumers have been validated.

`virtio_blk.cpp` still uses permissive `KvmClockReadNs()` for elapsed wait
checks and probe durations; POSIX timers, syscall clock reads and RTC also use
that interface. Its semantics differ from the stable-required audio/GPU and
scheduler reads. Physical/unstable-pvclock fallback, raw/coarse clock semantics,
and all signal/retry/overflow paths remain open acceptance items. No clock
consumer is changed during the user's live bisect test.

## Active scheduler and safety audit

The reviewed boot loads `rr` as the built-in policy: one global FIFO, 10 ms
slices, serialized through `g_readyLock`. `mlfq` registers as a module but is
not selected. It has four FIFOs with 5/10/20/40 ms slices, blocking/yield boosts,
demotion on exhaustion and a periodic boost to HIGH. The standalone
`sched_policy.cpp` implementation is not the selected `SchedOps` policy.

Mechanism ownership must be checked separately from policy queue tests:
`runningOnCpu` claims, pending requeue after leaving a kernel stack, retirement
references, blocked wake races, process stop/exit and the ready/all-process lock
order all live in `scheduler.cpp`. Kernel execution is deliberately not generally
timeslice-preemptible. A latency policy cannot safely change that assumption.

Confirmed module defect: MLFQ `InitProcess` reset a still-queued PID's priority
and links without unlinking its old membership. The RR implementation already
unlinked first. The new common regression replaces head, middle and tail PIDs
at all four new priorities, verifies both neighbours and counts, then requeues
and drains the replacement once. It fails MLFQ before the repair; afterwards
all 51 module checks pass, including the existing 100,000-operation eight-CPU
interleaving model. Those are host policy checks, not concurrent execution of
the kernel context-switch mechanism. No default-policy change follows.

The POSIX realtime timer selector now calls `RtcNowNs()` directly. A new host
regression includes the actual `posix_timer.cpp` and exercises its clock selector
and expiry code with independent monotonic/realtime fractional epochs, a
pvclock second rollover, a realtime rollover and non-KVM fallback. Before the
fix it reports five failures, including an absolute realtime timer expiring
before its deadline; after the fix it reports zero. Relative POSIX realtime
timers select a monotonic basis, so this defect chiefly affects absolute
realtime timers. This is not yet a reproduction of the playback-speed issue.

Initial audit leads (the policy-transition defects are repaired in the later
candidate and validation section):

- Runtime switching clears shared policy storage, then re-registers tasks under
  ready/all-process locks. Audit running, Ready-but-on-CPU, terminated and idle
  identification, held references and unlocked `Timeslice` reads before using
  hot switching as a benchmark technique.
- `Timeslice` is read outside the ready lock although the policy ABI requires
  serialized calls. Establish a safe snapshot together with the slice baseline.
- MLFQ priority zero is demoted on exhaustion in the module despite older docs
  claiming realtime is never demoted. Decide the contract before changing it.
- Yield/block promotion has no accumulated execution budget and can reward
  repeated short yields. Equal priority dispatch-count tests do not establish
  proportional CPU-time fairness, process-group fairness or a starvation bound.
- Tick-based CPU accounting is insufficient evidence for EEVDF virtual runtime.

## Verification and next gates

Ignored evidence is in `build/workloads/timing-scheduler-20261020/host/`:
`mlfq-before.log` (50 pass, one fail), `mlfq-after.log` (51 pass),
`posix-clock-before.log` (five failures), `posix-clock-after.log` (zero), and
`timing-paths-host.json`. `scripts/workloads/timing_paths.py` brackets libc
MONOTONIC with forced syscalls and samples relative and absolute MONOTONIC /
REALTIME sleeps for premature successful returns. Its host smoke passes. The initial live guest attempt found the session had
exited; subsequent tests use private clones, preserving the review originals.

The original baseline kernel's short idle-guest probe found 32/32 libc reads
inside forced-syscall brackets and no early relative sleeps. Four of twelve
MONOTONIC absolute sleeps and five of twelve REALTIME absolute sleeps returned
before the requested duration (largest shortfall 90,567 ns). The candidate now
rechecks the selected clock after every absolute wakeup and rejects arithmetic
overflow while constructing the deadline. It passes all 36 sleep observations
and 32 clock brackets, with no early returns. The candidate probe additionally
records the selected-clock finish against each actual absolute target. These
short idle samples do not reproduce the reported 50% playback speedup.

Candidate kernel SHA-256 is
`1da82d41b44f5ed19039d1059981259c988bce8839ab64a798c737225b3873b6`.
All 58 hosted suites and nine bootstrap-observer checks pass. The existing
compiled-entry supported-template audit and scheduler-start call-contract audit
also pass; neither establishes arbitrary handler-body or SMP scheduler safety.
Guest evidence is retained under
`build/workloads/timing-scheduler-20261020/{baseline,candidate}-observation/`.
The baseline acknowledged sync, exited normally and passed all four primary
offline filesystem checks. The candidate also acknowledged sync, its owned launcher exited zero and all
four primary filesystems passed offline checks. Full saved-profile fixture
verification and browser/game regression remain pending. No mixed-workload or browser gain is claimed.

1. Freeze a baseline and gather host/guest clock brackets, actual native audio
   completion/capture rates and browser media-time progression, without taking
   screenshots/profiles inside measurement intervals. Reproduce the speedup.
2. Correct confirmed clock-domain and early-wakeup defects with independent
   boundary/wakeup tests; compare the same workload and verify clean shutdown,
   filesystem checks and saved-user-data preservation.
3. Test policy queue and SMP mechanism invariants, including lifecycle churn,
   signals, wake races and controlled migration; repair policy switching before
   relying on it. Prefer boot-selected policy comparisons on private images.
4. Establish an eight-vCPU mixed workload: fixed CPU workers, periodic sleepers,
   input-like wakeups and local browser/audio playback. Report per-task service,
   wake/ready latency p50/p90/p95/max, underruns, throughput and scheduler cost.
   Separate warmup, startup and steady state; match visibility and host load.
5. Compare RR and repaired MLFQ. Consider EEVDF with explicit weights, virtual
   runtime, eligibility/deadlines, sleeper placement, lag, preemption, migration
   and overflow/lifetime contracts. Select implementation based on measured
   bottlenecks and safety gates, not only browser smoothness. Keep the default
   unchanged until evidence supports a change.


## First bounded RR saturation results

The `scheduler_mix.c` workload uses fixed CPU loops and four independent
16-ms absolute-deadline sleeper threads. CPU work is counted in identical
32,768-step batches. It skips missed periods rather than generating a catch-up
busy loop. Inputs are bounded to 64 CPU workers and 30 seconds; the initial
runs use eight or 24 CPU workers and ten timed seconds each. QEMU is confined
to host CPUs 0–13 on the 16-logical-CPU host. These runs contain no browser or
audio stream and do not establish mixed real-workload acceptance.

| RR CPU workers | Total batches | Largest/smallest worker count | Sleeper p95 lateness | Worst lateness | Missed periods per sleeper |
| --- | ---: | ---: | --- | ---: | --- |
| 8 | 1,921,931 | 1.0055 | 7.55–7.59 ms | 97.54 ms | 8 |
| 24 | 1,962,419 | 1.0175 | 94.26–95.59 ms | 165.51 ms | 492 |

Each worker made progress; no sleeper returned early. The service-count result
shows even distribution of this fixed CPU work, while periodic latency worsens
with runnable count. It is not per-app fairness or proof that RR alone causes
the latency. Ready-queue accounting spans setup/teardown as well as the timed
interval and must not be labelled a steady-state-only distribution. Coarse
host/RPC clock brackets under load are too wide to infer a small drift change.
The owned launcher exited zero after guest sync; all four primary filesystems
passed offline checks. Evidence is under `rr-load/` in the workload directory.

The first MLFQ comparison intentionally selects policy from INIT.RC before
`SchedulerStart`/AP activation, avoiding live hot switching. Its assertion
caught policy selection failing: registered MLFQ needs 8,240 bytes, but kernel
storage was 8,192. No MLFQ performance result was collected from that RR boot.
The rejected attempt was shut down cleanly and its four primary filesystems
pass. A shared 16-KiB storage contract, compile-time policy checks and hosted
size assertion now cover this mismatch. The assertion fails on the old bound
and all 51 module checks pass on the new bound. The rebuilt guest selects MLFQ successfully before scheduler/AP activation;
its boot log and timing-status reply agree. All 58 hosted suites and both
compiled audits pass again. Matched RR/MLFQ performance verification remains
pending; the first MLFQ run below uses the storage-corrected kernel.


## MLFQ pilot and procfs observation correction

On the storage-corrected kernel
`1471ce537a84563d409a21443acfe2f4abef6718c186af70bbd5589b80c0a48c`,
MLFQ finishes both bounded runs with all CPU workers progressing and no early
sleeper returns. At 24 workers, the pilot gives 2,043,281 batches, a maximum/minimum
worker-count ratio of 1.0246, sleeper p95 21.25–21.26 ms, maximum 63.94 ms and
75 skipped periods per sleeper. At eight workers it gives 1,984,538 batches,
ratio 1.016, p95 up to 20.36 ms, maximum 999.47 ms and 136 skipped periods.
These single trials, on different boots and kernel hashes from the first RR
pilot, do not justify a default switch or a causal performance claim. The
roughly one-second shared outlier still needs explanation. Retained evidence
is under `mlfq-storage-load/`.

The first `lapic_clock_observer.py` run parsed `/proc/uptime` as normal decimal
seconds and reported repeated backward jumps. Inspection found a separate
formatting defect: `GenUptime` prints integer centiseconds without zero padding,
so 226.09 becomes `226.9`. Those apparent jumps are not evidence of actual
counter reversal. Keep the failed diagnostic under `lapic-observation/`.

A second 24-second observation uses explicit `--brook-centiseconds` decoding
of the known legacy field, records raw strings and brackets reads with KVM
monotonic time. With a concurrent, bounded 24-worker/12-second load it takes
5,456 samples, sees zero backward counter steps and zero jumps exceeding the
read-window/quantization bound. Largest per-sample offset change is 24.36 ms;
maximum read window is 0.862 ms. Heap integrity passes afterwards. This does
not rule out early-boot or intermittent CMOS rebasing, and no playback speedup
is reproduced. Evidence is under `lapic-counter-observation/`.

The procfs source now emits both centisecond digits explicitly for uptime and
idle time (its formatter has no width modifiers). The rebuilt RR guest kernel
`51b528c4a0466b591b95e6b55a89b787d0f2874adb757fda31c0cb74463890cb`
passes a six-second, 1,900-sample default-decimal observation with
`--require-centisecond-format`: no malformed fractions, backward observations
or large jumps. The 32 clock brackets and 36 sleep checks pass again. Both
compiled audits pass for that kernel. This validates the procfs formatting
repair, not the reported application playback-speed issue.


## Matched policy comparison and blocking-audio-write defect

The repeated comparison freezes kernel
`51b528c4a0466b591b95e6b55a89b787d0f2874adb757fda31c0cb74463890cb`,
eight vCPUs, 8 GiB, host CPUs 0–13, Venus GPU composition and silent host audio.
Each policy gets private copies of the same checked data/Nix images, is
selected before scheduler/AP activation, and runs worker counts 8,24,24,8 for
ten timed seconds each. Both guests pass default-decimal procfs formatting,
clock brackets and sleep checks. No sleeper returns early, all CPU workers
progress, heap checks pass, launchers exit zero after guest sync and all four
primary filesystems pass offline checks.

| Policy / CPU workers | Sleeper p95 across two runs | Worst lateness across runs |
| --- | --- | ---: |
| RR / 8 | 7.20–7.79 ms | 83.37 ms |
| RR / 24 | 91.07–93.52 ms | 128.11 ms |
| MLFQ / 8 | 21.61–21.66 ms | 52.76 ms |
| MLFQ / 24 | 21.15–22.61 ms | 103.21 ms |

The loaded-case improvement comes with worse lighter-load latency. Do not
switch the default on these synthetic results. The earlier roughly one-second
MLFQ outlier is not reproduced by these matched runs and is not explained.
The retained individual rows and scope are under `matched/summary.json`.
The [policy proposal](scheduler-policy-next.md) sets out EEVDF motivation,
accounting/preemption prerequisites, module lifetime and validation gates.

An audio-only 30-second tone attempt on RR failed after roughly six seconds:
`os.write` returned EAGAIN on a blocking DSP description. Keep the failure
under `matched/rr/audio/w0/`; its sampled stream had no discontinuity flag.
A later traced retry delivered all 1,323,000 frames in 29.753 guest seconds.
Its warmed eight-second host capture contained 353,285 PCM frames, a frame/time
ratio of 1.0004 at 44.1 kHz and no silent 10-ms bins. The maximum four-second
client completion ratio was 1.0236. This does not reproduce a 1.5× playback
error and uses the silent backend rather than the user's PipeWire output.

The failure prompted a focused regression: pause a private stream, fill its
FIFO, clear O_NONBLOCK and start a writer; release space by reset after 1.2 s.
The baseline returned EAGAIN after 546 ms instead of waiting. The syscall had
an unconditional 500-ms LAPIC-tick deadline for blocking output. The candidate
removes that expiry while retaining nonblocking EAGAIN, signal interruption
and explicit device-discontinuity errors. On kernel
`05e4d6840be332129f8bc294dd392fa91f3bebb5f627ed5e2a529db1d5c61d86`,
the full nonblocking write still returns EAGAIN, the blocking writer remains
waiting until reset, and it then accepts exactly four bytes and exits cleanly.
Compiled entry/start audits pass. The idle and 24-worker reruns both deliver
all 1,323,000 frames, taking 29.928 and 29.922 guest seconds. Their warmed
8.006/8.083-second host captures contain 353,059/356,616 PCM frames: frame/time
ratios 1.00004/1.00041, with no silent 10-ms bins. Maximum four-second client
completion ratios are 1.0118/1.0241. Those short-window results do not reproduce
a 1.5× error. Existing DSP ownership and pointer/geometry/drain/signal probes
pass; all tracked lineages drain. A post-exit idle capture has zero frames,
heap integrity passes, the owned launcher exits zero after sync, and all four
primary filesystems pass offline checks.
Evidence is under `dsp-blocking-{before,after}/`. Do not claim this repair
explains the reported playback-speed issue.


## Silent PipeWire fixture for the remaining backend comparison

The current successful tone measurements use QEMU's `none` backend. To exercise
the user's PipeWire path without speakers or desktop-session changes, a
separate temporary PipeWire 1.6.6 server is prepared with only protocol/client
modules, a dummy driver and a null sink. It has its own 0700 runtime directory
and socket; no ALSA, device-discovery or microphone modules are loaded. A
half-second generated host tone is successfully configured and explicitly
linked into that private graph, and its process exits zero. The first unlinked
smoke attempt and metadata-module omission remain retained as fixture setup
failures. Private setup evidence is under `pipewire-null/`.

The same `05e4d684` kernel on RR completes the 30-second native tone in
27.413 s idle and 27.864 s with 24 CPU workers using the private PipeWire
backend. Four-second completion bounds reach 1.1268/1.1118 at the client.
QEMU's eight-second WAV captures still advance at roughly 1x host time, with
no silent 10-ms bins. The captured tone remains close to 440 Hz. These captures
are upstream of the sink and do not prove the sink plays every submitted frame.
Forcing the isolated graph from 48 kHz to 44.1 kHz reduces, but does not remove,
the discrepancy: completion takes 28.714/28.749 s. A further idle run without
WAV recording takes 28.463 s, so recording alone does not explain it.

In that last run, QEMU tracing records 772 HDA occupancy corrections and 33
codec buffer overruns. The corrections comprise 118 positive 1-ms, 258 negative
1-ms and 396 negative 4-ms changes to its timer origin. The net -1.724 s is
diagnostic only: an overrun discards the codec buffer and resets that origin.
The [QEMU 10.2.2 HDA source](https://github.com/qemu/qemu/blob/v10.2.2/hw/audio/hda-codec.c)
implements both behaviors. This is a concrete device/backend lead, not proof
that LAPIC calibration is wrong or that the user's 1.5x symptom is solved.

`BROOK_HDA_USE_TIMER=off` selects QEMU's alternate callback-paced codec for
controlled comparisons; `on` remains the default. It changes the emulated
device behavior, not Brook's clock source or scheduler. The first PipeWire
test VM completes guest sync and QEMU exits zero; its launcher wrapper exits
127 because its live shell source was edited before Bash reached EOF. That
harness error is retained rather than called a clean launcher acceptance.
Independent checks of all four primary filesystems pass. Subsequent launcher
edits must wait until the owning shell exits, as well as QEMU.

Evidence: `matched/rr/audio-pipewire{,-44100,-nocapture}/` and
`pipewire-null/hda-trace-summary.json`. Later controlled workload results follow;
the user's intermittent 1.5x symptom remains unreproduced.

### Matched HDA mode comparison

With host memory headroom below the earlier launch threshold, both modes are
tested with six GiB instead of eight, retaining eight vCPUs, RR, the same
kernel and the private graph forced to 44.1 kHz. QMP confirms `use-timer`
false/true. These are one idle and one loaded trial per mode, not repeated
alternating-order acceptance.

| HDA mode / guest CPU workers | 30-second tone completion | Maximum four-second client ratio |
| --- | ---: | ---: |
| Callback / 0 | 29.981 s | 1.0044 |
| Callback / 24 | 30.024 s | 1.0019 |
| Timer / 0 | 29.263 s | 1.0456 |
| Timer / 24 | 29.410 s | 1.0562 |

All four warmed QEMU WAV captures advance at approximately 1x host time and
have no silent 10-ms bins. This supports further callback-mode testing, not a
default change or a claim that all accelerated playback is fixed. The summary
and scope are retained in `hda-mode-comparison.json`.

Firefox uses a new private HOME/profile in each mode, local generated video,
autoplay enabled explicitly, and no PCM recording during the measurement.
Callback mode advances 28.532 media seconds in 29.913 page-clock seconds
(0.9538x); timer mode advances 20.676 in 20.181 (1.0245x). Both report audio
present and playbackRate 1. Both have uneven frame presentation. HTTP delivery
is delayed/reordered: its arrival timestamps do not establish host-paced FPS
or clock drift. These trials do not reproduce 1.5x playback. Firefox exits,
its tracked descendants drain, heap checks pass and the original profile trees
and named documents remain intact. The callback launcher exits zero after
sync and all four primary filesystems check clean.

The timer-mode host-contention trial adds exactly eight host CPU workers for
20 seconds, with an independent 30-second timeout, confined along with QEMU
to CPUs 0–13 of 16. The guest has 24 bounded workers. Its audio probe reports
EPIPE after 23.809 guest seconds; this is retained as a failed audio test, not
restarted automatically. The guest load drains and the VM remains responsive.
The host benchmark writes complete results, but the observer raises before
recording an authoritative owned host-process exit receipt. Its exit therefore
remains unverified. Guest sync, QEMU and launcher exit, four primary filesystem
checks, and saved document/profile preservation subsequently pass.

The matched callback-mode contention trial completes all 1,323,000 frames in
30.106 guest seconds. Its maximum four-second client ratio is 1.00125 and its
eight-second PCM capture advances at 0.99496x host time. It has a longest silent
run of six 10-ms bins: successful completion does not mean glitch-free output.
The owned host benchmark exits zero. This is one trial per mode, not proof that
callback mode eliminates discontinuities under arbitrary host starvation.
The existing DSP ownership and endpoint probes pass in callback mode, including
reset/close, dup/fork final close, ring geometry/pointer wraps, drain/cancel and
masked/unmasked signal cases. Their tracked task groups drain normally.

### Bounded real Quake II demo

`scripts/workloads/q2_demo_timing.py` consumes a locally supplied licensed
`pak0.pak`/demo, creates a short derived demo only inside a fresh private guest
HOME, and runs the installed GL3/OpenAL wrapper at 1280x720 with timedemo off.
It delimits 150 consecutive original demo records with log markers: nominal
server pacing is 15.1 seconds including the inserted marker record. No game
data is included in the repository. Frame/message counts are checked rather
than assuming an arbitrary demo offset represents elapsed gameplay.

The first attempt times buffered stdout and receives both markers together at
exit. It is explicitly invalid timing evidence. The probe instead observes
`logfile 2`'s per-line-flushed `qconsole.log` at 20-ms guest intervals, retaining
filesystem visibility and reader scheduling as measurement limitations.
The timer-mode interval is 15.440 s and callback intervals are 16.901/16.688 s;
none reproduces accelerated demo playback. The games exit zero and their
tracked descendants drain. Independent guest monotonic/host RPC brackets bound
clock-rate ratios to 0.9988–1.0011 and 0.9958–1.0013 respectively, excluding 1.5x
clock rate during these runs. They do not measure physical music tempo.
Evidence is under `q2-{timer6-log,callback6-log}/` and
`q2-demo-comparison.json`.

The final callback VM passes heap integrity, clean guest sync, owned launcher
exit zero, all four primary offline filesystem checks and the 12-document/four
original-profile preservation audit. The loopback fixture server, route
watchers and identity-checked private PipeWire daemon are stopped. Frozen
orchestration helpers are retained under the ignored evidence root's
`harness/`; paid game assets and derived demo packets remain private. Native SDL
presentation, human listening, repeated alternating-order backend trials and
real mixed-app policy comparisons remain outstanding. RR and timer-paced HDA
are still the defaults.

## Scheduler transition hardening

Kernel `4840083bb12e5242a70b544cee92eb63921f4aa2e546410c6b74c8965b76f5a6`
serializes timeslice reads, expiry decisions, policy registry access and final
ready-count reporting with `g_readyLock`. Registration rejects missing required
callbacks, oversized/zero-sized state and conflicting name/table registrations.
The optional debug callback is checked under the same lock as its invocation.

Replacement restores published tasks with their original `schedPriority`, skips
terminated tasks and PID-zero idle tasks, and enqueues only off-CPU Ready tasks.
The previous name-prefix idle heuristic incorrectly excluded ordinary apps.
Ready tasks still owning a CPU are left to the existing post-switch publication
path. Newborn policy initialization, PID-map publication and first enqueue now
share the birth critical section: replacement cannot clear an initialized task
that has not yet appeared in the all-process registry.

The shared production restore helper is exercised against both actual policy
modules, including off-CPU ownership, blocked/running/terminated states and
priority preservation. Required-callback/state validation is also exercised.
There are 55 passing module cases and all 58 hosted CTest targets pass. Compiled
entry/start audits pass within their documented supported-template scope.

A private eight-vCPU/six-GiB boot completes 24 alternating RR/MLFQ transitions
alongside 24 CPU workers and four periodic sleepers over 20 seconds. Concurrent
churn completes 128 forked children with four threads each (512 child threads).
The kernel census verifies actual `idle-sched-bench-v4` and
`idle-sched-bench-v4_t` task names. Both tracked job lineages drain; heap checks,
guest sync, owned launcher exit zero and all four primary offline filesystem
checks pass. The 12 saved documents (including UID/GID) and four original
profile trees pass the preservation audit. This is a single native switch/churn regression, not an exhaustive
SMP ownership proof or a steady-policy performance comparison.

Earlier fixture failures remain under `scheduler-safety/live-switch{,-v2,-v3}`:
a 120-ms loop can perform zero iterations after being descheduled; `PR_SET_NAME`
currently returns success without changing the kernel name; a symlink is not
sufficient evidence of an executable's census name; and `copy2` requests
unsupported `listxattr`. The accepted v4 fixture uses a plain byte copy, waits
for native `READY`, verifies names from the census and observes all task exits.
These attempts are not counted as passing churn acceptance.

`sched policy status|rr|mlfq` is available through the normal process-context
debug channel for controlled tests; it selects only already registered policies.
The bridge rejects normal guest RPCs after panic. Scheduler-module unload and
unregister quiescence remain unresolved: registered modules must stay resident.
RR stays the default. Evidence and frozen harnesses are under
`scheduler-safety/`, including `summary.json`, the native transition receipts,
the source snapshot and compiled audit reports. Mixed browser/audio/CPU policy
comparisons remain outstanding.

### Elapsed-clock correction: native candidate (2026-10-20)

The legacy scheduler clock could step in both directions. In the diagnostic
MLFQ browser run, adjacent QMP observations show apparent elapsed time falling
892 ms over 104 ms of host time while the raw BSP IRQ counter increases by 117.
The RTC discipline changes the apparent-clock offset by -1009 ms in that pair.
Other retained observations include forward steps exceeding one second. The
frequency controller also treated delayed IRQ delivery as a slow timer and
reduced the LAPIC reload. These are confirmed clock defects; they do not yet
establish the exact cause or factor of the user's accelerated YouTube playback.
Evidence is in `lapic-monitor-mlfq-clock{,-v2}/`. The v2 observer retained its
samples but exited nonzero when the VM stopped during session-lock acquisition;
that stop race is not counted as a clean observer run.

The candidate selects stable KVM pvclock for elapsed milliseconds, preserving
the prior boot origin and publishing an atomic monotonic floor across CPUs.
Scheduler, deadline, compositor, networking and module consumers use
`ApicTickCount()`. LAPIC periodic interrupts still deliver scheduling events;
they no longer define elapsed time when pvclock is selected. RTC code no longer
rebases the counter, and pvclock mode disables IRQ-count-based frequency tuning.
The legacy data export remains for module compatibility. IRQ-based CPU accounting
and the bare-metal fallback remain approximate; this is not a new scheduler or
a TSC-deadline timer implementation.

Four actual-RTC no-rebase assertions fail before the change and pass afterward.
All 58 hosted tests pass, including clock conversion/failure checks and an
8-thread atomic-floor stress test. Compiled entry/start audits pass for the
candidate ELF SHA256
`863dd3a60c7a74f8e7920adf59a45cfade8ceeb8e22d7ac81fbd79cc1a98847d`.

A first native smoke uses eight vCPUs, six GiB, MLFQ, silent audio and fresh
private disks with the matching XHCI module in both the initrd artifacts and
boot filesystem. Across 60 seconds and 12,278 uptime samples there are zero
backwards steps, zero large jumps and zero malformed centisecond fields. Stable
pvclock remains selected, rejected samples and timer-rate adjustments stay zero,
and the timer reload remains its initial 62,527 ticks/ms. Host brackets over
about 55 seconds bound the elapsed-clock rate to approximately 0.990–1.001x.
All 32 libc/syscall clock brackets and 36 relative/absolute sleep checks pass
under load. Twenty-four CPU workers complete a bounded 30-second run; periodic
sleepers have p95 around 24 ms and worst lateness around 43 ms. Unlike the prior
browser pilots, this smoke does not include Firefox and cannot alone prove that
the earlier ~1.16-second mixed-workload outlier is gone.

All three tracked job lineages drain, the heap check completes, the owned
launcher exits zero and all four primary offline filesystem checks pass.
Evidence is under `tick-clock/native/` and the private review root
`build/reviews/timing-mix-tickclock0-20261020`. Browser/audio acceptance and exact
accelerated-playback reproduction remain outstanding. RR remains the default.

The user also supplied a 17.567-second 1280x720/30-fps recording of Thriller in
YouTube. The playback timestamp is hidden; initial audio-onset autocorrelation
has competing peaks and does not justify an exact speed factor. The taskbar FPS
value is smoothed and updated per rendered frame, so rapid visible changes are
not an independent calibrated timer. Analysis artifacts remain local under
`user-speedup-video/`; no recording or licensed media is included in git.


### Same-fixture browser comparison with the elapsed-clock candidate

The original generated 30-second looping clip, fresh Firefox profile, eight
vCPUs/six GiB, fixed 24-worker/30-second load and private 44.1-kHz null sink
were repeated with the elapsed-clock candidate. HDA callback mode remains an
explicit diagnostic setting, not a default change. Screenshot capture occurs
after measurement. These are single pilot runs, not statistical acceptance.

| Candidate policy | Periodic p95 / max | CPU batches / worker max:min | Loaded media/page time | PCM sample duration / 8-s host capture | Input HTTP acknowledgment upper bounds |
| --- | --- | --- | --- | --- | --- |
| RR | 90.1 / 102.3 ms | 6,881,957 / 1.005 | No causally aligned samples | 0.023x | 19.2–20.8 s |
| MLFQ | 22.2 / 119.8 ms | 5,307,734 / 1.031 | 0.961x over 26.8 page seconds | 0.997x | 0.37–1.96 s |

The previous legacy-clock MLFQ browser pilots had shared worst sleeper delays
of 1.163 and 1.168 seconds. The corrected-clock MLFQ pilot no longer shows that
outlier, but this does not establish a bound or reproduce the user’s exact
accelerated playback. RR still suffers severe application audio starvation;
fixing the clock alone does not fix contention. MLFQ improves loaded latency
and audio in this fixture at a substantial throughput cost. RR remains default.

HTTP input acknowledgment includes network and browser processing; it is not
pure device-to-presentation latency. RR's loaded telemetry stalls, leaving no
causally aligned video samples. The WAV contains only periods when audio is
active, so counting stored silent bins alone would hide most of RR's dropout.
An independently buffered page trace and longer clip are the next measurement
improvements. Evidence is under `browser-mix/{tickrr,tickmlfq}/`, with original
controls under `browser-mix/{rr0,mlfq1,mlfq-clock}/`. Both candidate browser jobs
and worker lineages drain; heap checks complete, owned launchers exit zero and
all four primary offline filesystem checks pass. Each review also passes the
12-document/four-profile preservation audit.

Load-average sampling also needed migration: its exact-multiple check could
miss a skipped 5,000-ms boundary or charge twice at the same millisecond.
`ClockTakePeriodicSample` now claims each elapsed boundary once, preserves the
original phase and skips unavailable historical samples. Hosted regressions
cover skipped ticks, duplicate reads, backwards samples, long gaps and wrap.


Migration verification has a specific limitation: guest `sched_setaffinity`
currently succeeds without applying a mask, while `sched_getaffinity` always
reports eight CPUs. A test that only requests different masks would not prove
forced migration or CPU residency. The current clock/sleep probes and global
floor host stress must not be described as per-vCPU clock validation. Actual
residency observation or affinity support is needed for that gate.


### Final clock candidate: RR, default HDA and Quake II

After the load-average gate correction, all 58 hosted suites and both compiled
entry/start audits pass for ELF SHA256
`dd5cc190a495cee5ad49efcc526568a6ccb470c146f7ce358d58d7b648061f2c`.
A fresh private eight-vCPU/six-GiB RR boot with default HDA timer mode repeats
the 60-second observer and bounded 24-worker load. All 13,283 uptime samples
are monotonic with no large jumps; all 32 libc/syscall clock brackets and 36
sleep checks pass. Periodic sleeper p95 is 90.9–92.0 ms and worst is 100.1 ms.

The same boot then runs the owned Quake II demo probe using GL3 at 1280x720.
The 15.1-second expected demo interval is observed at 16.367 seconds including
log visibility/reader scheduling. The game exits zero and its lineage drains.
This is slower than nominal, not a reproduction of accelerated playback; the
original user's reported factor remains unmeasured. No game assets or derived
demo are tracked. Evidence is under `tick-clock/native-final/` and
`q2-clock-final-default-hda/`. The owned launcher exits zero, all four primary
filesystems pass offline checks, and the 12-document/four-profile preservation
audit passes. Further browser trace, actual migration/residency, CPU-time clock
semantics and scheduler runtime/lifetime work remain open.

### Buffered browser trace and longer-clip cohort

The page now retains bounded 250-ms media samples and input-handler/RAF stamps
independently of HTTP telemetry. Key E exports them after CPU load. The analyzer
intersects host-clock response brackets to map page stamps into host intervals,
allowing 2 ms of browser timestamp quantization and explicitly assuming the
separately checked 1x KVM/page clock. This is not physical display latency.
Trace sampling can itself stall: missing samples are reported, not interpolated.

An initial 120-second stream-copy MP4 had its `moov` box after 27.9 MB of media.
RR eventually played it; MLFQ stayed at readyState zero, with no duration, past
the 150-second controller deadline. Inspection of that same live browser after
192 page seconds still showed no metadata, and the server no longer held a
media transfer file descriptor. No CPU load was started in this failed MLFQ
attempt. It was closed and drained, with clean launcher/filesystem/preservation
gates. The failed receipts remain in `browser-mix/tracemlfq1/`; RR's completed
but unmatched diagnostic is `browser-mix/tracerr0/`. Neither is pooled with the
corrected pair. An attempted RR early stop was rejected by its pre-effect guard
because CPU load had begun; no close/input mutation was dispatched.

The corrected pair uses a separate 120-second MP4 with front-loaded metadata,
the same final clock kernel, eight vCPUs/six GiB, identical fresh Firefox launch
source, headless Venus, the same private null audio sink and callback HDA mode.
Both are fresh boots, ordered MLFQ then RR. No clip-end seek occurs during load.

| Metric during the fixed 24-worker/30-second CPU interval | RR (`fastrr1`) | MLFQ (`fastmlfq0`) |
| --- | --- | --- |
| Buffered trace coverage | 17 samples across 12.962 s, largest gap 8.54 s | 114 samples across 29.498 s, largest gap 0.38 s |
| Media progress in that covered span | 0.000 s | 28.811 s (0.977x) |
| Minimum media buffered ahead | 58.31 s | 31.94 s |
| Unplayable sampled ready states | 0 | 0 |
| Input dispatch → DOM bounds | 17.85–20.36 s across four inputs | 0–0.175 s across four inputs |
| DOM → RAF opportunity | 95–104 ms | 0–41 ms |
| PCM frames / roughly 8 host seconds at 44.1 kHz | 0 | 353,152 (~1.000x sample coverage) |
| CPU batches / worker max:min | 6,939,018 / 1.004 | 5,393,439 / 1.029 |
| Periodic sleeper p95 / worst | 91.14 / 97.89 ms | 20.66 / 133.10 ms |

The RR result establishes a real stall despite buffered media, rather than just
delayed HTTP receipts. It does not establish zero progress in the unsampled
parts of the 30-second interval. MLFQ keeps media and input responsive here at
about 22% lower compute throughput; it does not win every latency maximum.
Only one corrected long-clip pair is complete. All app/worker lineages drain,
heap checks complete, owned launchers exit zero, all four primary filesystem
checks pass, and both 12-document/four-profile preservation audits pass.
RR remains the default. EEVDF is the next optional policy trial after runtime
accounting/lifetime safeguards and a matched Linux reference, not an accepted
replacement yet.

`scripts/workloads/run_browser_sched_mix.py` promotes the retained controller
into a reusable parameterized tool. Its generated Firefox source is verified
byte-for-byte against the original cohort launcher and its RR native run passes
all gates above. It records harness/fixture hashes, reuses existing tracked jobs,
retains unresolved input/capture intents and preserves close/shutdown request IDs.
`scripts/workloads/analyze_browser_sched_trace.py` can reanalyze saved evidence
without any VM. Frozen code/fixtures are under `browser-mix-http-fast/harness/`.


### Persistent Linux reference: first native control

`tools/linux-workload-reference` builds a disk-backed NixOS control with the
repository's pinned Linux/apps. `scripts/linux_workload_reference.py` provides
owned QEMU lifecycle, key-only loopback SSH, VM-only input/screenshot transport
and stopped-disk acceptance. Linux boot0 uses the same QEMU 10.2.2, renderer
library hash, Intel render node, Venus/virtio GPU, 1-GiB hostmem, eight vCPUs,
six GiB, 0–13 host affinity, callback HDA and isolated 44.1-kHz sink as Brook's
mixed trials. Linux selects `kvm-clock` and `lapic-deadline`; clocksource and
interrupt delivery are distinct there too. No swap is configured.

The identical static CPU binary (SHA256
`9186eb43eb1673e6734f571dbe779822ec3b0dd4b6ee3263db008c47049253ec`)
is used on both OSes, avoiding compiler differences. The shared Firefox profile
source was extracted into an import-safe module. AST comparison proves the
native controller's flow and source are otherwise unchanged. An initial import
error occurred before launching any Linux browser job and was corrected.

Linux's default Firefox profile decodes near 24 fps and supplies continuous PCM,
but its Wayland visual callbacks stall at ~0.03/s. An independent line-flushed
12-second Weston presentation probe produces 703 frames. The separate software-
vsync diagnostic restores Firefox presentation without changing GPU acceleration.
The later 800×600 profile matches Brook's browser window dimensions. These are
separate profiles on one warmed Linux smoke boot, not alternating fresh-boot or
cold-start comparisons. Linux autogroup and scheduling classes are retained in
the guest environment snapshot; no RTKit service is present.

| Linux Firefox cohort | Periodic p95 / max | Fixed-work batches | Video callbacks/s / media rate | Capture sample coverage |
| --- | --- | ---: | --- | --- |
| Default Wayland vsync, default window | 2.94 / 16.95 ms | 7,049,722 | 0.034 / 1.001x | 1.000x |
| Software-vsync diagnostic, default window | 1.71 / 43.71 ms | 6,585,094 | 22.51 / 0.999x | 1.001x |
| Software vsync, matched 800×600 window | 0.45 / 66.72 ms | 6,558,081 | 23.07 / 1.000x | 1.000x |

The matched profile has zero sampled dropped frames, no sampled buffer starvation
and 800/800 nonzero 10-ms audio bins, with the entire eight-second capture inside
CPU load. These are callback opportunities/captured samples, not human audio or
physical display latency. Fixed-work count ratios are not precise CPU-service
fairness: Linux per-CPU placement and heterogeneous host execution can differ.
Do not attribute the full Brook/Linux gap to EEVDF alone.

The first two controller attempts time out only during app cleanup: Ctrl+Q
opens a confirmed quit dialog, and Weston does not bind Alt+F4 by default. Both
original browser lineages are manually closed and drained, with no replacement
jobs. Their performance evidence remains, but they are not clean controller
passes. The final matched controller completes its known quit-confirmation flow,
and both browser/CPU SSH jobs exit zero. The original output, frozen controllers,
source/fixture hashes, captures and screenshots remain under
`build/linux-workload-reference-20261020/mix-*`.

A private pak exported read-only from a stopped checked Brook disk is staged
outside Nix derivations/git. The same packet timing probe, extended with a direct
Linux engine option, completes GL3 at 1280×720 in 15.440 seconds versus nominal
15.1, with game exit zero and no remaining game process. The engine reports
virgl/Mesa Intel UHD 630 and 44.1-kHz sound. No OST is staged in this control;
this is not a music-matched or uncapped FPS comparison. Neither this nor the
clock-fixed Brook probe reproduces accelerated playback.

The image build emits directory-reading warnings during LKL population. Its
build exits zero and the installed system closure is subsequently verified
read-only from the normal guest user: 962 paths, including all named app
packages, match their registered NAR hashes. The original launcher then exits
zero without timeout after `reboot: Power down`, and offline ext4 checking
passes. A saved root copy includes only private test data; the immutable image
seed contains no licensed pak. The QEMU process is now terminal. No passwordless
sudo, compositor debug access or host desktop control is enabled.

Open controls include repeated fresh boots, startup/cache matching, additional
game throughput and further browser backend comparisons. This first functional
reference advances the performance target; it does not finish the timing/SMP
objective or authorize a default scheduler change.


### Futex deadline validation and early tick wakeups

A focused native probe now distinguishes an actual early `ETIMEDOUT` from a
permitted spurious success. It reads the requested clock after each syscall,
which conservatively detects early returns without subtracting syscall overhead.
The clock-fixed baseline (`dd5cc190...`) returns 18 early timeouts in 216 waits
across relative WAIT and absolute monotonic/realtime WAIT_BITSET. Five of twelve
relative 1-ms waits return early; minimum observed duration is 0.962 ms. Negative
seconds, out-of-range nanoseconds and an unreadable timeout pointer incorrectly
return ETIMEDOUT instead of EINVAL/EFAULT. Evidence remains in
`build/workloads/timing-scheduler-20261020/futex-native0`.

The fix keeps a selected-clock nanosecond deadline, validates the user timespec
with overflow checks, and uses elapsed milliseconds only as a scheduler wake
hint. On an early finite wake it rearms the original linked waiter without
clearing the wake latch. Removing and re-enqueuing would risk losing a concurrent
FUTEX_WAKE. Real wake/removal ownership still decides the result, and infinite
spurious waits must never report ETIMEDOUT (BRO-205).

An intermediate +1-tick guard (`613a5ef2...`, native1) eliminates early errors but
adds an unconditional scheduling tick. It is superseded by rearming without
that guard (`49d73731...`, native2). The latter passes all 216 timeout samples,
all three invalid-argument cases and 96 bounded concurrent real-wake races on
RR. The race probe includes immediate/delayed wakes, 1-ms and 100-ms timeouts,
and four independent address pairs; it checks timely wakes are not lost and
bounded threads drain. This is targeted acceptance, not an exhaustive SMP proof.

The 1-ms relative median is still 1.999 ms (minimum 1.116 ms) in native2, versus
2.002 ms with the guard. These samples do not establish a latency improvement:
correct exact deadlines can miss the next periodic interrupt, especially when
entry follows a previous tick-driven sleep. Sub-millisecond timer programming
and wake latency remain separate work; no busy wait was added to conceal this.
All 58 hosted suites and compiled entry/start mutation audits pass. The RR
native job and launcher exit zero, heap integrity passes, all four filesystem
checks pass, and twelve saved documents/four original profiles are preserved.
The probe source is `scripts/workloads/futex_timing.py`; its Linux-host run also
passes. These VMs use silent `none` audio and do not validate browser playback.

A fresh MLFQ boot of the same kernel (`futex-native3`) repeats the 216 timeout,
three invalid-argument and 96 real-wake cases with zero early timeout failures.
Its 1-ms relative median is 2.002 ms. The native job, tracked lineage drain,
heap check, clean launcher exit and four stopped-filesystem checks pass as on
RR. Before saving the change, original document/profile preservation is checked
on this separate clone too. Neither policy is selected as a new default.


### Permanent scheduler provider ownership

`ModuleUnload` previously freed a module even after it registered SchedOps.
The registry includes inactive policies too, so switching back to RR did not
make unloading MLFQ safe. Registration now first retains the table's owning
module, validates the table, then retains its name and all callback providers
before publishing it. Built-in addresses must lie in the linked kernel image;
other addresses must belong to a published active/initializing module mapping.
The registration dependency set is checked atomically. Existing pins can be
conservatively permanent after a rejected registration; this is a lifetime
contract, not policy unregister or hot replacement.

A short IRQ-safe loader lock reserves distinct slots before blocking file I/O,
publishes mapping ownership before module init, and serializes retention with
unload claims. It is not held across module code or VMM page teardown. An unload
of a retained provider returns failure without invoking its exit or freeing its
pages. Initialization failure after exported callbacks also retains those pages,
marks failed initialization and still reports the original load failure. Metadata
snapshots copy bounded names/version/description under the lock instead of
returning pointers that unload could invalidate. `/proc/modules` exposes a
permanent reference as refcount 1; the module diagnostic marks resident/failed
providers explicitly. This does not establish lifetime ownership for unrelated
device/filesystem registrations or turn borrowed ModuleFind handles into leases.

The production retention/unload/init-completion code is exercised by the host
module suite: 300 concurrent retain/unload races, refusal during claimed page
teardown, active versus initializing providers, multi-provider retention failure
without partial pins, init failure before/after exports, snapshot independence,
and 32 simultaneous unique slot reservations. All 58 host suites and compiled
entry/first-dispatch mutation audits pass.

An initial native boot (`abb3411a...`, `timing-mix-lifetime0-20261020`) refuses
MLFQ unload while selected and after switching to RR, then successfully selects
MLFQ/RR again. Twenty-four additional live policy transitions run with 24 CPU
workers/four periodic sleepers and 128 forked children, each with four threads.
Both tracked jobs exit zero and drain, heap integrity passes, the launcher exits
zero after shutdown, all four filesystem checks pass, and twelve documents/four
original profiles remain unchanged. Performance numbers from this mixed-switch
safety run are not a policy baseline. The final build also copies metadata
strings; its separate acceptance is retained with the final candidate below.

The final kernel (`f072996e...`, `timing-mix-lifetime1-20261020`) repeats both
unload refusals and the 24-switch/128-child/eight-vCPU stress acceptance. Its
tracked `/proc/modules` read reports `sched_mlfq 4096 1 - Live`, confirming the
permanent provider reference is observable. Three guest jobs (metadata, CPU,
churn) exit zero and drain; heap integrity, clean launcher shutdown and all four
filesystem checks pass. Final document/profile preservation is checked on the
stopped clone. Boot-script order and the exact serial refusal/switch events are
saved in `boot-unload-acceptance.json`; workload receipts remain under
`build/workloads/timing-scheduler-20261020/scheduler-safety/policy-lifetime-native1`.
RR remains the default; actual running-interval accounting, CPU-time clocks,
preemption/EEVDF policy ABI and repeated real-workload acceptance remain open.


### Running intervals, KVM steal records and CPU-time clocks

The previous kernel (`f072996e...`, `cpu-time-native0`) returns uptime for
CLOCK_PROCESS_CPUTIME_ID and CLOCK_THREAD_CPUTIME_ID. The native negative control
shows 251 ms of CPU time for a 251-ms sleep, child CPU time near the VM's
37.7-second uptime, and seven failed checks including lost group/thread
semantics. `clock_gettime(-1)` also incorrectly succeeds. The same bounded,
asset-free `scripts/workloads/cpu_time_probe.py` passes on the Linux host.

The new bounded ledger is separate from policy state and legacy delivered-IRQ
user/system ticks. It registers task and group incarnations at birth, accounts
at timer, first-dispatch, ordinary-switch and final-exit boundaries, and lets
CPU-clock reads flush their own CPU. All ledger access uses the ready lock;
there are no Process pointers or per-tick allocations. Group totals retain work
from exited siblings until the group's PID generation is replaced. Fork starts
at zero; normal exec/policy replacement preserve history. Brook nonleader exec
rebinding keeps that thread's history and stops future charges to its old group.
Remote running siblings are committed at their own next boundary, not
extrapolated from possibly stale remote steal data.

KVM CPUID bit 5 gates MSR_KVM_STEAL_TIME registration on each vCPU. One sanitized,
permanently pinned 4-KiB kernel-only page provides 64 aligned records. Versioned
reads are bounded and reject odd/changing versions or reserved flags. The KVM
ABI defines steal as host time the vCPU did not run, excluding vCPU idle; this
is separate from pvclock elapsed time and LAPIC interrupt delivery. References:
[CPUID](https://docs.kernel.org/virt/kvm/x86/cpuid.html) and
[MSRs](https://docs.kernel.org/virt/kvm/x86/msr.html). The ledger costs 43,600
bytes with 1,024 task slots/64 CPU slots, in addition to the 4-KiB steal page.
No Process or SchedOps layout is changed in this stage.

The final sample brackets its elapsed-clock read with steal reads and retries
the entire pair on change, at most eight times. A steal-record change during
the wall read rejects that pair. The host test simulates that interleaving and
a perpetually changing record; it also covers unavailable sources. This does
not make the two separate KVM records one atomic publication or prove that a
stable steal value includes every host delay represented by the wall sample.
Invalid samples
break an interval instead of charging an unknown gap to the next task. Regressing
wall/steal samples are counted and discarded; delayed steal correction is
carried through a monotonic service floor. Such correction can affect precise
per-task attribution if the hypervisor update lags, so it is observable.
The [clamp evidence report](runtime-clamp-evidence.md) retains the raw pairs and
deficit for the first and largest clamp; record stability alone does not prove
that the wall-minus-steal service estimate is monotonic.

Standard clock IDs 2/3 now return measured task/group service. The vDSO already
falls back to syscalls for these IDs. CPU getres advertises 1 ms because remote
group commits are periodic. Unsupported TAI/dynamic encoded clocks return EINVAL;
realtime-alarm reads use the realtime epoch and boot-alarm reads the uptime
basis. POSIX timers currently explicitly accept only realtime/monotonic clocks.
The total includes kernel and IRQ work while a task owns its CPU; it is not
separate precise user/system accounting. `/proc` CPU ticks, getrusage/times and
reaped-user/system statistics still use their previous approximate counters.
Bare-metal/unstable-clock and unavailable-steal modes are exposed, not claimed
as equivalent precision. Read-only runtime diagnostics are normal debug-thread
operations, not a new panic-safe inspection path.

All 59 host suites and compiled entry/start mutation audits pass. The new host
suite compares 100,000 eight-CPU events against an independent elapsed-minus-
steal oracle, plus migration, sleeping intervals, task/group generation reuse,
exited sibling totals, exec rebinding, discontinuities and saturation. Actual
steal-reader code is tested for unsupported CPU/odd version/reserved flags.
The first native RR pilot (`cbbddef0...`, native1) passes the API probe: a 251-ms
sleep charges 0.108 ms to the thread, exited siblings retain 293.8 ms of measured
work in the process total, and a fork child starts around 1.93 ms. All eight
vCPUs register steal records; runtime diagnostics report no invalid/backwards/
stale/clamped samples. These are functional measurements, not hard realtime
latency or a default-policy recommendation.

An initial loaded MLFQ trial (native2) runs 24 guest workers/30 seconds and twelve
host workers/25 seconds, with host/QEMU affinity 0–13 and CPUs 14–15 reserved.
The API checks pass and about 82.5 aggregate vCPU-seconds of steal are observed.
Its interpreter starts after contention begins, so complete-command overlap
cannot be proven; that limitation is retained, not silently treated as matched
coverage. A separate final-kernel gate trial below warms the interpreter first.

The final paired-reader kernel (`0d02e37b...`, native3) warms the interpreter
behind a private host HTTP gate, then starts the 24-worker guest workload and
12 bounded host workers. The gate opens only after all host workers report
ready. Its response and the observed guest completion both precede the first
host worker's end, proving the measurement window lies inside contention. The
API checks pass: 262.9-ms sleep/0.073-ms thread CPU, 276.0-ms busy wall interval/
40.9-ms thread CPU; 81.4 aggregate vCPU-seconds of reported steal are observed.
There are zero invalid, backwards, clamped, stale-identity or saturation events.
The JSON source, gate, worker identities/deadlines, process/thread receipts and
host QEMU task CPU counters are retained. Host and ledger counter snapshots have
different intervals; they are diagnostic evidence, not an exact host/guest
CPU-time equivalence certificate.

Heavy-load latency is still a limitation: the four periodic workers have p95
26.4–28.1 ms and maxima 2.82–3.07 seconds during the churn/fork probe. The load
trial is functional accounting acceptance, not an EEVDF or MLFQ performance
win. BSP-only timeout delivery and long non-preemptible kernel work need
profiling alongside the policy experiment. Do not hide these outliers behind
zero accounting errors or claim EEVDF alone will eliminate them.

A fresh final-kernel RR boot (native4) passes the same API probe after Quake II:
251.0-ms sleep/0.082-ms thread CPU, exited siblings' 240.4-ms total retained by
the process, and a fork child starts near 1.47 ms. All eight steal records remain
active with zero accounting error counters. The GL3/720p demo exits zero and
observes 16.451 seconds for the nominal 15.1-second packet sequence, compared
with the earlier clock-fixed 16.367-second pilot. Neither run reproduces the
reported acceleration; one comparison is not a throughput claim. Licensed game
data remains only in ignored private images/homes. All three candidate probe
cohorts (native1/2/3) and the final RR cohort (native4) drain tracked tasks and
pass heap checks, clean launcher exit, four filesystem checks and saved-data
preservation (12 documents/four original profiles). Native0's expected failing
negative control is also preserved and cleanly shut down.

The next step is the versioned policy ABI/service/preemption integration and an
optional EEVDF heavy-load comparison. RR remains the default; the playback-
speedup reproduction and the full timing/SMP goal are still open.
