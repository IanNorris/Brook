# Brook handover

Current state and immediate goal. Keep this short and current: replace stale lines rather than appending. Put detailed run records in [AGENT_JOURNAL.md](AGENT_JOURNAL.md), the previous agent's ~1 MB append-only log (newest entries at the top). Decisions belong in [DECISIONS.md](DECISIONS.md), and working preferences in [WORKING_STYLE.md](WORKING_STYLE.md).

_Last updated: 14 November 2026, by a Claude Code session started after the Codex agent was retired._

## Workspace

- **Root:** `/data/Programming/Brook/`. `main/` is a fresh GitHub clone kept on `main`. Each agent or task gets its own worktree, `git worktree add ../<task> -b <branch>`, with its own `build/`. Never share a build directory or drive the Dell from two agents at once.
- **Shared private assets:** `/data/Programming/Brook/shared/dell/` (mode 700, never commit), with `SHA256SUMS`:
  - `hardware-update-key`;
  - `source-initrd-cc876d82.img`;
  - `rollback-gen355/` (the known-good pair);
  - `quake-probe-bundle.tar` (demo timing script, swap trace library and paks).
- **Legacy checkout:** `/data/Programming/Codex/Brook` and its `build/hardware/` evidence are reference only. Old disk images are being cleaned up separately.
- **Build:** `nix-shell --run './scripts/build.sh Release'` from a clean checkout, then `ctest --test-dir build/release/host_tests`. `test_audio_delivery` can time out under parallel load; rerun it alone.

## Repository

- Work from `main` (`dc12f39e` at the time of writing). On 13 November at about 23:31 UTC it absorbed both lines of the previous agent's work: the main checkout's long-uncommitted changes (`d6724e11`), and the private checkout `/tmp/brook-main-BrURO4` (`dcb99b97`, `bd0ae24b`).
- `work/steam-desktop-hardening` is superseded. A stash backup of its final worktree, `stash@{0}`, is identical in content to `d6724e11`.
- The detached worktrees `Brook-gpu-lock-control`, `Brook-pre-timing-7d005ae0` and `Brook-timing-bisect`, and `/tmp/brook-main-BrURO4`, are historical. Their contents are in main. Do not develop in them.
- The `build/` tree holds hundreds of GB, mostly disk images. Never copy it wholesale.

## Active goal (set 14 November 2026)

**Reach at least 60 distinct displayed frames/s for the matched yquake2 GL3 demo on the Dell's GPU desktop, with the panel, recovery paths and normal boot intact.** Work through the milestones in order, iterating as the measurements direct. Ian has authorized autonomous iteration, including Dell reboots and AC power cycling. Stop when the goal is verified, or when a decision needs Ian.

| # | Milestone | State |
|---|---|---|
| 0a | Clean workspace builds from a fresh clone (`d668a832`) | done |
| 0b | Move the Dell GPU critical-section code out of `memory/virtual_memory.cpp` into `intel_bdw_critical.cpp` (pure move) | done: `2831dbbd`; Dell stage timings unchanged vs gen 355 |
| 0c | Reproducible Dell image build: `scripts/build_dell_image.py --qemu-check` (guarded QEMU repair-to-desktop via `scripts/check_dell_image_qemu.py`) | done: `74342a6b`, `e8680304`, `56fbc88b` |
| M0 | Diagnostics: `gpu worker stats` (kernel), `/tmp/brook-desktop-stats.json` + `scripts/desktop_stats_diff.py` (renderer), `scripts/dell_quake_measure.py` | done: gen 357 results below |
| M2 | Event-driven waylandd, channel ABI v2 (retain + PRESENTED), release on SAFE without CPU copy, present-driven callbacks | done: `aac9e79c`, `af3626c9`; gen 361 with overlay |
| — | Kernel Console hidden at startup on the Dell (`--hide-kernel-console`, Apps > Kernel Console to reopen) plus a taskbar network indicator | done: `f20af824`; gen 358: displayed interval 79 → 68–71 ms |
| M1 | Change-based damage, idle polls, quad pass with occlusion and bit-exact shader blending (compute fallback via `BROOK_RENDER_COMPOSE=compute`) | done: `4330fd74`; gen 362: displayed ~29 fps (2 buffers) / ~24 (8), swaps 46/85, compositor job ~10.5 ms, worker 37–42% idle |
| M4a | Wake the submission worker on submit and present, with fence wait queues instead of 1 ms sleeps | todo |
| M3 | Page flip (DSPASURF) instead of the BCS copy, or a cheap persistent GOP map if M0 shows the copy is small | todo |
| M5 | Deferred: PPGTT residency, persistent contexts, interrupts | backlog |

