# Per-VM PAUSE-exit diagnostic

The [accounting-admission controls](targeted-wakeup-timing.md) locate long
interrupt-side lock-admission chains under verified host pressure. Moving
QEMU's backend threads to another logical CPU pool does not remove them.
This diagnostic separates KVM's handling of guest PAUSE loops from the
instructions and timeout limits inside those loops.

## Narrow intervention

The host's read-only `KVM_CHECK_EXTENSION` for
`KVM_CAP_X86_DISABLE_EXITS` (143) returns mask 15. Stock QEMU 10.2.2's
`-overcommit cpu-pm=on` requests supported MWAIT, HLT, PAUSE and CSTATE
suppression together and can alter MONITOR exposure. That is a composite
control, not a PAUSE-only experiment. See the
[QEMU KVM implementation](https://github.com/qemu/qemu/blob/v10.2.2/target/i386/kvm/kvm.c).

A private build instead accepts `BROOK_KVM_DISABLE_PAUSE_EXITS=0|1` during
KVM architecture initialization, before vCPU creation:

- `0`: no exit-suppression capability ioctl; normal behavior is retained.
- `1`: requests exactly `KVM_X86_DISABLE_EXITS_PAUSE` (mask 4, bit 2), checks the
  capability and ioctl result, and aborts initialization on failure.
- Invalid values and concurrent `cpu-pm=on` are rejected.

Both modes log requested/supported masks and result. Baseline `result=0`
means no suppression ioctl was attempted, not that an ioctl successfully
re-enabled exits. This changes every guest PAUSE loop in that VM, not just
GPU polling. It does not rewrite PAUSE instructions or change their numeric
poll caps. It does not request HLT/MWAIT/CSTATE suppression or modify host-global
PLE settings. The
[Linux capability handler](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kvm/x86.c)
sets the per-VM `pause_in_guest` state; the
[VMX control setup](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kvm/vmx/vmx.c)
uses it to suppress pause-loop exits.

The exact original QEMU derivation is checked before overriding it. The
current Nix registry resolves QEMU 10.2.4, so it is deliberately not used.
The cached original nixpkgs source reproduces QEMU 10.2.2 derivation
`d0awaddl82yi060c0zcwm9q27y9fshhb-qemu-10.2.2.drv`. Other build changes limit
targets to `x86_64-softmmu` and disable documentation. Both experimental modes
use the same resulting executable, not one old and one rebuilt binary.

The four-core build exits successfully. Four paused, diskless startup checks
pass: baseline, PAUSE-only, invalid value and composite-mode rejection. Valid
cases exit through QMP; original process waits complete. These are startup
checks, not application/graphics/audio acceptance. The wrapped executable SHA-256
is `f58ca639e36906b0beee29a5737ec2feab28bea6bbed160b1b20888e4af7f3b9`;
each native VM verifies its actual `/proc/PID/exe` hash and startup capability log.

## First native contrast

Fresh private eight-vCPU/eight-GiB boots use the same frozen kernel
`fc41f3dbf97797d1f349754303090fee0b6e14976f8350ae717e1636a104d8bb`,
thirteen matched modules, original 12-KiB HDA lead, RR and normal GL3 Quake
menu/attract playback. Physical PipeWire output uses callback HDA/20-ms buffering.
Eight finite host workers share CPUs 0–7 with all eight verified vCPUs; the two
known resetting I/O helpers may use 0–15. Mixer/delivery recording, QMP endpoint
counters and a read-only host observer are common. Timer/peer/watch recording
and CPU profiling are off. Accounting admission remains blocking in both runs.

| PAUSE exits | vCPU host execution | vCPU runnable wait | Host scheduling slices | Maximum mixer late resume | Native audio fault |
| --- | ---: | ---: | ---: | ---: | --- |
| Normal | 9.347 CPU-s | 148.606 CPU-s | 1,544,702 | 8.637 s | yes |
| Suppressed | 77.882 CPU-s | 79.363 CPU-s | 29,717 | 17.155 s | yes |

Host deltas cover approximately 19.87 seconds of observer snapshots wholly
inside the nominal twenty-second worker window. All eight vCPU lifetimes match.
The normal run's individual execution ranges from 0.177 to 2.223 CPU-seconds;
the suppressed run gives every vCPU approximately 9.66–9.84 CPU-seconds.
Host-worker batches fall from 4.735 million to 2.586 million: this is a major
redistribution of host CPU time, not equal delivered compute pressure.
Backend runnable wait remains approximately 0.317/0.370 CPU-seconds over
different matching non-vCPU lifetime subsets.

Suppression changes host scheduling substantially but is insufficient to
prevent the audio failure. Both streams record discontinuity; frame ratios
crossing it cannot measure playback tempo. Runtime accounting is clean in both
runs, original app/controller/VM owners exit successfully, heap checks pass and
all four filesystem checks pass per boot. LAPIC boot calibration is similar
(62,523 versus 62,519 ticks/ms). Wide host/guest clock brackets include unity;
they do not establish precise clock rate or timely interrupt delivery.

## Completed accounting contrast

The remaining cells retain that exact binary/capsule and common light probes,
selecting existing AP accounting-try mode, first suppressed then normal exits:

| PAUSE exits | vCPU host execution | Host scheduling slices | Maximum mixer late resume | Maximum service bracket | Application completion per guest-clock second |
| --- | ---: | ---: | ---: | ---: | ---: |
| Suppressed, try | 78.439 CPU-s | 33,800 | 228.584 ms | 6.083 ms | 0.070 |
| Normal, try | 9.238 CPU-s | 1,492,351 | 157.993 ms | 486.106 ms | 0.157 |

Both try cells avoid a native fault/discontinuity, unlike both blocking cells.
Neither restores acceptable delivery. The completion figures cover the loaded
audio snapshot interval; host execution covers a different approximately
19.87-second observation window, not an identical per-frame interval. Normal
and suppressed try cells again allocate host CPU very differently; host-worker
throughput is 4.590 versus 2.557 million batches. One four-cell sequence does
not establish statistical performance differences or an optimal setting.

All try-mode IF-error and stale-ticket-wait counters are zero, with successful
and deferred attempts recorded. Runtime accounting remains clean. Original
app/controller/VM waits, heap checks and all filesystem checks pass for both
try cells. No absence-of-fault result is substituted for application audio
acceptance. No production QEMU, wait loop, accounting default or scheduler
default is changed.

This strengthens the evidence that exit behavior affects host CPU allocation,
while interrupt-side accounting admission contributes separately to long mixer
gaps. It does not identify all remaining producer/dispatch/device delays.
Disabling PAUSE exits is not a proposed production fix on these results.

Source patch, pinned build expression, logs, executable manifest, startup tests
and host capability/context receipts are retained under
`build/workloads/timing-scheduler-20261021/qemu-pause-control`. Native evidence
is under `build/reviews/unattended-q2-pause-exits-{0,1}-20261021`, including
actual executable identity, capability logs, full host observations and original
lifecycle/filesystem receipts. The try cells add `-try` to their review names;
the checked four-cell comparison is `qemu-pause-control/factorial-summary.json`
under the workload root. Host PLE settings remain gap 128/window 4096.
