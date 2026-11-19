# GPU hang diagnostics (Broadwell RCS)

Diagnostics for the intermittent RCS user-job failure seen on the Dell (gens 428 and 438, and 16 Nov):

```
gpu-submit: job failed after verified reset pid=.. ctx=1 objects=.. pages=.. site=2 submit=N count=1
```

Code: `src/kernel/src/intel_bdw_hang_diag.{h,cpp}`. The hooks are in `intel_bdw_render_backend.h`, `intel_bdw_bcs_mmio.h`/`intel_bdw_bcs_init.h` (execlist init stage), `intel_bdw_critical.cpp` (render owner, capture) and the worker in `intel_bdw_scanout.cpp`. Host test: `test_intel_bdw_hang_diag`.

## What the existing log line already says

`submit=` is `BdwRenderOneShot::LastSubmitSite()`. The two variants seen so far are different failures:

* **`submit=10`** (gens 428 and 16 Nov): the code timed out waiting for the breadcrumb. Both logs show `ring head=e0 tail=e0 acthd=e0 elsp=302 ipehr=4000001 seen=0 polls=2|7`. `0xe0` is the ring tail, and `IPEHR=0x04000001` is `MI_ARB_ON_OFF|enable`, ring dword 54, after the breadcrumb `PIPE_CONTROL`. `EXECLIST_STATUS_LO=0x302` has no element-valid bits (3, 4) set. So the command streamer **did** run the whole ring and the context left the execlist. The CPU never saw the breadcrumb value, and only made 2 or 7 polls before the 2000 ms critical-section deadline. Either the write landed late (after the last poll, which would mean poll starvation or a long batch), or the post-sync write was lost or went to the wrong place.
* **`submit=7`** (gen 438): `initializeExeclists` failed, which includes the takeover RCS reset. **ELSP was never written** (`polls=0`). The `ring head=0 tail=0 acthd=0 elsp=1` values were sampled after that reset, so they show reset state, not the job's state. Possible causes: the takeover reset was not acknowledged (busy GDRST domains or catastrophic error), RING_MODE/MI_MODE readback, the CSB write pointer not at the post-reset value 7, or the render-state restore. An uncertain reset would have panicked, so it was not that.

Neither variant fits "ELSP written, but no CSB event arrived". The new fields below tell the remaining causes apart.

## What is recorded

**Per-job breadcrumb ring** (always on, 128 entries, RCS user jobs). Each entry holds the job number, pid/ctx, nonce, objects/pages, batch address and length, the context descriptor written to ELSP, and the CTX_RING_HEAD/TAIL in the image. It also holds `EXECLIST_STATUS_LO` and `RING_CONTEXT_STATUS_PTR` before the first ELSP dword. After the fourth dword it holds `EXECLIST_STATUS_LO/HI` and the CSB pointer again. Before ELSP it also records `FORCEWAKE_ACK_HSW`, `GEN6_RC_STATE` and `GEN6_GT_CORE_STATUS`. Further fields: the failure sites, the init stage, the number of ELSP writes, the last breadcrumb value and poll count, and these times in µs after the worker's prepare start: ELSP write, first poll, breadcrumb seen, first CSB read, run end, and the longest gap between polls.

The ELSP samples add 8 MMIO reads per job. The critical section already does dozens of reads per job (every `owned()` reads the forcewake ACK). On the host, an append costs about 10 ns (136-byte record). The rest is TSC reads and copies of values the path already holds.

**Failure capture** (only for a failed attempted run, or once after `gpu hang reference arm`). It is taken inside the forcewake critical section, in `Transaction::run()` right after `BdwRunRetainedRender` returns. That is before `resetAfter()` issues the stop/reset, and it only reads. It contains:

* 64 registers (table `BdwHangRegs`, Linux i915 gen8 offsets): ring TAIL/HEAD/START/CTL, ACTHD(+UDW), DMA_FADD, IPEIR/IPEHR, INSTDONE (also called INSTDONE_1 on gen7+), INSTPS, INSTPM, EIR/EMR/ESR, PGTBL_ER, BBADDR(+UDW)/BBSTATE, SBBADDR/SBBSTATE, CCID, CONTEXT_CONTROL, EXECLIST_STATUS_LO/HI, CONTEXT_STATUS_PTR, RING_MODE (GFX_MODE), MI_MODE, HWS_PGA, HWSTAM, IMR, NOPID, PSMI_CTL, RESET_CTL, TIMESTAMP, the four PDPs, RING_FAULT, FAULT_TLB_DATA0/1, ERROR_GEN6, DONE_REG, GAM_ECOCHK, PAT, GDRST, MCR selector, FORCEWAKE_MT/ACK, RC_CONTROL/RC_STATE, GT_CORE_STATUS, RC6 residency, RPSTAT1, RPNSWREQ.
* All six CSB entries from the registers (`0x2370+8i`), and the HWS memory copy (CSB records plus the producer at `0x7c`).
* Memory, read after a clflush: LRC register-state page dwords 0..0x47 (RING_HEAD/TAIL/START/CTL, BB state, PDPs), PPHWSP dwords 0x30..0x3f, the full 56-dword ring, and the completion page dwords 0..7. These include the breadcrumb (+0) and the started marker (+16).
* GGTT PTEs of the 29 leased pages next to their expected DMA addresses.
* The first 8 batch dwords (CPU read), the init/reset diagnostics, the ELSP samples, the deadline cause (`deadline` 1 budget, 2 clock, 3 the 2000 ms limit), and the last 16 breadcrumbs.