**M0 results (gen 357, console visible, 14 Nov):**
- **Displayed Quake frames ≈ 12.5/s** (mean 79 ms between frames) at 2, 4 and 8 client buffers alike. Completed swaps were 35–69/s.
- **The gpu-submit worker is only 74–77% busy,** so this is latency-bound: one frame in flight, serialized.
- **Per displayed game frame:**
  - renderer CPU prepare ~23–25 ms, of which overlay polling is ~10 ms (2 × `usleep(10000)`);
  - compositor job ~19–23 ms on the worker, ~17 ms of it 1 ms poll sleeps while the GPU composites;
  - BCS present ~8 ms (2.4 ms setup plus a ~5.5 ms blit of 2.4–2.9 Mpx; a full screen is 15 ms);
  - Quake job ~6.6 ms, ~4 ms of it 1 ms poll sleeps.
- **The GPU clock stays at 500 MHz from issue to end,** so the ramp hypothesis is refuted.
- **Non-game presents run at ~13–17/s** (console and cursor) and each costs a full compositor job.
- Evidence: `/data/Programming/Brook/display-perf/build/dell/measure-m0/`.

**Incident (14 Nov, ~02:10 UTC):**
- **What happened:** the first M4a image (`dell-m4a-test`, gen 359, slot 0) panicked at the first Mesa job: "Broadwell worker uncertain … yield=1/0". The root cause was that `SchedulerYield()` inside the new spin slice can return with IRQs off, which made the next `MayYield()` fail. Fixed in `8fc4bb07` (with a host test); the fixed image is `m4a-wakeups/build/dell/pair-m4a-2`, not yet tested on the Dell.
- **The consequence:** the guard retried the active slot and entered a recovery-only reboot loop. That boot has no command jobs, so there is no remote way to switch back to gen 358 in slot 1. Ian cold-booted the USB maintenance stick; gen 358's slot was promoted to gen 360 with `scripts/hardware_promote_slot.py`, and the SSD then booted normally.
- **Lesson:** the QEMU check never runs Intel GPU code, so kernel GPU-path changes are a recovery risk. Until the guard can fall back to the previous slot automatically, deploy them only when Ian is around.

**Gen 361 (M2 + overlay caching, `6a560d68`):**
- **Displayed:** mean interval 69 ms with 8 buffers and 60 ms with 2 (was 68–71).
- **Swaps fell:** 53 and 26/s (were 69 and 40).
- **Overlay:** captures 30 → 8/s, wait 10 → 2 ms; non-game prep 10 → 0.5 ms.
- **Worker:** idle only 8–13% (compositor 45–47%, present 26%).
- **Non-game composites now cost ~18 ms each at 12–20/s.** Composite cost doesn't scale with damage, so M1 (damage-proportional composition) is next, then M3.
- The rollback after the M4a loop used `scripts/hardware_promote_slot.py` from the USB maintenance boot (gen 358 → gen 360).

Ian's direction (14 Nov):
- Don't use extra client buffers. Relinquish the earlier buffer once the OS has consumed it.
- Eliminate CPU copies in the pipeline.
- Avoid CPU writes for the kernel shell in GPU mode. Transparent rendering interfaces can come later.

