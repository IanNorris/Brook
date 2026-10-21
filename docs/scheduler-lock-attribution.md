# Scheduler lock attribution before a PV waiting experiment

This is the implementation and acceptance contract for the diagnostic, based on the
2026-10-20 source audit and Astra's read-only review. It is not implemented by
`sched kvm status`, and it does not authorize a different scheduler default.
The measured lock delays, failed preemption-deferral comparison, IRQ sampling
bias and native capability checks are in
[audio delivery diagnostics](audio-delivery-diagnostics.md).

## What the current critical sections contain

| Path in `src/kernel/src/scheduler.cpp` | Work under the ready lock | Consequence for attribution |
| --- | --- | --- |
| `RuntimeChargeLocked` | Direct-index runtime ledger update and policy service callback | MLFQ's callback updates counters directly; no process-list scan in the ledger sample |
| `SchedulerKernelRegionEnter/Leave` | Reference/depth/latch updates | `ProcessRef/Unref` are atomic count changes; neither frees a process inline |
| `PickNextLocked` | Policy selection, PID validation, eligibility checks and reinsertion | Bounded by the initial ready count, but can examine multiple candidates; do not assume one queue operation |
| MLFQ `Tick` | Once-per-second promotion of queued lower-priority tasks | Queue-sized operation, unlike ordinary MLFQ enqueue/dequeue/accounting |
| `SchedulerAddProcess` | Publication, policy registration, lifetime and PID lease accounting | Nests ready → all-process → PID locks; another lock's wait can extend ready-lock ownership |
| `SchedulerSwitchPolicy` | Policy reset and restoration of registered processes | Nests ready → all-process; fresh performance boots avoid hot-switch episodes |
| `DoSwitch` | Pick/claim, runtime ownership, current-task and pending-requeue/retire publication | Raw release precedes TSS/syscall-stack setup, profiler CS record and physical context switch |
| `DrainPostSwitch` | Deferred requeue under the ready lock, with reference cleanup in separate paths | Dispatch-to-drain time is not one continuous ready-lock hold |

The normal MLFQ paths do not identify an obvious expensive operation whose
removal explains the measured multi-second waits. This is source evidence, not
a bound on execution time: host descheduling and nested lock waits can suspend
even a short critical section. Error/diagnostic printing and policy replacement
also exist and must be recognizable in a capture. Do not optimize from the
post-STI sample counts as though they were unbiased instruction occupancy.

## Capture protocol

Start with one selected lock, `g_readyLock`, and fixed per-CPU storage. Select
the lock before enabling; do not allocate slots or dereference Process pointers
in the observed path. Each record needs the diagnostic epoch, CPU, local
publication sequence, ticket, phase, call site and available wall/steal samples.

1. Record reservation/waiting after a ticket has actually been reserved.
   Record acquisition only after that ticket is served. Include successful
   `TicketTryClaim` acquisitions and its stale-ticket wait fallback. A failed
   try-claim is neither an owner nor a reserved waiter.
2. Record both normal and raw releases. Take a pre-publication timestamp and,
   if useful, a post-publication timestamp before restoring IF. The post-release
   update belongs only to the releasing CPU's record: clearing a shared owner
   field afterward could erase the successor's publication.
3. Retain bounded slow acquisition/ownership episodes locally. A bounded number
   of contention snapshots may read `next`, `serving`, matching owner/waiter
   records and relevant KVM preemption hints. Avoid sampling every spin.
4. Aggregate/export outside the scheduler lock. Data publication and snapshots
   must use atomic fields and bounded sequence validation. Never wait for an
   odd sequence belonging to a host-descheduled publisher.

Reservation, acquisition and release publication each have a gap relative to
the real operation. Report unmatched, stale or changing snapshots as unknown.
An acquisition timestamp includes any delay after observing its turn and before
recording that observation. A timestamp before advancing `serving` is not the
unlock instant; a wide release bracket cannot identify who caused its delay.