**Not captured:**

* GT FIFO (`0x120000/0x120008`), slice/sampler/row INSTDONE (`0x7100/0xe160/0xe164`) and GAB_CTL. Their pages are not in the audited UC set, and adding them would change admission. To include them, extend the render page list in `VmmDellForcewakeCriticalImpl` (and its `Audit()`) first.
* BCS and VCS jobs (RCS only).

**Started marker.** The CS-stall `PIPE_CONTROL` between the context workaround and `MI_BATCH_BUFFER_START` (ring dwords 32..37) now also posts a qword write of the nonce to `completion+16`. It uses the same flags as the final breadcrumb. `started==nonce` with no breadcrumb means the job reached the batch. `started!=nonce` means it never got there. The marker is on by default; `gpu hang marker off` affects rings built after the command. This is the only change to GPU commands. `marker` in a capture is read back from the ring itself.

**What survives until reset.** Everything above is read before the stop/reset, but:

* For `submit<=9` the takeover reset (inside `initializeExeclists`) has already run. Registers then show reset state. For pre-reset state, use `init.busy`: the last engine observation that refused quiescence, a free sample. Also use `init.exec_stage`/`exec_value`.
* CSB registers and the HWS copy survive until our reset. The LRC image only holds GPU-saved state if the context was switched out.
* After the reset, ring registers are cleared. The PTEs are cleared by the retire step that follows.

## Commands (debug bridge)

```
gpu hang status              counts, marker state
gpu hang last                last failure capture (JSON)
gpu hang ring [n]            last n breadcrumbs, oldest first (default 16, max 128)
gpu hang reference arm       capture the next job even if it succeeds
gpu hang reference           that reference capture
gpu hang marker on|off|status
```

On failure the kernel log gets three `gpu-hang:` lines (verdict, key registers, ELSP samples and timing). The lastlog store (`/nix/var/log/brook/lastlog*.bin`, read with the existing `lastlog` commands) persists the log tail across a hang and power cycle, so these lines survive a reboot. The full capture is RAM-only. The uncertain path still panics; its panic message goes to the panic journal (`PANIC.BIN`). Appending a compact capture to that packet would be the cheap way to persist it if needed.

## Reading a capture

`verdict` is a heuristic (`BdwHangVerdict`). Confirm it against the raw fields, ideally next to a `reference` capture of a healthy job from the same boot.

Ring dword map: 0 invalidate `PIPE_CONTROL`, 6 barrier, 12 context WA, 32 started marker, 38 `MI_ARB_ON_OFF` off, 39 `MI_BATCH_BUFFER_START`, 42 flush, 48 breadcrumb, 54 `MI_ARB_ON_OFF` on, 55 NOOP. The tail is `0xe0`. `RING_HEAD=0xa8` means the head is inside the user batch.

