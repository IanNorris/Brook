# Bounded ready-lock owner yield experiment

The [observation-only probe](ready-lock-owner-observations.md) records 82
owner-qualified KVM preemption hints while audio remains badly underfed. That
supports testing a hint, without establishing that descheduled holders dominate
the stall. This experiment is default off and changes neither ticket admission
nor scheduler/audio defaults.

`sched lock-owner on` enables publication/observation. `sched lock-yield on`
then enables at most **one advisory host hint per wait invocation**. Yield
admission requires observation; owner-off atomically clears both control bits.
Ordinary and successful try claims still publish after actual acquisition;
normal/raw releases clear publication before advancing `serving`.

The exact wait ticket now distinguishes `already_granted` checkpoints from
unpublished-owner waiting. A candidate must name another CPU holding the
current serving ticket, have a registered preempted hint, and survive owner
and serving rechecks. IF must be clear before the call. Unsupported or error
returns also consume that wait's budget. The hypercall result never permits
entry: the existing exact-ticket acquire loop remains authoritative. IF changes
and nonzero returns fail validation; the implementation does not silently repair
them. The original spin threshold is an iteration bound, **not an unchanged
wall-time bound**: a host yield can increase elapsed time without many spins.

CPU registration caches the KVM feature-13 capability, instruction choice and
actual LAPIC ID before publishing the steal-record readiness flag. The wait
wrapper uses RAX=11 and RBX=target LAPIC ID, returning RAX. It neither allocates
nor reads CPUID. Intel/AMD advertised vendors select VMCALL/VMMCALL; unknown
vendors refuse. This selection assumes the pinned guest's advertised vendor
matches the execution environment, and is not arbitrary vendor-spoof support.
The scheduler caller uses its post-start CPU identity with interrupts masked.
No HLT, guest wake/kick protocol, scheduler-policy switch or host-global tuning
is introduced.

Disabling is not synchronous cancellation: a waiter past its final enabled check
may still issue its one hint. Drain original workloads before checking stable
cumulative observations, cleared owner publication and reconciled counters.
Saturation, publication/self errors, unsupported/error calls and IF errors all
remain failed gates. A stale target after the final check can affect performance
only. KVM may decline the scheduling hint; a zero return does not prove the
target ran. See the primary [KVM ABI numbers](https://raw.githubusercontent.com/torvalds/linux/v6.12/include/uapi/linux/kvm_para.h)
and [pinned host-side implementation](https://raw.githubusercontent.com/torvalds/linux/v6.12/arch/x86/kvm/x86.c).

## Validation and comparison

All **71** host suites pass. Tests cover actual cached wrapper refusal, Intel/AMD
selection, CPU indices deliberately different from APIC IDs 51/99, unsupported
features/vendors/records, unchanged failure output and no per-call CPUID. The
exact hint-decision helper is exercised for already-granted tickets, IF refusal,
final owner races, unsupported/error budget consumption, mode controls, post-call
IF diagnosis and repeated qualifying observations with only one call. Existing
concurrent ticket/try/wrap and ledger tests also pass. Host mocks do not certify
native hypercall execution or general scheduler safety.

The kernel passes both symbol-table links, independent table regeneration and
thirteen preserved modules/109 exports at ABI 34. Kernel SHA-256 is
`753207aefa8b3fce0b8d695e6dc996562efd5b6efa2f82b431e52b1eb2da2032`.
The frozen wrapper disassembly contains both hypercall instructions and no
CPUID instruction; its prologue/epilogue preserve the callee-saved registers.
This is a bounded source/instruction check, not full compiled-body verification.

The native pair keeps owner observation **on in both arms**, changing only yield
admission. It uses original QEMU/PLE and thirteen modules, MLFQ/AP try, idle
coalescing off, eight vCPUs/eight GiB, original 12-KiB HDA and physical PipeWire
callback audio with 20-ms buffering. A finite forty-second packet-coded OSS
source uses four 4,096-byte fragments; eight host CPU workers share CPUs 0–7 for
twenty seconds. Named I/O-helper exceptions retain CPUs 0–15. Native issuer mask
must be 255. Expensive history/lock/timer/ready recorders stay off.

Require actual hint exposure and zero errors, alongside independently verified
load overlap, useful audio progress, exact original source drain/EOF/group
identity, final runtime/policy/heap checks, original VM owner and four filesystem
checks. Compare guest calls with QMP directed-yield attempts/successes, whose
snapshot windows need not equal the whole enabled interval. More host CPU or
successful yields alone do not establish playback acceptance. Preserve failures;
zero actual hints makes this experiment inconclusive. The results follow below.

## Native result: calls work, useful progress does not recover

Both same-kernel private boots submit and complete exactly **1,764,000** native
audio frames, leave no stream and preserve complete original job/group/EOF
evidence. Both WAV captures and complete audio-query host brackets lie inside
every host worker's measured execution interval.

| Mode | Loaded nominal completion / guest second | Forty-second source drain | vCPU host execution over roughly 19.9 s |
| --- | ---: | ---: | ---: |
| Observation only | 0.10169 | 58.602 guest s | 10.28 CPU-s |
| Observation + bounded yield | 0.05348 | 58.795 guest s | 8.69 CPU-s |

The enabled run records **37** eligible observations, **19** actual calls and
**18** budget-exhausted observations. All calls return zero; unsupported, error,
IF, owner-publication and self counts remain zero. The separate QMP bracket
records **15 directed-yield attempts and 15 successes**, versus zero/zero in
the control. Those host brackets surround the pressure run and differ from the
whole-enabled guest counter interval, which includes setup and drain. Their
counts are not expected to equal 19, or to describe exclusively loaded time.

`already_granted` counts are 17/18 in off/on; missing owner counts remain
67,589/70,365. The former ambiguity therefore explains only a small part of
these particular checkpoint counts. This still does not measure time spent in
either state or prove a descheduled next-ticket waiter. A missing publication
can occur before owner publication or during release/handoff. No waiter registry
or wake protocol has been added.

WAV stores **0.819/0.611 seconds** of PCM during **6.007/6.092 host seconds**
off/on. Approximately 25%/39% of stored millisecond bins are quiet. Decoded
pitch is near 1×; fitted source progression is approximately 0.812/0.702 per
stored PCM second. These short fits are not physical-host playback tempo: WAV
omits inactive-delivery intervals. Same-symbol runs separated by gaps do not
by themselves establish replay.

Three stable disabled snapshots reconcile each CPU's publication/cleanup and
classification totals, with owner zero and no saturation. Native issuer mask
is 255. Final runtime, policy/ledger, heap and IRQ gates pass after original
source and report-export drain. Both original VM owners exit zero and all
eight filesystem checks pass. These gates do not override poor playback.

The narrow experiment demonstrates native advisory calls and host yield
exposure, **not an audio fix**. One variable pair does not establish a causal
regression or quantify the hint's effect. No useful progress recovery supports
promotion, so yield remains disabled; earlier clock-clamp, historical playback
acceleration and severe-contention failures remain open.

Raw evidence lives under
`build/reviews/unattended-lock-yield-{off,on}-a-20261021`.
`reboot-resume/summarize-lock-yield.py` validates original sources, owner/yield
exposure, cumulative native gates, measured host overlap, QMP counter deltas
and original VM/filesystem ownership before reducing
`build/workloads/timing-scheduler-20261021/lock-yield-summary.json`.
