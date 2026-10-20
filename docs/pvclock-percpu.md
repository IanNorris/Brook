# Per-vCPU pvclock repair — 20 October 2026

Brook's old shared boot-CPU clock record can return backwards nanoseconds after
QEMU pauses and resumes the VM. Register a separate KVM clock record on every
vCPU and select the current CPU's record in both kernel and vDSO readers.
This repairs a measured clock defect; it does not establish the cause of the
user's intermittent roughly 1.3× Quake II/YouTube playback report.

## Evidence and mechanism

The diagnostic kernel `d8605b4682185b0babd5cd8bcb0bed00db8dd8dc20e353f4b36e24f19111d0a9`
passed sixteen native TCP receive-copy cases with zero backwards runtime-clock
samples. During the following Quake II GL3/720p demo, the bridge captured one
completed frame using a brief VM pause. Immediately afterwards the runtime
ledger recorded seven backwards samples, covering CPUs 1–7 (mask 254), and
none on CPU 0. The largest rewind was 12,462,169 ns. The final recorded ledger
owner was idle PID 0 on CPU 4, rather than a game task.

These observations match stale BSP-record extrapolation: an AP can resume
before the BSP re-enters KVM and refreshes its own clock record. The AP's read
of the old BSP base includes paused host time; a later BSP update corrects that
base and time appears to go backwards. KVM registers clock records per vCPU;
its stable-clock promise does not turn another stopped vCPU's stale record
into a current snapshot. See the primary [KVM MSR ABI](https://docs.kernel.org/virt/kvm/x86/msr.html).
This causal explanation is inferred from the CPU mask, pause timing and ABI;
the native replacement tests below exercise the resulting repair.

The ledger now records maximum wall/steal rewind, affected CPU mask and the last
raw before/after sample, CPU and ledger PID. These are diagnostics, not a time
floor or accounting workaround. Existing invalid-interval discard behavior is
unchanged.

## Implementation

`src/shared/pvclock_page.h` defines the original Brook page format shared by C
and C++: 64 slots of 64 bytes, containing the 32-byte KVM clock ABI plus padding.
Slot zero's padding contains magic/version/stride/capacity/feature metadata;
there are no kernel pointers. The entire page is zeroed before host registration
and stays pinned. Every CPU registers its own physical slot through
`MSR_KVM_SYSTEM_TIME_NEW` before entering the scheduler. Registration is checked
and idempotent. `clock status` exports `pvclock_cpu_mask`.

Kernel readers require the current CPU's registered slot, validate the version
around ordered TSC sampling and retry migration. The stable reader retains its
bounded failure API. The legacy reader retains its retrying API, so a transient
host update does not turn into a returned zero timestamp.

The vDSO validates the public format and RDTSCP feature before executing
`RDTSCP`. Brook writes its CPU index into `IA32_TSC_AUX`; the helper selects that
slot, samples inside the versioned snapshot and retries CPU-tag changes. Bad
metadata, unsupported features, arithmetic failure or exhausted retries enter
the syscall path. Unsupported-RDTSCP hosts use kernel reads. This preserves the
versioned ELF ABI, read-only clock mapping and existing fallback behavior.
It does not add TSC-deadline interrupt delivery or change the scheduler default.

## Validation

Candidate kernel SHA-256:
`a9627f6fb5fcb7fd1f976f5c03138a03c68396f6c8ede195bef8ad4ec372d7a1`.
GPU module SHA-256 remains
`724914fd7298dfb50d89ce1912b3fbd5d8b916ead51541ba983c48a4daf9f4bb`.
All 13 ABI-34 modules were staged together. Each private VM has eight vCPUs,
6 GiB RAM, host affinity 0–13 and a private 44.1-kHz null audio sink.

- All 62 hosted tests pass, including actual clock-reader registration,
  non-BSP selection, migration, stale-BSP resume model, bounded failures and
  rewind diagnostics. Supported entry/start mutation audits pass; previously
  unsupported entry-audit cases remain outside that proof.
- Actual ELF and section-stripped vDSO host executions pass on CPU slot 1,
  including format guards, versioned lookup, arithmetic and syscall fallback.
  Eight ELF-baking validation tests pass.
- RR: eight native readers completed 1,888,811 libc/syscall bracket checks
  over twelve deliberate 75-ms VM pauses. All CPU tags 0–7 appeared, with zero
  backwards reads, bracket errors or rewind magnitude. Runtime accounting also
  reported zero invalid/backwards/clamped/stale/saturated samples.
- RR: the subsequent GL3/720p Quake II run, including completed-frame screenshot
  capture, measured 15.2074 seconds for its nominal 15.1-second interval and
  exited zero. Reader scheduling/log visibility contributes to that interval.
  The runtime ledger retained zero backwards samples. Launcher exit, heap,
  four offline filesystem checks and saved-data/profile preservation passed.
- EEVDF: the same native pause test completed 1,885,268 checks with all eight
  CPU tags and zero backwards reads, bracket errors or rewinds. Static-musl
  and dynamic-glibc vDSO guest checks both pass: eight-thread bracketing, three
  deliberate protection faults, eight fork/unmap cycles and exec discovery.
  Its subsequent 30-second, 24-worker Firefox cohort completed cleanly. Periodic
  wakeup p95 was 4.04 ms, worst 89.43 ms. The buffered trace recorded 23.18
  decoded frames/s but only 4.30 presentation callbacks/s, with at least 30.66
  seconds buffered ahead and no sampled unplayable states. Media advanced at
  0.926× page time. The eight-second PCM capture contained 351,104 frames, with
  a longest silent run of 90 ms. This is not a smooth-playback acceptance or a
  performance win over the preceding single EEVDF cohort. Both native vDSO jobs
  and the browser lineage drained; heap, clean shutdown, four offline filesystem
  checks and saved-data/profile preservation passed.

Ignored artifacts are under
`build/workloads/timing-scheduler-20261020/rewind-diagnostic-acceptance`,
`percpu-clock-acceptance`, `percpu-clock-eevdf` and `q2-pc-qr0`.
A frozen source/runner manifest is in `percpu-clock-source`, with machine-readable
clock/runtime/filesystem gates in `percpu-clock-acceptance-report.json`.
The first RR probe request used `/PVRESUME` instead of `/boot/PVRESUME` and
failed before creating a process; its receipt remains beside the successful
correct-path job. It is not counted as a clock test result.

These short controls do not cover physical hardware, unstable KVM clocks,
non-KVM fallback, CPU hotplug, guest suspend or every timing consumer. Audio
pause discontinuities remain a separate HDA/device-clock behavior. No repair of
the original subjective playback speedup is claimed without reproducing it.
