# Desktop and gaming scheduler direction

The user has explicitly prioritized desktop/gaming responsiveness over server
thread throughput, and asked to stop further RR investigation. RR results and
already collected profiles remain evidence; no further RR profiling/tuning is
planned. The working comparison is MLFQ versus experimental EEVDF. No scheduler
default changes in this checkpoint.

Judge a candidate primarily by input responsiveness, frame-time consistency,
continuous correctly paced audio, and usable foreground apps while background
work continues. CPU work batches are a secondary guard against excessive
scheduling overhead and starvation, not the optimization objective. A lower
periodic-sleeper p95 alone does not establish a better desktop scheduler.

Foreground preference must account for the compositor, audio and IPC workers
that an app depends on, rather than boosting only the window-owning thread.
App/group fairness is also a design target: opening a browser with hundreds of
threads should not make the desktop or another app unusable simply by adding
more runnable threads. Any foreground preference must remain bounded so that
background work still progresses. These are acceptance criteria, not features
already implemented by the current EEVDF policy.

Microsoft documents the analogous tradeoff: server quanta generally favor
throughput, whereas clients use shorter quanta overall and a longer quantum
for the foreground thread. See [Microsoft CPU analysis](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/cpu-analysis).
This supports explicit desktop tuning, not copying a Windows quantum formula
or assuming every task benefits from the shortest possible slice.

## Linux slice audit

Brook's current EEVDF is an original global-run-queue implementation. Initial
priority hints select 1/2/4/8-ms requests, and normal tasks use priority 2, hence
4 ms. It has not matched the effective Linux slice configuration. The policy's
service/deadline accounting, global SMP selection and kernel preemption rules
also differ from Linux. The algorithm name alone does not make it a Linux
scheduler implementation or isolate all presentation differences to that code.