Controls advance epochs without clearing live per-CPU records. A release must
close its original ticket/epoch, even if tracing was disabled or reset while it
was held. Local sequence and epoch identities must prevent a reused ticket from
accepting an old completion. Exhaustion must be explicit and disable further
capture rather than silently wrap identities. Missing clocks, unavailable steal
or hints, dropped records and inconsistent reads remain visible.

## Interpretation

| Matched evidence | Supported interpretation |
| --- | --- |
| Served ticket matches an acquired record | An identified holder was observed |
| Published release advances serving, but matching waiter has no acquisition record | Handoff/admission delay or a publication gap; absence alone does not prove no holder |
| Long ownership interval with a substantial matching steal increase | Reported host descheduling contributed during ownership |
| Long ownership interval without a corresponding steal increase | Guest work or unreported host delay; not proof of expensive guest execution |
| Positive preemption hint for a matched CPU/ticket | Supporting host-preemption evidence at that observation, not an interval-wide state |

Neither a zero hint nor wall-minus-steal alone establishes exact guest execution.
Ready-lock ownership can include waiting for the nested registry/PID locks.
Correlate a slow owner's call site before instrumenting those locks separately.
Do not add another diagnostic lock inside the observed critical section.

## Acceptance before changing lock behavior

Hosted checks must cover normal and raw releases, successful/failed try claims,
stale-ticket fallback, ticket wrap, sequence/epoch exhaustion, enable/reset/off
during waiting and ownership, stale completions, CPU bounds, missing clocks,
and deliberately inconsistent publications. Real concurrent writers/readers must
exercise bounded snapshots without introducing data races or blocking readers.
Any nested selected-lock assumption must be explicit and tested.

Native acceptance must verify default-off behavior, unchanged IF state, ticket
order, existing timeout behavior, ownership/accounting and all pick/claim/off-CPU
safety gates. Use fresh quiet and bounded-host-load cohorts, matched kernel and
modules, exact original process waits, full-window audio criteria, clean shutdown
and saved-data checks. Measure instrumentation cost separately. Retain failures
and unknown episodes; a cleaner capture is not a performance improvement.

Only attribution sufficient to separate these mechanisms justifies a default-off
PV waiting or directed-yield experiment. Per-core queues, kernel preemptibility
and EEVDF remain distinct comparisons; none follows automatically from KVM
advertising the necessary feature bits.

## Implemented first version

`sched lock-trace on|off|reset|status` observes only `g_readyLock`. It is off at
boot and samples no clocks while off and without an older active capture.
Ordinary acquisition and the successful-reservation callback in `TicketTryClaim`
publish reservation and acquisition. Both release paths publish before/after
the existing `serving` increment, before any IF restoration. Neither ticket
admission, the spin timeout nor scheduler policy callbacks change.

Per-CPU records use atomic payload fields and bounded sequence validation. Each
CPU is the sole writer of its slots, with IF masked; controls never clear them.
A local serial distinguishes reuse of the same uint32 ticket. Enable and reset
advance the window. Off preserves the current window and stops new reservations;
older reservations can finish their original window. Reset preserves enabled
state inside its CAS, so a stale external snapshot cannot undo a concurrent off.

Status exports current, largest observed wait and largest observed hold records
for each CPU, sample-valid/steal flags, active CPU mask, lifetime completion/loss/
clock-error counters, and before/after ticket/window observations. Caller tags
are hexadecimal addresses of static caller-name strings in the matched kernel
ELF. Sample order errors and missing samples are retained, not silently converted
to valid durations. Epoch/serial/publication exhaustion never wraps identities.

This first version is not a complete event journal. Maxima may retain different
episodes, so the predecessor and successor needed for a handoff match can be
missing. There are no per-spin KVM hint snapshots. Unmatched or changed rows
remain inconclusive; neither a long hold nor a clean capture establishes a
performance fix. Instrumentation overhead and native loaded attribution remain
acceptance work before considering a PV lock algorithm.

