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


### Matched-settings bad endpoint and first bisect candidate

The historical good run completed its approved clean shutdown, original wrapper
exit and four read-only filesystem checks. Current kernel `fc41f3db…` then
booted with the same historical launcher, RR, eight vCPUs/8 GiB, Intel render
node and 200-ms PipeWire/HDA timer settings. Its QOM confirms timer mode and its
nine probe controls read disabled; host `/proc` captures the actual QEMU options
and selected library paths. A first attempt at instance 543 exited before boot
because historical forwarding computes `11237 + 100*instance`, exceeding 65535.
The corrected fresh review uses valid instance 541 and preserves that failure.

In `build/reviews/live-current-historical-settings-v2-20261021`, session
`7a2ece10853342f6bb9fe4e3ded1c674`, the user observes Quake at **11–12 FPS with
slow, stuttering music**. This reproduces the Quake regression while holding
scheduler selection and host/audio settings fixed. Firefox under this particular
control has not been tested; do not infer its result from the earlier MLFQ run.
The two-second Quake profile contains 1,591 samples and 7,282 switches with zero
drops. Its original owner later exits zero after approved clean shutdown and all
four filesystem checks pass. Thus the comparison supplies a useful kernel/module
bisect boundary while the same host can run the historical good build.

A stack-based offline pass finds 21 paired outgoing-poll gaps totaling 930 guest
ticks in the slow capture, versus eight totaling 211 in the good capture. Generic
mutex-stack paired gaps total 596 ticks across 92 pairs versus 13 ticks across 19
pairs. These include both blocked and ready time, are guest tick observations,
and retain capture-edge/same-tick/cross-CPU ambiguities. They do not independently
measure host wall time or mutex grant-to-dispatch latency. The changed GPU mutex
is one concrete lead; poll readiness/wakeup behavior is another. Similar overall
switch counts do not support a large switch-count explosion, but cannot rule out
expensive individual switches. Profiles and machine-readable analyses remain in
the two reviews and `matched-quake-{switch-analysis,gap-by-reason}.json` beneath
the workload root.

A genuine `git bisect` runs only in the detached checkout
`/data/Programming/Codex/Brook-timing-bisect`, using good `7d005ae0` and bad
`8df35535`. Its path filter includes kernel, drivers, shared ABI, CMake and
toolchain source, leaving 46 relevant changes. The primary classification is
Quake rendering; audio is recorded separately because it may have another first
bad commit. No intermediate is marked based on intent or profiling alone.

First candidate `044678fa9a953c07a4d71d7f1afdf289d4d089f0` (20 October 14:59 BST)
builds unchanged with matched ABI-34 modules. Its frozen kernel is
`9c791159f1bbb65cd65a5e33465aa2b49fd9d7f003f433f267da3f4895fbbd94`.
Native review `build/reviews/live-bisect-044678fa-20261021`, session
`c68a4e1f97c94b02bbaf77ec019b8baa`, is running with the same historical settings
and awaits the user's classification. Available probe defaults are verified off;
commands absent at that revision are explicitly retained as unavailable. The
current working branch remains separate and unchanged by bisect checkout.
`historical-regression-bisect.json` and the saved bisect log retain this state.


### First candidate reproduces fast media at normal clock rate

At `044678fa`, the user reports Quake at approximately 10 FPS, music fast but
without stutter, and the game demo faster than intended. Firefox/YouTube also
plays video and audio fast, with normal pitch/faster tempo and a higher reported
dropped-frame rate than the historical successful run. This is a distinct audio
state from the latest kernel's slow/stuttering output. The rendering bisect marks
this candidate bad on the user FPS criterion only; it does not identify a first
bad commit for all audio symptoms.

`q2-fast-capture` retains a clean two-second profile (1,594 samples, 7,242
switches, zero drops) and 265,306 stereo PCM frames during a 6.004-second host
window (6.016 seconds of nominal samples). Around that interval, the same cached
stream identity/epoch advances its native completed-frame counter by 378,960.
The host brackets are 6.643–6.723 seconds; the KVM-backed audio clock advances
6.682889 seconds, bounding its rate to **0.9940–1.0060x**. The completed-frame
counter advances **1.2781–1.2935x** nominal 44.1-kHz progression. The counter is
not itself decoded source progression, sample pitch, or proof of physical output.