The exact upstream Linux 6.18.26
[fair scheduler source](https://github.com/gregkh/linux/blob/v6.18.26/kernel/sched/fair.c)
sets a normalized 700,000-ns base slice and default logarithmic CPU scaling;
eight CPUs produce a nominal 2,800,000-ns base. The
[scheduler syscall implementation](https://github.com/gregkh/linux/blob/v6.18.26/kernel/sched/syscalls.c)
reports each fair task's requested `se.slice` through `sched_getattr`.
These are source facts; the following verifies the actual reference VM.

A fresh private Linux reference uses eight vCPUs, six GiB, the same pinned
QEMU/render node, callback HDA, requested 20-ms native PipeWire buffering and
private 44.1-kHz null output. The new read-only
`scripts/workloads/linux_sched_state.py` checks effective per-thread requests,
policies, nice values and readable configuration without changing them. Its
host smoke is recorded separately and is not substituted for guest evidence.

Linux 6.18.26 reports 2,800,000-ns requests for all 308 observed user threads:
305 normal fair-class/nice-0 and three batch/nice-19 threads. Firefox's threads
therefore also use 2.8 ms in this snapshot. The clocksource is `kvm-clock`,
online CPUs are 0–7 and autogroup's sysctl is enabled. Guest kernel config
includes `CONFIG_HZ=1000`, high-resolution timers, scheduler hrtick support,
dynamic/lazy preemption and fair group scheduling. Compiled support does not
prove that every runtime feature is active. Root-only debugfs values remain
unreadable; they are not inferred from the host.

The first inventory incorrectly tried a thread-subdirectory `autogroup` path;
its recorded errors are retained. The probe now reads `/proc/PID/autogroup`,
with a current-process host smoke confirming that interface. The native
inventory's effective slices, policy/nice values and cgroup paths are unaffected,
but it does not establish individual autogroup memberships or CPU-group weights.
Firefox's separate systemd session scope is visible. Group behavior must remain
a comparison limit until recorded directly, rather than attributing the full
Linux/Brook gap to quanta.

## Same-audio Linux workload reference

The Linux run uses the same static CPU binary, generated clip, 24 CPU workers
for 30 seconds and fresh 800×600 Firefox profile as the Brook controls. It
retains the explicitly documented Linux software-vsync workaround. This is one
warm-host reference, not a repeated statistical result.

- Periodic p95: 2.980 ms; worst periodic delay: 49.327 ms.
- Buffered loaded video presentation: 22.704 callbacks/s for the 24-fps clip.
- Media progression: 1.00164 seconds/page second; one sampled dropped frame.
- Maximum input dispatch-to-DOM upper bound: 20.363 ms.
- Audio: 353,152 PCM frames during an eight-second host capture, inside load.
- CPU work: 6.983 million batches, reported as throughput rather than fairness.

The buffered clock model passes and no trace samples are dropped. Tracked Linux
jobs drain, its original QEMU launcher exits zero with serial powerdown evidence,
and its stopped private ext4 image passes read-only checks. The VM and private
fixture/audio services are stopped. RTKit warnings remain in application logs;
no physical-speaker acceptance is claimed.

Brook EEVDF's approximately 3-ms periodic p95 is already similar in scale to
this Linux run, while its presentation is much slower. Matching the 2.8-ms
request is a worthwhile controlled experiment, not proof that it alone will
close the gap. Compositor/IPC work, blocked dependencies, preemption, per-CPU
placement and application/group scheduling also need separate evidence.

Compare MLFQ and EEVDF using desktop criteria, beginning with a clearly
labelled 2.8-ms normal-request control against the existing 4-ms EEVDF baseline.
Keep frame cadence and input/audio results alongside wakeup latency. Preserve
background progress and bounded starvation; do not promote a candidate solely
for higher batch counts or lower synthetic p95. Record foreground/background
roles and Linux group behavior before claiming equivalent fairness semantics.

Ignored evidence is in
`build/linux-workload-reference-20261020/slice-audit` and
`build/workloads/timing-scheduler-20261020/linux-slice-audit`, including the exact
upstream files, frozen probe, live inventory, configuration, benchmark results,
original launcher receipts and filesystem checks. Earlier Brook comparisons are
in [the six callback/20-ms trials](scheduler-small-buffer-comparison.md).

## Profiling harness checkpoint

A read-only `procs` snapshot timed out after load began in the initial diagnostic
attempt. The same VM/jobs were inspected, closed and shut down normally; no
profile-start request had been made. The failed attempt remains unaccepted.
The controller now collects process names before launching CPU load and permits
30 seconds for the single bounded profiler-start RPC. It retains intent/receipt
handling and never retries an uncertain start.

Corrected diagnostic cohorts export profiles and pass app/runtime/heap,
original-wrapper/routing, four-filesystem and saved-data preservation gates.
The user's RR steering arrived while that cohort was already cleaning up;
its captured artifacts are retained without further RR analysis. The MLFQ
2-second export reports 1,548 samples, 39,953 stored context switches and 1,881
producer drops. Count totals agree, but it is lossy and cannot establish exact
wait-site proportions or CPU service. A shorter MLFQ/EEVDF capture is preferable
if profiling is needed again. These diagnostics are separate from the six
unprofiled controls. Evidence is under `rr-dependency-profiles{,-fixed}` in the
ignored workload directory; the historical directory name is not a plan to
continue RR work.

## Normal-request control build

The [four forward/reverse native controls](eevdf-normal-request-control.md)
show no repeatable desktop benefit from 2.8 ms. They retain a native HDA fault
in one baseline and a partially pre-load audio window in another; neither is
silently accepted as clean loaded audio. The build default remains 4 ms.

`BROOK_EEVDF_NORMAL_REQUEST_NS` is a build-time control, accepted as either
`4000000` (the unchanged default) or `2800000`. It changes only priority-2 and
invalid-priority fallback requests. Hints 0/1/3 remain 1/2/8 ms, and weights,
queue selection, service accounting and boot policy selection remain unchanged.
The top-level build forwards it to the kernel subproject; standalone kernel and
host-test configurations accept the same cache variable. For example:

```sh
nix-shell --run 'cmake -S src/kernel -B build/private-eevdf-28 -DCMAKE_TOOLCHAIN_FILE=/data/Programming/Codex/Brook/cmake/toolchains/kernel-x86_64-clang.cmake -DBROOK_ROOT=/data/Programming/Codex/Brook -DCMAKE_BUILD_TYPE=Release -DBROOK_EEVDF_NORMAL_REQUEST_NS=2800000'
```

Build and stage only while QEMU is stopped, following the normal ABI-34 staging
procedure in [the EEVDF experiment](eevdf-experiment.md). The ABI's whole-ms
`Timeslice` metadata rounds up (2.8 ms reports 3 ms); actual request/deadline and
remaining-service calculations stay in nanoseconds. Kernel preemption still
occurs only at safe user-mode timer boundaries, so the configured request does
not guarantee dispatch at that exact wall-clock interval.

Both values pass the EEVDF core and shared-module suites, including the
100,000-step ownership/fairness models. Added checks preserve the last single
nanosecond of a request through yield/block/enqueue, then renew at exhaustion;
module inspection checks the compiled request and metadata. The 4-ms build's
complete 62 hosted suites pass. Both native builds pass import resolution for
13 modules/106 kernel exports. These checks do not establish desktop performance
or justify changing the selected scheduler or default request.

The native comparison uses one common kernel and twelve common modules. Only
`sched_eevdf.mod` differs in the frozen candidate. Independent build directories
produce debug-path differences and a kernel `.rodata` difference; section hashes
show other modules' allocated sections match. Freezing the common kernel/modules
removes those differences from the workload comparison. Build hashes and section
inventories are retained under the ignored timing-workload directory. Native
workload acceptance is recorded separately from these build/model checks.

## Audio service versus scheduling delay

The [mixer service probe](audio-mixer-service-timing.md) now separates sleep
resumption, service and loop-tail brackets, with recording disabled at boot.
Its first two MLFQ/EEVDF diagnostic boots observe approximately 91–93 ms maximum
late resumption and approximately 46 ms maximum service. Those are elapsed
brackets, not pure runnable delay or CPU work. A short DMA publication hazard
also reproduces independently of the longest mixer episodes. Therefore neither
a good synthetic sleeper p95 nor the absence of a mixer CPU hotspot establishes
continuous desktop audio. Keep device publication safety, blocked wakeup,
ready-to-dispatch latency and host delay distinct in subsequent comparisons.