The reducer/publication tests cover ticket reuse, stale completions, in-flight
off/reset, same-CPU reentry rejection, independent max-wait/max-hold retention,
CPU bounds, missing and backwards clocks, publication/epoch exhaustion,
successful/failed/stale-success try reservations and cross-CPU active-bit
preservation. Concurrent atomic publishers/readers and per-CPU episodes with
concurrent controls pass. All 69 hosted suites and the compiled entry audit pass
(twelve supported templates, 56 mutation rejections; four templates remain
unsupported). Astra found no lock-safety blocker and identified the reset/off
control race, which was corrected before the final build.

## First native acceptance

A fresh private headless/muted VM boots the frozen kernel
`a065ff1db0ecf68eb2642449b01ac3a876676fa9e0e040221f321cebae18b4f2`
with thirteen matched ABI-34 modules, eight vCPUs, MLFQ and explicit
IRQ-accounting try. Default status is disabled, epoch zero, active mask zero,
and zero records/counters on every CPU. The controller enables capture and
runs one tracked unprivileged Python job: eight threads each execute 1,000
getpid/stat/short-sleep iterations. The original job exits zero with both output
streams at EOF, no lost bytes and no remaining guest tasks.

After off, observed current records are released, the active mask is zero,
and 707,838 lifetime completions have zero publication losses or clock errors.
Retained slow episodes exist on all eight CPUs. Max observed reservation-to-
acquisition is 32.005 microseconds; max observed acquired-to-pre-release is
30.616 microseconds. The retained ownership intervals have zero reported steal
increase. These are instrumented quiet-probe observations, not an overhead
comparison, service-time proof or reproduction of the seconds-long loaded
stall. The retained predecessor/successor records need not form a matched pair.

Off/reset remains disabled with a newer epoch; on/reset remains enabled with
a newer epoch. IRQ-accounting IF-error counters remain zero. Runtime ledger,
heap, acknowledged guest final sync, QEMU exit, separate original wrapper wait,
four offline filesystem checks and twelve saved-document/four original-profile
comparisons pass. Evidence and caller-name decoding against the exact ELF are
in `build/reviews/lock-trace-native-20261020`. This establishes functional
integration, not complete native raw/normal-release or fairness coverage.

The user live review subsequently starts the earlier established baseline,
without this trace code, with explicit speaker-audio authorization. Do not
build or stage further kernels while that review VM is running. Loaded trace
attribution and instrumentation-cost controls remain the next private work
after handback; this diagnostic is not a scheduling performance fix.

## Offline analysis

`scripts/workloads/analyze_sched_lock_trace.py` consumes a saved status reply.
It merges duplicate slots by CPU/epoch/serial, accepting a partial publication
followed by a compatible completed one, and rejects conflicting identities or
unsupported sample flags. Unknown publications, old windows, partial episodes,
missing clocks, lifetime loss/error counters and reported CPU IDs remain visible.
Known clock rewinds suppress derived intervals and cross-record bounds; missing
clock samples exclude cross-record matching. A reported steal delta larger than
elapsed wall time is retained with no invented nonnegative execution estimate.

Ticket-neighbor matches are candidates, even if only one predecessor is retained.
The analyser reports conditional bounds from the release bracket to the next
acquisition observation. Unretained events or full uint32 ticket cycles are not
ruled out, and acquisition publication delay remains included. It never selects
the nearest of multiple candidates or calls those bounds exact handoff latency.
The quiet native capture has 24 distinct retained events and four such candidates;
their conditional upper bounds are at most 451 ns. This does not explain the
seconds-long host-load stall or establish an uninstrumented performance result.

Caller-name decoding requires both an ELF and the expected kernel SHA-256 for
the captured run. The analyser checks the file against that supplied hash; the
hash argument must come from the capture's independently retained session/build
evidence, not merely from whichever ELF happens to be available. It does not
cryptographically bind a raw status JSON to that session.

For the retained native example, from the repository root:

```sh
python3 scripts/workloads/analyze_sched_lock_trace.py \
  build/reviews/lock-trace-native-20261020/trace-after.json \
  --kernel build/workloads/timing-scheduler-20261020/lock-trace-frozen/BROOK.elf \
  --expected-kernel-sha256 a065ff1db0ecf68eb2642449b01ac3a876676fa9e0e040221f321cebae18b4f2 \
  --output build/reviews/lock-trace-native-20261020/offline-analysis.json
python3 scripts/test_analyze_sched_lock_trace.py
```