## Background: 60 fps displayed on the Dell

Target: at least 60 **displayed** frames/s for the yquake2 GL3 matched demo (1280×720 window, uncapped, 600 records) on the Dell's HD Graphics 5500. This is the first step toward smooth games and video on the Dell, with Steam and Stardew to follow.

### Ground truth (audited 13–14 November from raw swap traces)

- **Linux on the same Dell:** about 420 completed swaps/s, 2.3 ms median, render engine about 17% busy (`build/hardware/dell-linux-baseline-20261112/`).
- **Brook's completed-swap rate:**
  - rose from 6.3/s (00:17 on 13 Nov) to 21.3 (02:22, CPU clflush skipped for coherent LLC buffers);
  - then 34.4 (03:25, gen 338: short scanout lock and one desktop frame at a time);
  - then about 65–70 (06:00–07:30: client limit of 8 Wayland buffers plus patched Iris).
  - It held about 66–70 in like-for-like runs afterwards, and reached 95 with an 800 MHz diagnostic clock.
  - Lower results after 09:00 were experiments or misconfigured four-buffer runs, not regressions.
- **Brook's displayed rate:** about 12–17 distinct game frames/s were completed by the compositor (14.1/s at 500 MHz on gen 354, 17.1/s at 800 MHz, 21.7/s in bare mode). Earlier runs didn't count displayed frames.
  - The completed-swap figure is **not** the displayed frame rate. With 8 buffers, Quake renders bursts the compositor never shows.
- **Diagrams** (`*/frame-timeline.svg` under `build/hardware/dell-present-ownership-20261113/`):
  - each compositor GPU pass takes about 30–33 ms at 500 MHz (about 14 ms bare), against about 3–4 ms per Quake frame;
  - both share one render ring and one kernel submission worker;
  - the presented frame is then copied to the fixed GOP framebuffer by the blitter, with no page flip and no vblank.
- **The binding limit is the presentation path** (compositor cost, serialization and copy to scanout), not game rendering.
- **What a normal launch uses:** the installed launcher still selects stock Mesa, at about 26–33 swaps/s. The patched Iris build and 8-buffer setting are opt-in (`brook-iris-run`, `BROOK_MESA_WAYLAND_BUFFERS=8`).

### Review outcome and plan (14 November)

Two independent code reviews (compositor and driver), Dell measurements on the gen 355 baseline, and a Fable senior review are in `build/review-20261114/`. Start with `fable-verdict.md`.

- **Dominant costs per displayed frame:** the composite pass for game-damaged frames (15–30 ms), the BCS copy to GOP (about 7 ms inferred), and the game's own job (6.7 ms, about 10× Linux GPU time). Timer-driven pacing in waylandd wastes most game frames when the client has 8 buffers.
- **Per-job driver setup and reset is small** (about 0.25 ms) and stays for now.
- **Proposed milestones:**
  - M0: counters for distinct displayed frames, BCS stage timing and worker busy time;
  - M1: rewrite composition as per-layer quads;
  - M2: present-driven pacing in waylandd;
  - M4a: wake instead of 1 ms sleeps;
  - M3: page flip instead of the BCS copy;
  - M5 (deferred): persistent contexts and residency.
- **Before any code milestone:** a scripted, reproducible Dell image build from `main`.
- **Rules:** awaiting Ian's go-ahead. Code changes go on a branch. Count displayed frames, not only swaps.

## Dell state (14 November)

- **Current (supersedes the gen 355 line below):** generation 356, slot 1, payload `dell-vmm-move-20261114`, built from `2831dbbd` by `build_dell_image.py`; bridge session `79b1dcaaaffc4d2a80f9589683d65802`. Baseline measurements: 68.6 swaps/s with 8 buffers and 39.5 with 2 (`display-perf/build/dell/measure-vmm-move/`). Slot 0 holds gen 355 for rollback.