A separate bounded guest helper measures actual userspace clocks between
independent host HTTP brackets. Python realtime/monotonic, libc/vDSO and raw
syscall realtime/monotonic/raw/coarse clocks, plus raw gettimeofday, all have
bounds enclosing 1x (approximately 0.9897–1.0160x). The requested two-second sleep
is not assumed accurate: its actual host brackets are 2.539–2.606 seconds.
The same original guest job exits zero with complete output/no live tasks, and
its private local HTTP service is shut down. This particular reproduction does
not show a global userspace/KVM clock speedup or justify blaming LAPIC calibration.
An app-specific calculation or another timing path remains possible.

`firefox-fast-capture` retains a deliberately shorter 500-ms profile to avoid
the previous browser overflow: 402 samples, 12,762 switches, zero dropped events.
Its six-second PCM capture has 265,711 frames. Same-identity stream 49/epoch 7
completes 407,769 frames across host brackets 6.635–7.183 seconds, bounding
counter progression to **1.2873–1.3937x** nominal. The bounds remain wide because
the read-only RPC itself can wait; they are not narrowed using its midpoint.
Normal-pitch fast tempo and this device/clock split support investigating source
content skips/device progression, including the independently known HDA timer
mode discard behavior, rather than claiming a resampling or global clock error.
They do not by themselves prove a decoded track speed or a causal video clock.
All captures leave the display unpaused and preserve the speaker route.

The next rendering candidate is
`01334a6a628e411a4cc120c6b4a1f38e74e2cb21` (20 October 06:09 BST), selected by git
bisect in its separate checkout. Current guest artifacts are frozen private
copies, so selecting source does not change that running VM. No build or staging
starts before its handback. Audio and rendering observations remain separate
in `historical-regression-bisect.json` and the saved bisect log.


The user independently timed 30 seconds of YouTube track progression against a
stopwatch at 24 seconds of wall time: approximately **1.25x** end-to-end playback
speed. This manual measurement has unquantified uncertainty but establishes
source progression separately from the device counters. It is consistent with
the captured counter/clock split, without establishing its cause. After explicit
handback, candidate `044678fa` completed guest sync, the original launcher exited
zero, and all four offline filesystem checks returned zero.

The next candidate `01334a6a` was built unchanged in a fresh per-revision build
directory, twice for its final symbol table. Its private frozen capsule contains
12 matching ABI-33 modules (not the ABI-34/13-module set from the later candidate).
Kernel SHA-256 is
`2224d7aadbcd2f2eb53b2ee6017e43f69bde827e7d97ff23947ccc60d312d1da`.
The second native review preserves the same historical launcher, RR, eight CPUs,
eight GiB, PipeWire buffer and HDA timer settings.


### Second candidate: rendering nearer baseline, fast playback persists

At `01334a6a`, session `8397b8e85cd94537a1a2932bfcf6a4b7`, the user reports
Quake still running fast but approximately 12–14 FPS. This is near the historical
13–14-FPS range, unlike candidate `044678fa` at approximately 10 FPS. Keep the
rendering and playback classifications separate; reported application FPS is
not independent of its clock and does not alone establish wall-time throughput.
No good/bad classification is applied to this candidate yet.

A two-second capture retains 1,589 samples, 6,589 context switches and zero
reported drops. Exact-kernel/module symbol resolution identifies 1,174 idle
samples, 191 inclusive GPU SubmitCommand samples and 161 GPU command-lock spin
samples. The historical good run had 1,202, 196 and 125 respectively. The latest
slow matched-settings run instead had 1,407 idle samples, sleeping command-lock
ownership and 154 inclusive SubmitCommand samples. These are IRQ-biased
observations in unmatched scenes, not CPU occupancy or a quantified speed gain.
The earlier candidate's profile resembles the old GPU ownership behavior while
fast playback remains present. This makes the later GPU sleeping-lock change a
useful rendering lead, without identifying it as causal.

The HDA driver has no source changes between the good endpoint and `01334a6a`.
The mixer changes only its ten-millisecond wake deadline from raw BSP-delivered
IRQ count to `ApicTickCount()`, which now uses elapsed pvclock time. By
`044678fa`, driver changes add retained terminal-fault/publication evidence; the
normal accepted publication predicate and sampled reads remain unchanged.
These source observations do not rule out changed scheduling or servicing
patterns triggering the known host HDA timer-mode discard behavior. The latter
already has an independent synthetic traced control in
`docs/hda-timer-discard-control.md`; the present application reproduction has
not yet received an equivalent causal trace. Historical timer=true/200-ms
settings remain fixed during this bisect rather than silently applying the
new callback/20-ms defaults.

Machine-readable comparison: ignored workload artifact
`timing-scheduler-20261021/live-three-revision-quake-profiles.json`; each review
retains its own module-aware analysis and original raw profile. The VM remains
with the user for Firefox testing; no kernel build or staging occurs.