Eleven focused tests cover known bounds including overlapping release brackets,
partial/complete duplicate merging, missing predecessors, ticket wrap/multiple
candidates, future/old-window exclusion, clock rewind and lagged steal, unknown
publications, malformed identities/flags, candidate holders and wrong-ELF hash
rejection. Astra's review identified the backward-clock bound issue; the final
analyser excludes those records and retains a regression for either side of the
proposed handoff. This work is offline and leaves the live review VM untouched.

## Prepared loaded comparison

The ignored `build/workloads/timing-scheduler-20261020/lock-cost-preparation`
directory retains a source-only controller, launcher, independent verifier and
hashed plan. None has run a private trial yet. Both launch paths reject execution
before explicit handback, and the controller checks for existing QEMU processes
before creating its trial directory. Syntax checks and the handback rejection
paths pass; these checks do not establish native acceptance.

The proposed order is off/on/on/off on the same frozen `a065ff1d…` kernel and
thirteen matched modules. Each run uses fresh checked disk clones, eight vCPUs,
6 GiB, MLFQ, IRQ-accounting try, the same producer/syscall/decision probes, and
the controlled Quake demo with synthetic music. Twelve bounded host workers
and four sleepers run for thirty seconds on CPUs 0–13; CPUs 14–15 remain free
of these workers. A newly owned private null sink receives SDL/PipeWire audio.
Do not reuse a previous daemon's metadata as proof that it is running.

Compare complete demo host-time brackets, producer gaps, HDA faults and
full-window PCM continuity, alongside worker completion and the existing runtime,
IF, heap, filesystem and saved-data gates. Keep missing END markers, short audio
captures and unknown trace publications as failures or incomplete evidence.
Tracing off must retain zero events; tracing on must expose losses, clock errors
and incomplete publications. The retained maxima still cannot guarantee an
identified predecessor/successor. This measures the cost of enabling tracing in
one compiled kernel, not the cost of compiling it in compared with its absence.
Failure-owner review and native validation remain required before interpreting
any result from the prepared scripts.

The prepared independent verifier rejects empty or duplicate trial selections
and checks the original outer-wrapper wait, tracked job identities, collected
exit, empty task groups, both output EOFs, byte cursors and retained loss receipts.
It distinguishes a single verified trial from the complete off/on/on/off cohort.
Twenty-six offline checks use retained successful job evidence and mutations for
stale identities, nonzero/signalled exits, surviving tasks and incomplete output.
The actual earlier exit-zero game run with no END marker is rejected, as is a
complete run whose stored duration is replaced with the expected duration.
These checks validate evidence handling, not scheduler or audio performance.

While the authorized baseline live review remains running, six read-only audio
counter queries span roughly ten seconds. Independently bracketed host requests
bound the guest/host elapsed-clock ratio to 0.980752–1.003470. `clock status`
reports pvclock, all eight registered CPUs, zero rejected samples and zero rate
adjustments. This excludes a continuous 1.3–1.5× elapsed-clock rate during that
short observation; it does not reproduce or close the intermittent speedup.
The same stream identity/epoch advances submitted/completed counters without a
reported discontinuity, which is not a PCM-continuity or listening test. This
live launcher displays Quake's OpenGL 1.4 renderer, not the controlled GL3
diagnostic cohort. The raw requests, host brackets and clock reply are retained
in `build/reviews/live-timing-baseline-20261020/audio-live-clock-observation.json`.


## Loaded cost comparison checkpoint (21 October)

The current matched capsule is kernel
`fc41f3dbf97797d1f349754303090fee0b6e14976f8350ae717e1636a104d8bb`
with thirteen ABI-34 modules, including vmalloc reclamation and reserved-band
MLFQ expiry. Its complete manifest also includes the generated symbol table.
Two preparatory attempts stopped before QEMU: the first capsule omitted that
table; the second launcher retained an obsolete GPU-module hash. Both failures
remain preserved. Staging now rejects a missing/empty symbol table before
changing staged artifacts, and the corrected launcher checks every frozen
artifact against its manifest rather than a separate historical module hash.