- **Target:** Dell XPS 13 9343 at `192.168.85.106:1234`. Generation 355, slot 0: boot `7f78aa2b8ea24c17816b5b80eff04a26`, session `bccc100914d547f095a4d01834494c93`.
- **Boot payload:** the restored gen 338 baseline `dell-short-lock-serial-20261113` (receipt `build/hardware/dell-baseline-restore-20261114/receipt.json`). It was built with BROOK_GPU_SUBMISSION_TIMING, the coherent LLC render path and the 500 MHz on-AC clock trial. Pair: `build/hardware/dell-present-ownership-20261113/pair-short-lock-serial/`.
- **Previous payload:** `dell-scanout-fair-counts-20261113`, the boot Ian started after the previous agent's last image.
- **Build recipe gap:** the exact recipe for building a Dell pair from source isn't recorded (it was assembled in a private checkout). The gen 338 kernel came from a guard-timing CMake configuration (cache SHA `34df12a0…`) with source initrd `build/hardware/dell-packed-overlay-candidate-20261111/INITRD.IMG`. A scripted, reproducible "Dell image from main" build is the first code task.
- **Review measurements on this boot:** `build/review-20261114/dell-gen355-data.md`.
- **Power:**
  - checked power-off via EFI runtime, with reset via ACPI io8;
  - the Tasmota AC relay reports `Power1: ON` (config `~/.config/dellpower.txt`, private);
  - the `dell_recovery.py` controller is not armed (last attempt state `spent`, 12 Nov).
- **Recovery:** SSD F5 recovery (`BROOKF5.EFI`, firmware entry added by Ian) and the USB path remain available.
- **Steam:** Ian's attempt to start it failed before this restart. That boot's logs are lost, so it is not yet investigated. Earlier Dell Steam-client attempts hung the machine; see `docs/steam-dell-software-glx-checkpoint.md`.

## Operating notes

- **The `brook-vm` MCP isn't registered in Claude Code.** Use `BROOK_ROOT=$PWD python3 tools/vm_bridge/server.py --call TOOL --arguments 'JSON'`. It has the same tools and side effects. Read `tools/vm_bridge/skills/brook-vm/SKILL.md` and its references first.
- **The Dell debug listener is single-connection.** Only one agent should operate the Dell at a time.
- **Power tools:**
  - `scripts/tasmota_power.py --config ~/.config/dellpower.txt {status,on,off}` switches the AC relay;
  - `scripts/dell_recovery.py --host IP {status,arm,disarm,run,inhibit}` is the boot-pinned, one-attempt AC power-cycle controller;
  - Wake on AC after a checked power-off is verified physically.
  - Ask Ian before power-cycling.
- **Job table:** a long campaign can exhaust the 256-slot table, and the current boot may lack the terminate API. Archive completed work and plan a checked restart.

## Backlog (outside the immediate goal)

- **Loader F5 wait ignores input on the Dell:** works in QEMU/OVMF. Boot the timing loader (branch `work/boot-timing`), press keys during the wait and read `BOOT_TIMING_DETAIL wait_keys=`; zero suggests ConIn isn't connected on the fast-boot path. See `src/bootloader/src/DOCS.md`.

- **Automatic slot rollback in the guard:** after N consecutive desktop panics on a newly activated generation, select the previous verified slot instead of looping in recovery-only mode. Also expose a slot-select operation in the recovery-only bridge.
- **Split `src/kernel/src/syscall.cpp`** (17k lines) by area: memory, files, process/signal, sockets, epoll/timers and Brook-specific.
- **Remove or declare the twelve orphaned `tools/netsurf-build/*` gitlinks** (they have no `.gitmodules` entries).
- **Transparent kernel rendering interfaces,** so console and shell output uses the GPU path.

Smooth Chromium/YouTube playback (about 90% frame drops at 720p60), a Dell Steam client launch with audio, the bridge-loss watchdog (design only), the Xwayland `setitimer` interface, and promotion of the patched Iris build to the default launcher.
