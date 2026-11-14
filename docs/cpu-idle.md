# CPU idle C-states and energy bias (Broadwell-U)

This is the first step of `docs/cpu-power-management-scope.md`. All of it is off by default: the boot behaviour is still `sti; hlt` (C1).

## Commands (debug channel)

- `cpu idle mwait on|off`: on selects C6 (MWAIT hint 0x20); off returns to HLT.
- `cpu idle state hlt|c1|c1e|c3|c6`: explicit state. The request is clamped to what CPUID advertises.
- `cpu idle status|reset`. Status reports:
  - CPUID capabilities (`mwait`, `arat`, `interrupt_break`, `cpuid5_edx`);
  - the effective state, hint and ECX, and any clamp reason;
  - `misc_enable` and `turbo_disabled`;
  - `turbo_ratio_limit` and `pkg_cst_config_control` (read-only);
  - package C2/C3/C6/C7 residency;
  - per CPU: entry counts per state, `idle_tsc`, core C3/C6/C7 residency (sampled by that CPU's own idle loop), and EPB.
- `cpu epb <0-15>|default|status`: writes IA32_ENERGY_PERF_BIAS on every CPU. Each CPU applies it at its next idle entry and reads it back. `default` applies the Linux rule: firmware 0 or 15 becomes 6, anything else is left alone. Turbo and RAPL limits are never written.

## Selection rules (`cpu_idle_policy.h`, tested in `test_cpu_idle`)

- MWAIT is used only on bare-metal Intel family 6 with CPUID.01H:ECX[3] and leaf 5. VMs, which set the hypervisor bit, always use HLT.
- A state is offered only if its CPUID.05H:EDX sub-state count is non-zero; otherwise the selection steps down: C6, then C3, then C1E, then C1.
- C3 and C6 need ARAT (CPUID.06H:EAX[2]); without it the request is clamped to C1E, because the LAPIC timer would stop.
- ECX bit 0 (break on interrupt) is set only when CPUID.05H:ECX[1] reports it.

## Wake-source argument

Brook wakes idle CPUs only by interrupt: the LAPIC timer, the reschedule and TLB-shootdown IPIs, and device MSIs. No path relies on a memory store to wake a halted CPU.
- MWAIT with IF=1 exits on any interrupt, like HLT, and also on a write to the monitored per-CPU line. Changing the state or EPB writes every monitor line, so parked CPUs re-read promptly.
- The sequence is `cli; monitor; sti; mwait`. STI's interrupt shadow covers MWAIT, so an interrupt pending at STI is taken after MWAIT starts and ends it. That's the same property the old `sti; hlt` relied on.
- With ARAT the LAPIC timer keeps running in C6. The TSC is invariant, so `clock status` is unaffected.
- Exit latency from C6 is about 100 µs (intel_idle's bdw value is 133 µs). That's the cost to measure on audio refill and GPU completion polling.

## Dell A/B plan

On one boot, with the yquake demo and zero-copy wt:
1. **Baseline (hlt):** `thermal reset`, `cpu idle reset`, `cpu idle status`.
   - Idle desktop for 60 s, then take `cpu idle status` again plus `hw_thermal`. Package residency deltas against the TSC delta give PC2/PC3/PC6/PC7 %, along with the package temperature and power.
   - Run `scripts/dell_quake_measure.py … C` and a 5-minute live attract demo. Record fps, displayed mean and p50, package temperature, max and power, and the `bridge hardware dell-hda-queue` later-gap deltas while Chromium audio or the yquake OpenAL demo plays.
2. **`cpu epb default`:** repeat the idle reading.
3. **`cpu idle mwait on`:** confirm `state: c6` and `clamp: none`, then repeat steps 1–2. Expect:
   - deeper core and package residency at idle;
   - lower idle package power and temperature;
   - unchanged fps;
   - no new HDA later gaps.
4. **If audio gaps or frame-pacing tails rise,** try `cpu idle state c1e` (exit about 10 µs) and compare.
5. **Back out** with `cpu idle mwait off` or a reboot; nothing persists.

## Governor (`cpu idle state auto`)

**Why it's needed:** the gen 394 A/B showed fixed C6 saves about 30% of idle package power (4.67 → 3.22 W) but adds about 3 ms to yquake's p50 (C6 exit latency on the busy GPU-worker and renderer paths).

**How it chooses:** per CPU, the last 8 idle intervals (TSC-timed) predict the next one conservatively, using the second-shortest of the window:
- C6 at 400 µs or more;
- C3 at 100 µs or more;
- C1E otherwise, and also on a fresh CPU with fewer than two samples.

**Latency hold:** the GPU submission worker calls `CpuIdleLatencyHint(4000)` each time it yields while a job is in flight, so that CPU stays at C1E for 4 ms after GPU activity. A busy CPU stays shallow, and a quiet one converges to C6.

**Status per CPU:** auto selection counts, holds, the last prediction, and hits / too_deep / too_shallow with `hit_pct`:
- too_deep: woke before the state's target residency;
- too_shallow: slept long enough for a deeper state, not counting holds.

**Package residency:** `pkg_pct` gives package residency as a percentage of the TSC interval since the previous status call.

**Boot defaults:**
- EPB becomes 6 when the firmware left 0 or 15 (Linux behaviour). The Dell's firmware value is 0, so this now applies at boot. Status shows `epb_firmware` and `epb_boot_applied`.
- The idle state stays `hlt` by default until the governor has been A/B tested on the Dell.