The corrected run retains private evidence beneath
`build/workloads/timing-scheduler-20261021/lock-cost-v3` and its source/owner plan
in `build/workloads/timing-scheduler-20261020/lock-cost-current-v3-20261021`.
Both trials use fresh checked disks, eight vCPUs, 6 GiB, MLFQ with IRQ-accounting
try, synthetic Q2 music, the controlled 150-packet demo and identical diagnostic
settings except ready-lock tracing. Twelve host workers and four periodic
sleepers are bounded to thirty seconds on CPUs 0–13. Audio goes to a separately
owned private null sink.

| Trial | Observed demo interval | PCM during six host seconds | Runtime gate |
| --- | ---: | ---: | --- |
| Trace off, first | 45.211 s | 0.260 s | Pass |
| Trace on, second | 46.262 s | 0.480 s | Fail: one service-floor clamp |

The clamp is retained with identity CPU 1, PID/incarnation 15/15, at guest
38.500 seconds, before the demo START at 45.890 seconds. Its deficit is 16,268
ns. This is not proof that tracing caused the clamp or the playback failure.
The runtime model explicitly handles lagging steal updates using a monotonic
service floor; this trial does not meet the stricter zero-clamp comparison gate.
The cohort stopped after this failure; there are no third/fourth trial results
and no accepted enabled-cost estimate. Both original VM wrappers and the private
audio service exited zero. Both sets of four filesystem checks and saved-data
preservation pass. The off trial's full verifier passes while retaining failed
audio acceptance and an HDA sample-gap fault.

The on trace records 8,891,699 completions with zero reported loss, clock errors,
partial/unknown/inconsistent retained rows, or active publishers at the final
read. Offline decoding against the matched kernel retains 24 events and two
conditional handoff candidates. These limited retained maxima are not a journal
or causal proof. In particular, they do not establish the cost of disabled
instrumentation or the cause of the historical live regression.

### Regression isolation before bisection

The user reports a recent regression and suspects accumulated instrumentation.
First reproduce it with diagnostics disabled, then bisect a demonstrated
same-workload good/bad pair. The earlier `irq-safe-frozen` build already produced
40.370–42.813-second host-loaded demos and failed audio, so comparing those with
a quiet approximately 15.2-second result cannot establish a new regression.

Use matched checked source disks, fresh homes, the same synthetic music/demo,
QEMU/GPU/audio backends, CPU affinity/count, RAM, policy and timer mode. Retain
all defaults and explicitly read back disabled producer, syscall, decision,
ready-lock, mixer, delivery, ready-watch and timer probes. Avoid sampling,
strace, image capture and repeated status polling during the measured interval;
collect start/end evidence after the job drains. Use separate quiet and bounded
host-loaded cohorts, rather than mixing their scores. Repeat endpoint runs in
alternating order before using a performance threshold for commit bisection.

Compare the preserved earlier capsule and current capsule with probes disabled.
Also compare disabled/enabled probes on the same current build to distinguish
runtime instrumentation effects. If a repeatable same-condition build regression
remains, bisect complete kernel/module builds in a separate worktree, preserving
the current branch. Record each actual build identity and keep invalid runs
unclassified; missing markers, timing faults, incomplete jobs or lifecycle
failures must not become performance passes. A compile-time diagnostic-removal
control is a separate experiment if disabled runtime checks remain suspect.


### Diagnostics-disabled historical/current controls

Two completed alternating cohorts use the earlier matched `irq-safe-frozen`
(`112db7da…`) and current complete capsule (`fc41f3db…`). Every trial starts from
fresh checked copies of the same golden source disks and fresh Q2 home, with
MLFQ, IRQ-accounting try, eight vCPUs, 6 GiB, fixed graphics/audio backends and
the same synthetic music/demo. Guest diagnostic controls are read back disabled;
the older kernel reports four newer commands unavailable, which is preserved
rather than described as a successful control operation. QEMU HDA trace flags
are removed. No profiling, screenshots or PCM capture occur during these runs.
Only the original tracked job is polled every two host seconds; its guest log
reader still polls every twenty guest milliseconds. These are guest-observed
marker intervals, not a new independently bracketed host/guest clock-rate test.

