# CPU power management on the Dell (Broadwell-U): scope

Status: scoping only, nothing implemented. Written 14 Nov 2026 with the thermal sampler (`work/thermal`).

## Why

Under the yquake demo the package sits at 77–85 °C and about 11.7 W. It has reached 99 °C against TjMax 100. The fan runs gently, not at full speed. Brook does no CPU power management of its own, so whatever the firmware left set decides both idle power and how much package budget the GPU gets. The GPU shares the package power and thermal limits.

## What Brook does today (from the code)

- **Idle:** `IdleLoop` (`scheduler.cpp`) executes `sti; hlt`. HLT requests C1 only, or C1E if the firmware has set POWER_CTL bit 1. There is no MWAIT anywhere in the kernel.
  - Package C-states deeper than PC2 need every core in C3 or deeper, so on Brook **the package probably never reaches PC3/PC6/PC7 or PC8–10**.
  - Idle CPUs keep the package warm and leave less turbo and GPU headroom.
- **P-states:** nothing writes IA32_PERF_CTL (0x199). The cores run at whatever ratio the firmware last requested. With EIST on and no OS requests, that is commonly the maximum non-turbo ratio or a firmware-chosen turbo request. It is unknown until read on the Dell.
- **HWP:** absent. Broadwell predates Speed Shift (Skylake), and CPUID.06H:EAX[7] is clear; `thermal power` reports `hwp_supported`.
- **Energy/performance bias:** never written. IA32_ENERGY_PERF_BIAS (0x1B0) holds the firmware value: 0 is performance, 6 normal, 15 power-save. Linux sets 6 at boot when it finds 0.
- **Turbo:** never changed. IA32_MISC_ENABLE bit 38 (turbo disable) and bit 16 (EIST enable) are as the firmware left them.

## Measure first

`thermal power` (or `hw_thermal` with `power: true`) dumps these raw registers on the Dell:
- `misc_enable`, `perf_ctl`, `perf_status`, `energy_perf_bias`, `platform_info` (base, max non-turbo and min ratios), `turbo_ratio_limit`;
- `pkg_cst_config_control`: the package C-state limit, and the bit-15 lock;
- `pkg_power_limit`, `pkg_power_info`: PL1/PL2 and TDP;
- CPUID.05H:EDX: the MWAIT sub-states per C-state;
- package C2/C3/C6/C7/C8/C9/C10 and core C3/C6/C7 residency counters.

Take two readings about 10 s apart, idle and under yquake. The residency deltas against the TSC delta show which C-states are actually reached; the expectation is about 0% beyond C1/PC2. `thermal status` gives the per-CPU C0 residency and the C0 frequency (APERF/MPERF).

## What Linux does on this CPU

- **intel_idle,** `bdw_cstates`: MWAIT hints C1 0x00, C1E 0x01, C3 0x10, C6 0x20, C7s 0x32, C8 0x40, C9 0x50, C10 0x60. It chooses by predicted idle length against each state's exit latency and target residency, roughly 2/10/40/133/166/300/600/2600 µs. Broadwell has ARAT (CPUID.06H:EAX[2]), so the LAPIC timer keeps running in deep C-states, and the TSC is invariant.
- **intel_pstate,** active mode without HWP: each CPU's PERF_CTL ratio follows utilisation, with turbo allowed and EPB set to 6 when the firmware left 0.

## Recommended safe first step

1. **MWAIT idle with a conservative ceiling,** behind a runtime switch (`cpu idle hlt|c1e|c3|c6`, default `hlt` until measured).
   - In `IdleLoop`, replace `sti; hlt` with `monitor` on a per-CPU wake word, then `sti; mwait` with the chosen hint and ECX bit 0, so interrupts break out even while masked.
   - Check CPUID.01H:ECX[3] (MONITOR/MWAIT) and the CPUID.05H sub-state count before offering a state.
   - Start with C6 (hint 0x20): it gives most of the package-C-state benefit at about 100 µs exit latency.
   - Leave C7s–C10 for later. They interact with the display's PSR and eDP link (the panel is already sensitive) and have millisecond exit latencies.
   - Measure package C-state residency, idle package power, package temperature and fan. Also measure audio underruns (HDA queue later gaps) and GPU job latency, since the gpu-submit worker polls and sleeps.
2. **Energy bias:** if EPB reads 0, set 6 on every CPU, as Linux does. This is low risk and reversible.
3. **Later, a simple P-state policy:** request the maximum ratio when C0 residency is high, and a low ratio when idle, through PERF_CTL per CPU.
   - Gains: lower package power when idle or lightly loaded, which leaves more of the package budget for the GPU and makes the higher GPU clock (`gpu clock`) sustainable without the 90 °C fallback tripping.
   - Hold this until the measurements show the firmware's default ratio is wrong.

## Risks

- **Wake-up correctness:**
  - A missed MONITOR store or IPI wake would stall a CPU until its next timer tick (1 ms).
  - The scheduler's wake and IPI paths, and `DrainPostSwitch` at the top of the loop, must re-check after arming MONITOR.
  - Fall back to HLT on any doubt.
- **Latency-sensitive devices:**
  - HDA DMA refill (the mixer already shows 30–36 ms sleeps);
  - the xHCI keyboard and touchpad;
  - the GPU worker's completion polling.

  Deeper states add microseconds to wake-up, so measure audio gaps and frame pacing.
- **Firmware lock:** a set PKG_CST_CONFIG_CONTROL lock bit means the package limit can't be raised. Only the OS's MWAIT requests can be changed.
- **TSC clock:** C6 keeps the invariant TSC. A deeper state on a non-invariant part would not, but this CPU is invariant (`clock status`).