The user confirms YouTube video and audio also run fast on `01334a6a`, with very
few stutters. The saved 500-ms Firefox profile has 392 samples, 11,555 context
switches and zero drops. Same-identity stream 49/epoch 7 completes 343,305
frames across independent host brackets of 6.007439–6.085792 seconds:
**1.27916–1.29584x** nominal 44.1-kHz progression. The separately sampled native
audio clock bounds **0.99252–1.00547x**. This reproduces the device/clock split
before the later GPU sleeping-lock change. It does not identify the HDA timer
path as causal in this application capture, nor establish a decoded media rate.
Evidence is retained in the review's `firefox-fast-capture` directory.

A diagnostic ownership-only patch is prepared in ignored workload directory
`gpu-lock-isolation-preparation`, with its base-file hash and comparison plan.
It passes `git apply --check` but is not applied or built. It swaps only the
GPU command guard back to historical ticket-spin ownership while preserving
current completion validation, stable timeout and uncertain-DMA quarantine.
This will isolate ownership cost if the bisect points there; a wholesale revert
would confound that result and remove safety fixes. No default change is made.


After explicit handback, `01334a6a` syncs and exits via its original launcher
with code zero; all four offline filesystem checks pass. For the existing
reported-FPS bisect criterion it is marked good (12–14 versus baseline 13–14),
while audio/video pace is explicitly still bad. The overlapping FPS ranges and
non-matched scenes limit this classification; it must be revisited if later
controlled wall-time results disagree. Git selects
`7c3c243670783faf1432bbdc7ab4be79316baf99` next, leaving five revisions after it.


At third candidate `7c3c2436`, the user reports approximately 8 FPS in Quake,
no audio for about ten seconds after startup, then fast audio. The user requests
no further testing on this candidate and continuation of the bisect. Marked bad
for rendering. Guest sync and original wrapper exit complete successfully,
with all four offline fsck checks returning zero. Git selects
`31f52bb7808b1d97b6aa99a072d5e0c95de39e27` next (two revisions after it),
which predates the GPU sleeping-ownership change. No causal conclusion follows
before testing the selected candidate.


Fourth candidate `31f52bb7` is reported at approximately 10 FPS with fast audio
and marked bad for the rendering criterion. This predates GPU sleeping command
ownership, so that change cannot be the sole first regression under this
classification. User-directed shutdown completes guest sync, original launcher
exit zero and four zero-exit offline fsck checks. Next selected candidate is
`88008cdf2532ded9bccfab7956ba6c3e1ab0f6e8`. Its optional EEVDF registration
is not evidence that the live RR-selected VM uses EEVDF; verify boot policy as
for every earlier candidate.


Fifth candidate `88008cdf` returns to a user-reported 13–14 FPS, with fast audio
still present. Its requested two-second profile contains 1,598 samples, 7,658
context switches and zero reported drops. Exact-symbol analysis has 1,307 idle
samples, 174 inclusive GPU SubmitCommand samples and 84 command-lock spin
samples; unmatched-scene/IRQ-bias limitations still apply.

Marking this revision good for reported rendering FPS makes git bisect identify
`31f52bb7` as the first bad revision. **This is a provisional observational
boundary, not an established causal defect.** Its source delta stages TCP reads
in kernel memory before fault-aware copy-out, fixing a demonstrated socket-lock
page-fault deadlock. Offline Quake rendering has no obvious direct dependency
on that receive path. Adjacent repeats with controlled wall-time rendering and
matching scene/configuration must verify this unexpected boundary before any
implementation change. Do not revert the TCP safety fix on this evidence.
Accounting and optional EEVDF registration already exist in the observed good
revision, while the actual policy is verified RR. Fast playback spans both sides.
The running VM is retained pending handback; no build or staging occurs.


### Adjacent repeat contradicts the single-pass boundary

On a fresh `31f52bb7` repeat, the user manually launches Quake and reports fast
audio but 13–14 FPS. This contradicts the earlier approximately 10-FPS result
on that same revision. The single-pass bisect therefore does not establish a
causal TCP regression. Keep the safety fix. Host conditions, workload/config
matching and repeat variability still require controlled comparison.

The first automated adjacent attempt is rejected. An unwritable `/data` test
home fails before game launch; its same-VM retry uses a writable `/tmp` home
but overlaps the user's manual game and exits with only the START marker. The
original retry job is terminal with exit one, complete output and no live group
tasks. Its logs are retained; no measured throughput is accepted. The separate
manual game/VM remains running pending handback. Future unattended runs must
verify no existing game before launching and require both markers.