| Condition | Earlier first | Current first | Current repeat | Earlier repeat |
| --- | ---: | ---: | ---: | ---: |
| No intentional host load | 15.888 s | 15.747 s | 15.952 s | 18.752 s |
| Twelve bounded host workers plus four sleepers | 44.393 s | 48.175 s | 46.047 s | 46.171 s |

The quiet runs pass runtime accounting, original collected job exits/EOF/empty
groups, original VM/router/null-sink exits, matched artifacts, heap, four
filesystem checks and saved-data preservation. They do not reproduce a large
current-build pacing regression with probes off. No intentional host load is
not a guarantee of an otherwise idle host; the final older quiet run's variation
is retained, not dropped in favor of the first score.

In loaded runs the thirty-second host fixture begins after the controller
observes demo START. It is not a guarantee of load covering the entire demo.
All seventeen host threads have affinity 0–13; all workers complete and original
waits/filesystem/preservation checks pass. Both current runs and the last older
run have clean runtime counters. The first older run has two service-floor
clamps, largest deficit 12,452 ns; the strict zero-clamp comparison gate therefore
remains false. The raw current/earlier mean ratio is 1.0404, not an accepted
4-percent regression estimate. All source/status/results and this gate remain
explicit in the independent verifier's output.

Evidence and hashed plans are beneath
`build/workloads/timing-scheduler-20261021/diagnostics-off-v2` and
`diagnostics-off-loaded`, with source launchers/controllers/verifiers in their
sibling preparation directories. An initial quiet launch stopped before QEMU
because its private-audio runtime variable was missing; it remains separately
preserved. The corrected cohort supplies and verifies the newly owned audio
service identity. These results narrow the search: the large host-loaded
slowdown exists in the earlier build without its available probes enabled.
They neither clear all instrumentation nor explain the user's browser report,
audio continuity, or the cost of compiled-in disabled checks. A usable commit
bisect still needs a reproducible same-condition good/bad endpoint pair.

After both cohorts' original owners terminated, a separate native review was
prepared in `build/reviews/live-diagnostics-off-20261021`, from the checked
user-review baseline with original game music and profiles. It uses the current
complete capsule, eight vCPUs/8 GiB, MLFQ/try, normal host PipeWire audio, and all
nine controls verified off. The running user review must be handed back before
rebuilding, staging, shutdown or further automated input.


### Disabled-path source audit during the native review

This audit reads source and saved binaries only; it does not enable probes or
send automated input to the user's review. A disabled `SyscallTimingEnter`
returns after its epoch check, before reading clocks or acquiring the recording
guard. Although `SyscallTimingLeave` itself performs those operations, its
`SyscallTimingCall` destructor calls it only for an enabled ticket. Thus the
ordinary disabled RAII path does not execute them. Ready-lock reservation checks
its selected-lock flag and enabled state before `LockTraceNow`; release checks
active publishers, so it can still finish an already-started observation after
the control is disabled. That completion behavior must be preserved.

Disabled diagnostics still have costs. `RuntimeDispatchLocked` clears its
per-CPU `DecisionDispatchTiming` structure before checking the decision token on
every dispatch. Decision-observer calls and epoch reads remain compiled in.
The older scheduler-event ring also writes unconditionally on queue/switch
operations. These source facts are not measurements of their native cost.
Functional CPU-time/service accounting and KVM steal reads are not disposable
diagnostics: MLFQ policy charging and Linux CPU-clock APIs depend on them.

Saved symbol inventories also show a layout change: the earlier eight-byte
ready lock lives in BSS; the current twelve-byte selected lock lives in data,
adjacent to the PID allocator. This is a lead for cache-line isolation, not proof
of false sharing or a regression. Do not attribute a workload difference to the
probe body while overlooking compiled layout and other module changes.

