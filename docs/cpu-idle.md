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