Before the user-offered host reboot, the unattended retry of the existing
paced demo protocol completes with guest exit zero, both markers, no live job
tasks and a 15.885733844-second guest observation interval. Host HTTP marker
injection was inadvertently removed while dropping the timedemo changes, so
this run is not independently host-validated and the outer comparator rejects
it. The saved controller restores the notification injection (syntax checked,
not native revalidated yet). No rendering-throughput result is inferred.
The manual Quake window is gracefully closed by exact incarnation reference
and all client group tasks are observed exited before the paced retry.
The VM then completes guest sync and original launcher exit zero, and all four
offline filesystem checks return zero. A durable ignored checkpoint and
controller copies are in `timing-scheduler-20261021/reboot-resume`. The offered
host reboot can now be used as an environmental control without discarding the
frozen kernels, private disks, failed attempts or contradictory observations.


### Measurement-mode correction and saved next instrumentation

The user clarifies that manual observations use the menu's normal attract demo.
An early rejected automated attempt used unconstrained `timedemo 1`; these
results must not be pooled. The existing paced fixture uses `timedemo 0` and a
60-FPS cap, with 150 fixed demo records. Demo duration measures source progression
and may be affected by simulation pacing; it is not frame-time throughput.
The user requests actual frame times as the primary rendering metric.

`q2_demo_timing.py` now accepts optional `--marker-url`, requiring independently
returned host timestamps and matching marker names. This removes fragile source
rewriting from the saved controller. Three local tests exercise valid/invalid
acknowledgements and the real process/log loop using a synthetic PAK and fake
engine; no licensed data is in the fixtures. Native HTTP validation remains due.

`tools/profiling/sdl_swap_trace.c` is an optional LD_PRELOAD probe: two guest
monotonic samples per SDL GL swap, buffered records, no per-frame allocation or
I/O, and exit-time CSV with explicit drop/incomplete/clock-error counts.
`analyze_swap_trace.py` rejects incomplete traces, reordered/overlapping frames
and multiple swap threads. It reports frame-completion intervals and swap-call
durations (p50/p90/p95/max), stall counts and aggregate FPS. These are application
swap cadence, not compositor/scanout presentation. Independently validate the
guest clock and observer overhead, match attract mode/configuration, and retain
uninstrumented controls before drawing performance conclusions.

The probe builds with warnings as errors and an actual host fake-SDL executable
produces five records/four intervals; bad quality headers are rejected. No native
Quake probe run or regression repair is claimed. Host-check evidence is in
`timing-scheduler-20261021/swap-probe-host-check`. The VMs remain stopped while
the user prepares to reboot; the durable reboot checkpoint retains the exact
frozen capsules and comparison caveats.


### Host reboot control: same revision, normal observed audio

The host boot ID changes from `99dd72f2-6f59-4efd-8acf-f8bb7f5820b5` to
`f2227bd4-abc2-47dd-b91d-f1fae297a283`. The user requests a single control of
the last-tested build. Review `live-post-reboot-control-31f52bb7-20261021`,
session `360433e02a274d6ea56f672cd1bf880d`, uses the exact frozen kernel
`7753dfde261d93c754cac2dffdd53724c3c65a5c773c7ec8e602bb509881bb2a`, matching
ABI-34 modules and fresh clones of the same seed. No automated game or new
frame-time probe is used; the user launches normal menu/attract mode.
The live QEMU executable, selected machine/CPU/memory/GPU/audio/display options,
selected graphics environment and affinity all match the preserved pre-reboot
historical-settings launch. RR and HDA timer=true are verified.

The user reports that audio speedup is gone and rendering is 12–14 FPS. A
two-second profile has 1,585 samples, 6,567 context switches and zero drops.
An unpaused six-second PCM capture and cached-counter bracket retain same-stream
17/epoch 1 progression of 288,202 frames. Host interval bounds are broad at
6.037066–6.574507 seconds: completed-frame rate is 0.9940–1.0825x nominal, and
separately sampled guest-clock rate is 0.9939–1.0824x. The measured completed
frame/guest-clock progression is near unity, unlike the prior roughly 1.28x
split; no sampled stream discontinuity is present. This is not a decoded-music
rate fit or a causal identification of which host component changed.

This directly weakens a deterministic source-only interpretation of the initial
FPS bisect and supports investigating persistent host/audio/device state and
repeat variability. It does not prove that reboot repairs all rendering or
that the TCP change has no performance effect. Evidence and actual launch
comparison are retained in the review; the user's VM remains running.