| Hypothesis | What the capture shows |
|---|---|
| Init/reset failed, never submitted (`submit=7`) | `elsp_writes` < 4, `init.stage`: 2 not quiescent (see `busy`), 3 takeover reset not acknowledged (`takeover_reset`, GDRST, RESET_CTL), 4 execlist init (`exec_stage` 2 RING_MODE, 3 MI_MODE STOP_RING, 4 HWS_PGA, 6 CSB pointer not 7 after reset with `exec_value`=pointer, 9 pointer write, 10 HWS producer), 5 render-state restore |
| Breadcrumb late (slow job or poll starvation) | `breadcrumb.now==nonce`, `deadline=3`, large `max_poll_gap`/few `polls` (starvation) vs. normal gaps (long batch). Verdict `breadcrumb_late_after_deadline` |
| Breadcrumb write lost or misdirected | ring completed (HEAD=TAIL=0xe0, IPEHR `0x04000001`), `started==nonce`, `breadcrumb.now!=nonce`. Check `ggtt[25]` against `dma[25]` and the PAT. Verdict `ring_completed_breadcrumb_missing` |
| Lost ELSP write (RC6, forcewake, FIFO) | `elsp_writes`=4 but `started!=nonce`, CSB pointer still `0x?05` and HWS producer 5, no element valid. `elsp_before.fw_ack` bit 0 clear or `rc_state`/`core_status` not RC0 points at power state. Verdict `elsp_lost_no_csb_event` |
| Element accepted, context never loaded | as above, but an EXECLIST_STATUS element-valid bit is set. Verdict `context_not_loaded_element_pending` |
| Wrong or stale descriptor, corrupt image | `descriptor` vs `lrc_regstate` (RING_START dword 9 vs `ggtt[20]`, PDPs dwords 0x25..0x33 vs `GEN8_RING_PDP_*`), CCID, CSB events with an unexpected context ID (`csb_site=5`) |
| Stalled in the ring prelude | `started!=nonce`, CSB events present, `RING_HEAD` < `0xa8`. IPEHR names the stuck command |
| Hung inside the user batch | `started==nonce`, `RING_HEAD=0xa8`, BBADDR set, ACTHD advancing between a capture and the reference. INSTDONE shows the busy units |
| Waiting on a semaphore/event | as above with IPEHR opcode `MI_SEMAPHORE_WAIT` (0x1c) or `MI_WAIT_FOR_EVENT` (0x03) |
| GPU page fault / error | `GEN8_RING_FAULT_REG` bit 0, `ERROR_GEN6`, `GEN8_FAULT_TLB_DATA0/1`, EIR/ESR |

The EXECLIST_STATUS_LO bit meanings used here (bits 3/4 element valid, 2 queue full, 14..15/17..18 active state) match the existing quiescence mask in `BdwObserveEngine`. Check other bits against the PRM before relying on them.

## After the next hang on the Dell

```
gpu hang last
gpu hang ring 32
gpu hang reference arm      (then run any GPU work)
gpu hang reference
lastlog previous 0 4096     (after a reboot: previous boot tail incl. gpu-hang: lines)
```

## Gen 440: starved completion poll (late completion)

The first capture (`build/dell/q2-440-launcher` in display-perf) was `breadcrumb_late_after_deadline`. Both the started marker and the breadcrumb equal the nonce, and the ring is idle at its tail. The job had finished. The worker made 4 polls in the first 2.9 ms (gaps of about 1 ms). Its next yield did not return until about 2194 ms, past the 2000 ms critical-section deadline (`dl=3`), and the finished job was reset as failed. The `submit=10` failures of gen 428 and 16 Nov have the same signature.

**Fix: deadline grace.** `BdwDeadlineGrace` (`intel_bdw_wait_yield.h`) applies to both the completion wait and the context-status retirement wait. When the normal poll stops because of the deadline, it allows exactly one more check under the ownership that is still held. It does not apply after a failed yield, lost ownership or uncertainty, and the check never yields. If the nonce (or the context-complete CSB event) is there, the job retires normally. It is then counted as `late_completions` in `gpu hang status`, flagged 4 in its breadcrumb, logged as `gpu-submit: late completion accepted ...`, and excluded from the completion predictor. Otherwise the job fails exactly as before.

**Where the time goes.** Each capture and breadcrumb now records the run's longest poll yield, split into phases: `unlock_us`, `wait_us` (the `SchedulerSleepMs(1)` or spin slice), `lock_us` (re-taking the kernel page-table lock) and `audit_us`. It also records `apic_ticks` (timer interrupts on CPU 0 during the wait), and the last context switch into CPU 0: `from_pid`, `from_kernel`, and `from_ran_us`, how long that process had held CPU 0. The KLog line is `gpu-hang:  longest yield ...`.

How to read it:

| Fields | Meaning |
|---|---|
| `wait_us` large, `apic_ticks` ≈ `wait_us`/1000, `from_ran_us` ≈ `wait_us` | One process held CPU 0 the whole time. Brook does not preempt ring-0 code: a tick that interrupts a kernel thread or a syscall never switches. `from_kernel` and `from_pid` name it |
| `wait_us` large, `apic_ticks` small | CPU 0 ran with interrupts off. No tick also means no timed-wake scan, because sleeping tasks are woken only by CPU 0's tick |
| `lock_us` large | Another CPU held the kernel page-table lock (`VmmKernelPtCriticalEnter` spins with IF=0) |
| `wait_us` large, `switches` > 1 or `from_ran_us` small | Run-queue starvation of the CPU-0-pinned worker (EEVDF pick); confirm with `sched timing on` + `sched timing watch <gpu-submit pid>` |