The retained enabled-trace maximum hold is 1.990270 ms, with 1.972981 ms reported
steal during that observation, at `SchedulerTimerTick` on CPU 3. Its
wall-minus-reported-steal interval is 17.289 microseconds, not exact execution
time. This supports host descheduling while a ready-lock holder exists in that
retained observation. It does not identify which waiter caused an audio stall,
prove complete handoff history, or authorize a lock/yield implementation change
without its own safety and workload checks. The hashed kernel symbol inventories
and selected trace record are saved in
`build/workloads/timing-scheduler-20261021/disabled-probe-audit`.


## Pre-investigation live good endpoint

The user requested moving the search before timing/scheduler iteration. The
branch point is `7d005ae088a7f1ee8c0ca15f45760ae08ed67c19`, committed 20 October
at 00:26 BST, immediately before the first scheduler/timer fixes. A detached
checkout at `/data/Programming/Codex/Brook-pre-timing-7d005ae0` builds that exact
revision with its two pinned required submodules. Its matched kernel is
`916a4ceba52f200d5331d4f3012f378a49cebff87bf8c30a75779809bad815d0`, with twelve
ABI-33 modules. This does not replace the current branch or artifacts.

After the previous review's approved clean shutdown, original owner exit and
four filesystem checks, fresh private clones from the same checked review
baseline booted those artifacts through the historical launcher. The native
review is `build/reviews/live-pre-timing-7d005ae0-20261021`, session
`a3147b54e38247d4afcbdec6eca38a2a`. It uses eight vCPUs/8 GiB and historical RR,
PipeWire's 200-ms requested output buffer, and QOM-confirmed HDA `use-timer=true`.
The old network debug interface lacks `sched policy status`; the host readiness
assertion initially encountered that schema mismatch. The same original VM was
retained, not restarted. Its boot serial independently confirms loaded RR and
its saved kernel/module hashes match the frozen manifest. The original wrapper
handle remains authoritative until handback/shutdown.

The user reports Quake smooth at **13–14 FPS, without audio stutter**, compared
with approximately 11 FPS and slow/stuttering music on the current MLFQ/try,
callback/20-ms run. After closing Quake and running Firefox, the user reports
vastly improved audio/video playback, including at 720p, while still observing
reported dropped frames. These are user observations of live scenes, not a
controlled timedemo or an independently established media progression rate.
They establish a useful historical good endpoint on today's host, but the
kernel, scheduler selection and audio defaults all differ.

Two-second Quake profiles use independently matched kernel/module symbols.
Current slow: 1,596 samples, 7,385 switches, zero drops. Historical good: 1,584
samples, 6,988 switches, zero drops. Non-idle retained stacks include virtio-GPU
command/completion waits in both; the old driver also samples its command spin
lock, whereas current uses the newer command-ownership/completion helper. Counts
are not CPU occupancy because interrupts mask sampling, and scenes/policies/
audio are not matched. Only `290c5646` changes the GPU driver between these
source endpoints; it is a candidate for a later controlled test, not an
identified culprit. The historical two-second Firefox capture has 1,483 samples,
37,584 recorded switches and 9,644 dropped events. It remains explicitly lossy.

Host inventory records Intel i915 render node `/dev/dri/renderD128` as the
selected path, with the secondary NVIDIA GPU on renderD129. `nvidia-smi` reports
a driver/library mismatch; this is retained as environmental context, not
attributed as the cause on the selected Intel path. No intentional CPU load was
active during these live comparisons. A separate next control is prepared with
current kernel/modules but the same historical launcher, RR and audio defaults.
That separates a kernel change from configuration before starting a commit
bisect. Do not stage or switch until the user hands this VM back.

The supplied reference recording exists at
`/home/ian/2026-10-19 00-05-40.mp4`. Its metadata reports creation at
18 October 23:05:40 UTC (19 October 00:05:40 BST), 447.033 seconds, and a
1280x720/30-fps recording stream. Recording FPS is not guest game FPS. The latest
commit preceding that timestamp is `8e1ab908`; timestamp proximity alone does
not identify the actual running kernel. The video remains a further-back
reference if this live good endpoint does not support a reproducible bisect.
