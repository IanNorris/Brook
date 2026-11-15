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
| M3 | Page flip (`BROOK_BDW_SCANOUT_FLIP`) and persistent BCS scanout map (`BROOK_BDW_PERSISTENT_SCANOUT_MAP`), runtime-gated (`gpu flip probe/arm/off/status`, `gpu scanout map on/off/status`) | in progress, `work/m3-flip`. Gen 364: probe 3/3 pass (pipe A → eDP, PSR off, stride 12800, 60 Hz). Persistent map: present prep 2.7 → 0.4 ms, 2-buffer interval 35.5 → 33.9 ms. `gpu flip arm` refused with `ggtt-bind` (safe fallback); fixed in `8c146e19` (take over inherited GGTT entries), Dell retest pending |
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

**Perceptual check (gen 363, `dell-m1-volume-20261114`, 14 Nov):**
- **Run:** plain yquake2 attract demo (default 640×480 shown 2×, GL3 via patched Iris, default buffering).
- **Ian:** "very playable"; yquake2's own counter read 46 fps (21.9 ms), "quite erratic" but frame pacing visually acceptable.
- **Renderer counters over 14 s:** 45.0 distinct displayed frames/s (every swap shown). Intervals p50 19.5, p90 29.8, p99 44.6 ms. Game-frame submit→fence 9 ms, prepare 3 ms.
- **The remaining jitter** is the 33–50 ms tail (serial one-frame pipeline plus BCS copy), so M3 and pipelining are next.

**Clock finding (gen 365, `dell-batch1-20261114`, 14 Nov night):**
- **On the Dell, CLOCK_MONOTONIC/REALTIME count LAPIC ticks** (no kvmclock).
- **`bridge hardware clock-compare`:**
  - idle: monotonic = TSC = HDA = host within 0.2% over 20 s;
  - **under the Quake benchmark, monotonic ran 11.5% slow** (8.05 s vs host 9.09 s), while TSC and HDA tracked the host.
- **Consequences:**
  - browser A/V drift (Chromium audio slow, Firefox fast);
  - **all clock_gettime-based fps figures above are inflated by ~10% under load.**
- **Fix in progress (`work/tsc-clock`):** TSC-backed time. Re-baseline after it lands.
- **Boot timing on the Dell (old loader):** firmware 4.27 s (FPDT), kernel 2.79 s; loader and menu need the new bootloader, a supervised step pending with Ian.

**Pipelining A/B (gen 366, `dell-pipeline-20261114`, same boot):**
- **8 buffers:** serial → pipelined took the displayed mean from 41.6 to 33.3 ms, but swaps fell from 86 to 59 and worker idle from 42% to 7%.
- **Bug:** the next game window incarnation (2-buffer arm) got zero direct frames, and the renderer submitted ~13k frames continuously.
- **State:** stays off (serial default) until fixed in `work/pipelining`. The Dell is back on the serial supervisor (job, same boot).
- **Process slip:** this image was deployed although its QEMU check reported `passed:false` (both attempts were OVMF load stalls; the recheck passed). New `scripts/dell_deploy.py` refuses to deploy without a passing verdict; use it for all deploys.

**TSC clock (gen 367, `dell-batch2-tsc-20261114`):**
- `clock status` reports `tsc`/`invariant-tsc`. Under load, monotonic tracks TSC; LAPIC ticks lose ~7%, which no longer matters.
- **First real-time Quake numbers:** displayed mean 49.7 ms (8 buffers) and 43.0 ms (2 buffers), i.e. ~20 and ~23 displayed fps; swaps 70.8 and 36.7/s.
- **All earlier fps figures were inflated by the LAPIC-tick clock.** Use these as the baseline.

**15 Nov, gens 398–402 (with Ian):**
- **Running on the Dell:** gen 402 `dell-integration9b-20261115` = `work/integration9` 51fa9401. It contains:
  - multilayer v3 + opaque-region culling;
  - touchpad (Synaptics absolute, two-finger scroll, jitter, speed, phantom guards);
  - the popup subsurface pointer fix;
  - VA-API 4 reset/flush tuning;
  - worker recovery + containment;
  - proc-cleanup;
  - the Chrome warning policy;
  - opt-in zero-copy auto-arm.

  **Not merged to main yet.**
- **Open, urgent:** the v3 layered offer is sometimes never PRESENTED, so frame callbacks are held and Chrome freezes (audio continues). Seen with a 2-layer commit (an ad overlay). With the multilayer agent.
- **Chrome:**
  - GPU-direct works after the client≠VFB size fix (windowed ~45 ms on gen 400).
  - Maximised is slow: 12.8 Mpix shaded per 5.76 Mpix frame; opaque culling is meant to fix it, but `opaque_single`=0 so far.
  - Zero-copy hurts blending (write-through readback): composites 30 → 71 ms. Keep it off until direct scanout and damage land.
- **Panics:** 40 (renderer job, CSB) is now recovered without a panic. Containment means a failed compositor job no longer marks client buffers. The site=2 cause is not yet diagnosed (new 3-line logs).
- **VA-API:**
  - Chromium VA-API works with i965 carrying static softpin libdrm (`brook-vaapi-0.3`, **not staged**), but it's slower than software until the async VCS lane lands.
  - Firefox VA-API, VP9 0.16× and the YouTube H.264 preference are pending.
- **Touchpad (Ian):** speed OK but slightly fast (now −20%, settable), vertical scroll worked, the rest fixed in later builds.
- **Chromium sandbox:**
  - Brook lacks PID/net namespaces, unshare/setns and seccomp, so `--no-sandbox` stays and the policy hides the bar. Scope: `docs/chromium-sandbox.md`.
  - No Landlock exists in the tree.
- **Clearwater Next** (`work/ui-fable`, Fable): Ian chose the **glass** theme and **Mixed** as default; the customisable accent palette (~10 hues incl. hot pink and phosphor green) is in its final pass. Launcher organisation, edge-to-edge caption hit areas and the theme architecture are in DESIGN.md. The screenshots are large (~45 MB incl. history), so prune before merging.
- **Git identity:** `~/.config/git/identity` has an empty user.name since the reboot; commit with `GIT_AUTHOR_*`/`GIT_COMMITTER_*` env vars.

**15 Nov, gen 397 (`dell-integration6-20261115`: main + publish-batch fix + VA-API 2a, video engine built):**
- **Publish-stage panic fixed** (`ae899bce`). Table publication was capped at 128 pages and didn't grow with the 512-table limit; it's now batched in 64-page chunks up to 1,024, and failure refuses the job.
  - Panics 37 (Firefox YouTube) and 39 (pid 100 about 50 s after boot, likely a browser session restore that left the Dell unreachable until AC-cycled) were both this bug.
  - On gen 397: Firefox (362 app jobs) and Chromium (3,446) play YouTube on the GPU path for 90 s each, no panic, no refusals.
- **VA-API works on the Dell:**
  - `vaapi-brook-pkg` closure staged create-only (only `libdrm-brook-softpin` was new; 126/126 present), with Ian's approval.
  - `vainfo` (i965, `LIBVA_DRIVERS_PATH=/nix/store/b9fz5qks…-brook-vaapi-0.2/lib/dri`, `/dev/dri/renderD129`) lists H.264/MPEG-2/VC-1/JPEG/VP8 VLD plus H.264 EncSlice and VideoProc. The only failed ioctl is GETPARAM 42 (HuC, n/a on BDW).
  - **ffmpeg `-hwaccel vaapi` decode of a 720p H.264 clip: 60/60 frames bit-exact vs software** (`-pix_fmt yuv420p`), 120 VCS jobs, 0 failures, ~5.5 ms per job.
  - GETPARAM correction: HAS_BSD is 10, not 5.
  - Evidence: `display-perf/build/vaapi-gen397b/`.
- **Git identity:** since Ian's host reboot, `~/.config/git/identity` sets an empty user.name. Commit with `-c user.name/-c user.email`.
- **Next:** VA-API phase 3 (Chromium VA-API on Ozone/Wayland, Firefox via ffmpeg, an H.264 preference for YouTube), then phase 4 hardening (keep VCS state between jobs instead of two media resets per job).

**Early 15 Nov (gens 394–395):**
- **Merged to main:** `d26b3612` (vaapi chain plus steam), then `8611cda1` (browser-gpu).
- **GPU browsers are the default on the Dell:**
  - new store path `p0mwsladz…-brook-hardware-network-launchers`; `/nix/profile/bin/{brook-chromium,brook-firefox,brook-iris-run}` relinked; rollback `/nix/.brook-profile-rollback-42b6c3acfe4b.json`;
  - Chromium ANGLE and Firefox hardware WebRender confirmed via the normal launch path.
  - **Firefox VP9 plays at 0.16× in both GPU and software modes** (Chromium plays the same clip in real time), so it's a Firefox media-path issue.
- **CPU idle A/B** (`work/cpu-idle` 7f94cea6, gen 394):
  - firmware EPB=0; MWAIT, ARAT and interrupt-break supported.
  - Idle package power: HLT 4.67 W → EPB6 4.05 → C1E 4.02 → C3 3.29 → **C6 3.22 W** (57 °C vs 63).
  - yquake p50: HLT 18.0, C1E 18.7, C3 21.2, C6 20.9 ms. Zero audio gaps throughout.
  - Next: a menu-style idle governor (`cpu idle state auto`) plus EPB 6 at boot.
- **VA-API 1c passes on the Dell** (`work/vaapi` 29818357, gen 395, `vcs_store_probe` tool):
  - 5,503 user `I915_EXEC_BSD` jobs, 0 failures (mean ~2 ms per round trip);
  - no effect on yquake (19.8 ms with VCS load) or zero-copy;
  - the gate toggles correctly;
  - an injected hang is detected at 2 s and quarantined, no panic.
  - Next: phase 2a (libdrm_intel softpin, GETPARAMs, tiling metadata, WC mmap). The `vainfo` trace on the Dell needs Ian's approval for the store write.

**Night of 14 Nov, unattended (gens 387–393):**
- **Gen 387 panic** while unattended, during yquake with idle windows open: `Broadwell worker uncertain … gt=0 … sampled=0` on a compositor job. Five early-exit paths before any device access were classified uncertain, and two also poisoned the forcewake claim. Fixed in `work/worker-refusal` 2bbe1ef1: classified as RefusedUntouched, retried up to 16×, with the reason logged.
- **Gen 386 panic** (GPU Chromium, 65k-page job): fixed in `work/worker-overhead` 67a27e3d. The table limit went 128 → 512, and unmappable jobs are refused (`ENOSPC`, or "job refused"), never panicked.
- **Gen 388 soak** (`work/worker-refusal` = integration5 + fix), ~25 min unattended: **no panics, restarts or refusals.**
  - Benchmark alone: mean 19.9 / p50 18.3 ms. With Gallery + Settings open: 21.7 / 21.3 ms (6,648 keeps / 103 captures).
  - GPU Chromium YouTube for 90 s: 3,492 app jobs, fine.
  - 15 min live demo: 74–79 °C, ~10 W, CPU pinned at ~2.9 GHz; **package max 100 °C since boot**.
- **Worker overhead per job** (gen 387): prepare 0.56 ms (attach 0.22, publish 0.16, snapshot DMA 0.17), reprepare 0.20 ms, audit 15 µs; batch fallbacks 0.
- **VA-API phase 1 complete** (`work/vaapi` 2693a25b, gen 393, `BROOK_BDW_VIDEO_ENGINE=ON`, runtime `gpu video on|probe|reset|selftest|pm …`):
  - The VCS self-test passes 3/3 with zero-copy armed and no effect on desktop fps or clock.
  - Found along the way:
    - **a VCS media reset resets RING_MAX_IDLE 10 → 64, so RC6 drops to 0% and the GPU clock sags;** firmware values are now restored after every reset;
    - holding the diagnostic lease across VCS retries starved presents and stranded a zero-copy ring image; VCS steps now run as short queued steps on the CPU0 worker.
  - **Open (zero-copy path):** any diagnostic holding the lease while zero-copy is armed (e.g. `gpu flip probe`) can still strand an image.
- **Next:** VA-API 1c (user BSD submission) then 2a (libdrm_intel softpin patch, packaging); MWAIT idle and energy bias (thermal safety); merge the integration chain to main.

**Late 14 Nov (gens 385–387, Ian present until ~20:45):**
- **PPGTT builder made linear** (`work/ppgtt-cache` d90993bf). Gen 385, 2-buffer demo: prepare 2.0 → 1.04 ms, compositor 12.9 → 11.6 ms, swaps 57/s, displayed mean 19.9 / **p50 17.6** ms. The table-reuse cache gives no gain (132 hits of ~1.5k); leave it off.
- **Chrome keep** (`work/chrome-capture` 389b0d69): window chrome is cached by content key, so steady frames copy 0 bytes (~15 MB per frame before).
- **Live yquake attract demo** (Ian): **65–70 fps** on gen 386/387 with no other windows.
  - With App Gallery plus Settings open it drops to 49 fps: VKRENDER at 18.8% CPU re-copying idle CPU windows every frame.
  - Fixed in `work/capture-keep` ca5ee5d1 (gen 387). Ian: "a lot faster once it settles"; the windows A/B isn't measured yet.
  - The ~4 s settle at Quake start is I/O: AHCI reads plus FAT mutex contention (`FatObjectStat`/`Child`), machine 85% idle; profile in `display-perf/build/prof-q2-startup/`.
  - yquake2's fps counter is a 60-frame rolling mean.
- **Chromium on the GPU path** (ANGLE/GLES via the staged brook-iris-run; flags from `work/browser-gpu` data/nix-wrappers/brook-chromium): YouTube is "miles better" windowed, maximised and fullscreen. The APP graph was blank for GPU clients; fixed in `work/thermal`.
- **PANIC on gen 386 with GPU Chromium:** `Broadwell worker recovery stage=prepare … pid=294 objects=68 pages=65451` (`PanicWorkerStage` < `ExecuteRetainedWorker`). A 256 MB job exceeds a prepare limit (likely the 128-page table inspection), and prepare failure panics instead of failing the job.
  - The guard reset to recovery, the panic was retrieved (`display-perf/build/panic-gen386/`, decode with `tools/crash_decoder.py --packet … --elf`), and it rebooted automatically.
  - Fix in progress with the PPGTT agent. **Don't run GPU Chromium until it lands.**
- **Thermal** (`work/thermal` 729be038): per-CPU sampler, taskbar TEMP graph, the `hw_thermal` bridge tool and `thermal status|history|graph|power`. Idle is `sti;hlt` only (C1); the next step is MWAIT C6 behind a switch (`docs/cpu-power-management-scope.md`). Ian: the fan spins gently, the laptop is warm but not hot. Package 77–85 °C sustained under yquake, peak 99 °C since boot.
- **Worker overhead** (`work/worker-overhead` 0c2c291e): batched memory-type audit and `gpu worker overhead` breakdown. Not measured on the Dell.
- **VA-API milestone started** (`work/vaapi`): phase 1 is the VCS engine (estimated 1.5–3 weeks total). Broadwell decodes H.264/VP8 only, so YouTube needs an H.264 preference.
- **Ian's next big direction:** a fully GPU-driven shell, i.e. the kernel sends draw descriptions (rects, glyph runs, icons) instead of CPU-rasterised overlay and chrome.

**M4a and GPU clock on the zero-copy base (gens 382–383, 14 Nov night):**
- **M4a wakeups** (`work/m4a-zc`, runtime switch `gpu wakeups on|off`, default off):
  - stable: 0 IF restores over 100k+ wakes, no panic;
  - no clear gain: displayed 21.2–23.1 ms vs 21.8 off; game queue wait 8.9–9.1 ms either way, because it waits behind the serial compositor job.
- **Profile** (wakeups on):
  - gpu-submit spends ~1/3 in `BdwBuildPpgttImage` (per-job PPGTT rebuild, 2 ms prepare) → `work/ppgtt-cache`;
  - VKRENDER is mostly in `sys_brook_desktop_layers` (window chrome re-rendered and copied each frame) → `work/chrome-capture`.
  - The profile filled `/boot` (32 MB FAT) after ~1/3 of 12 s; keep profiles ≤ 8 s or free `/boot` first.
- **GPU clock** (`work/gpu-clock`, `gpu clock <MHz>`, caps 300/300/950 MHz, thermal fallback at 90 °C):
  - at ~870 MHz achieved: compositor job 10.1 ms (12.9 at 500), game 60.2 swaps/s;
  - **but the package reached 92–94 °C under yquake even at 500 MHz**, so the fallback tripped at every setting.
  - **Safety:** Brook has no CPU P-state/C-state or fan management. Ask Ian whether the fans run. Don't run sustained high-clock tests unattended.

**Zero-copy reaches ~45 displayed fps (gen 381, `dell-zero-copy-carry-20261114`, `work/zero-copy-fix3` `1c66453f`):**
- **Bug fixed:** the cache-mode change had fed full-draw regions back as the next frame's damage, so most frames were full redraws.
- **Matched 2-buffer demo, wt armed:**
  - displayed mean **22.3 ms**, p50 20.2, p90 24.4 (copy path: 33.3 / 30.0 / 41.8);
  - swaps 49.7/s (31.6 copy); compositor job 13.1 ms; present 0;
  - 804 of 805 latched in 1 vblank; `verify` coherent; 0 restarts.
- **Flush mode** still writes back ~13 MB per present. Keep wt.
- **Next:** M4a wakeups ported onto this base (`work/m4a-zc`), then per-job overhead (M5). The game/worker pipeline is now the limit.

**Evening, 14 Nov (gens 377–379):**
- **Renderer restarts, two causes, both fixed:**
  - client-buffer CPU-transition race (`c975d4a7`);
  - hasvk adds every live VkDeviceMemory to each execbuffer, so cached ring-image imports made a quad job reference two tickets' outputs (`b9d05868`, per-frame import).
  - Gen 379: 0 restarts over 4 yquake runs (gen 378: 13).
- **Zero-copy cache modes** (gen 379, `gpu zerocopy cache flush|wt|uc`), 2-buffer demo:

  | Mode | Swaps/s | Displayed mean | Compositor | Worker present |
  |---|---|---|---|---|
  | off | 31.6 | 33.4 ms | 12.6 ms | 11.0 ms |
  | flush | 35.3 | 38.6 ms | 16.0 ms | 7.6 ms |
  | **wt** | **44.9** | **31.4 ms** | 16.0 ms | 0 |
  | uc | 45.1 | 31.3 ms | 16.0 ms | 0 |

  - wt is the chosen default.
  - **Remaining blocker for 60:** displayed p50 is quantised to ~33 ms although 98% of latches are 1-vblank (under investigation).
  - `verify` never sees an idle front: the taskbar graph presents continuously.
- **Live yquake attract demo with audio** (gen 377): Ian saw ~59 fps between hitches. The hitches were the renderer restarts above.
- **Chrome audio fixed on the Dell:**
  - store install of the q2mhvi0z chromium and 3wv97kbb firefox wrappers plus the downstream OSS plugin;
  - `/nix/profile` repointed; rollback record `/nix/.brook-profile-rollback-2ed5fc3e….json`;
  - Chrome 44.1k frames/s, 0 gaps; Ian: "sounds good, no fluttering".
- **Browsers on the GPU** (gen 378, `work/browser-gpu` launchers via `brook-iris-run`):
  - Chromium ANGLE: 606 app jobs, rAF p50 17 ms;
  - Firefox hardware WebRender: 869 jobs, p50 16.7 ms;
  - **shipping needs `brook-iris-run` in `/nix/profile`**, a store write awaiting Ian's approval.
- **Volume pop now mixes over playing audio** (cue streams, `22e9f017`); not yet heard by Ian.

**Afternoon session, 14 Nov (gens 372–376, with Ian):**
- **Integration branches:** `work/integration` (gen 374) → `work/integration2` (gen 376, `dell-integration2-20261114`, not yet on main). It has the audio downstream allowance, the zero-copy ring plus coherency, the waylandd fast blit plus socket guard, the `run --once waylandd` fix (counts INTLWL.ELF), Steam launch feedback, restart escalation, renderer exit-for-restart, the hardware volume plus pop, and the compositor idle fix.
- **Zero-copy:** the kernel side works on the Dell (latch in 1 vblank, 0 missed).
  - **Taskbar flicker:** caused first by per-context ring histories, then by write-back render writes (PAT slot 3 never UC). Both are fixed; Ian sees no flicker.
  - **Not measured armed yet:** the renderer hit `QUAD_UNCERTAIN stage=submit vk_result=-4` after `ASYNC_RETRY errno=11`, restarted (recovery works), and the new renderer can't re-register the ring (`errno=16`). Being fixed.
- **App routing:** apps now connect to INTLWL (the GPU path). Chrome YouTube went from ~170 ms to 46 ms windowed and ~90 ms maximized (~150 ms fullscreen, still CPU raster).
- **Volume:** the master volume now drives the DAC up to 0 dB (step 127); before, it was fixed at −23.5 dB. The pop is normalised to −0.1 dBFS. Ian: works well. **Bug:** the pop overwrites the playing audio instead of mixing (being fixed).
- **Idle CPU:** the 9% idle came from the CPU fallback after the renderer parked. With a healthy renderer on gen 376: 0 CPU overlay draws and 0 bytes copied over 10 s.
- **Restart hangs:** the services phase had no time limit. Now SIGTERM, SIGTERM again at 3 s, SIGKILL at 8 s. "Closing apps" can still block on windows that ignore close (Ian power-cycled twice).
- **Steam:** Xephyr fails because the desktop `/tmp` has no `link()` (lock file). The workaround `-displayfd` is on `work/steam`; proper fix: tmpfs `link()`.
- **Backlog added:**
  - repair progress UI with a distinct icon (Ian saw 92% then reboot);
  - tmpfs `link()`;
  - Chromium GPU stalls after the first frame (`work/browser-gpu`);
  - the Chrome audio store install, awaiting Ian's explicit approval.

**Browser and audio captures (gen 371, 14 Nov, Ian watching):**
- **Chrome YouTube** (windowed; slow, fluttering audio):
  - mixer stream 34.5k frames/s against the HDA's 44.1k;
  - 22% of output is silence (later gaps 9.4k frames/s, 26 events/s);
  - stream capacity 1920, fragment 64, queue ~77% full.
  - The mixer starves this stream (10 ms / 12.8 ms = 0.78). Being fixed in `work/dell-audio`.
- **Firefox YouTube** (fullscreen; audio fine): 44.24k frames/s, zero gaps, capacity 4416. This is the healthy control.
- **Both browsers render in software:** `gpu worker stats` class `app` = 0 jobs during playback.
- **The CPU bottleneck is the user waylandd** (`/nix/profile/bin/waylandd`, `4p01fkxz…`), at ~24% of all CPU (one core). The hot spot is its CPU scene composition (`surface-scene.h` `pixel_over` plus per-pixel `idiv` scaling); multi-layer and scaled windows take this path.
- **Frame times:** ~170 ms windowed (~5 fps), ~2 fps fullscreen.
- **Work started:** `work/browser-gpu` (why software) and `work/waylandd-compose` (fast CPU path plus multi-layer GPU publication).
- **Evidence:** `display-perf/build/{chrome-audio-gen371,firefox-fs-gen371}/`.
- **Volume ceiling:** the Dell stream reuses the speaker test's fixed DAC gain (`hda_dell_tone_plan.h:38`, step 80); taskbar volume is software-only. Proposed: drive the DAC gain from the volume control, tested with Ian present.
- **Mouse judder:** the frame time drops 17 → 8 ms while the mouse moves; the compositor renders at ~125 Hz into an unsynced 60 Hz GOP. Zero-copy flips or a refresh cap should fix it.
- **Zero-copy scanout** (`work/zero-copy`) is the primary display milestone.

**Timing loader promoted on the Dell (14 Nov, with Ian):**
- **Problem caught before deploy:** the boot-timing loader's protocol version bump to 6 would have disabled EFI power-off on every existing kernel; `efi_runtime_inventory.h` and `platform_power.cpp` accept only v5. Fixed in `992e0879`: the handoff stays at v5 and the timing block sits behind an `extensions` marker. QEMU tested every old/new loader and kernel pairing.
- **Promotion:** from the USB maintenance boot via `scripts/dell_loader_promotion.py` (runbook `docs/dell-loader-promotion.md`). The inventory matched the 7 Nov hashes. `BROOKT6.EFI` (`a42be35c…`) was staged and promoted with `GUARD_DEFAULT_OK`. The old v5 default is kept as `BROOKP5.EFI` (`c717c048…`). F5, RECOV2, RECOVERY and the guards are unchanged.
- **Booted the SSD first time:** on gen 370, then gen 371 `dell-main-timing-20261114` (main). EFI power-off is still `efi-runtime`.
- **Gen 371 `BOOT_TIMING`:** firmware 5,079 ms (FPDT), menu wait 500 ms, **loader 3,724 ms**, kernel 2,863 ms, total 12.2 s; no keys seen during the wait (`wait_keys=0`, Ian didn't press any).
- **Next:** the loader's 3.7 s (32 MiB initrd plus kernel read and guard hashing) is now the largest controllable boot cost; profile it.
- **Rollback:** promote `BROOKP5.EFI` back with the macro variant described in the runbook, or boot F5 or the USB stick.

**M3 copy-based flip on the Dell (gen 370, `dell-m3-flip-20261114`, `work/m3-flip` `f103889e`):**
- The GGTT takeover fix (`8c146e19`) worked: probe passed and `gpu flip arm` went active (3 kernel surfaces, 5,753 inherited PTEs taken over, 32 ms setup, 0 GOP returns, still active after both arms).
- **It is slower.** 2 buffers: displayed mean 39.5 ms (p50 41.5, p99 56), present 16.3 ms (blit 15.5) against 33.2 ms and 10.9 ms on gen 369. 8 buffers: 45.8 ms mean.
- **Cause (by design):** each present still BCS-copies the renderer output, plus repair rectangles, into a kernel scanout buffer before writing DSPASURF. The copy got bigger rather than going away.
- **Decision:** leave flip runtime-off (`gpu flip off`); don't tune it. The next structural step is **zero-copy scanout**: DSPASURF points at renderer output images allocated and imported so they can be scanned out, removing the per-present copy and its BCS overhead. Evidence: `/data/Programming/Brook/display-perf/build/m3-flip-gen370/`.

**Pipelining default (gen 369, `dell-pipeline-default-20261114`, boot default):**
- **2 buffers:** displayed mean 33.2 ms (≈30 fps real time), p50 30.0, p99 49.7; swaps 31.7.
- **8 buffers:** 37.4 ms mean, p99 83.5.
- **vs serial on the TSC clock:** 42.5 ms mean and p99 95.
- **Opt-out:** `BROOK_RENDER_PIPELINE=1`.
- **Gotcha:** a supervisor started by hand as a bridge job blocks a checked restart in "closing apps". Stop the job (cancel the restart first if it's already stuck), then restart.

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

- **Keyboard shortcuts (implemented on `work/shortcuts`, QEMU-verified, not yet on Dell):** Super tap toggles the launcher (Escape closes it), Super key-down revokes pointer capture, Alt+F4 sends the normal advisory close. State machine `src/kernel/src/global_shortcuts.h`, host test `test_global_shortcuts`; evidence `build/shortcuts-check/`. Wayland lock release on hardware with a current waylandd and yquake2 is still unobserved (the QEMU seed's waylandd predates pointer constraints).
- **Dell application audio for yquake2:** the gen 363 boot has no `/dev/dsp`, and the OSS OpenAL store path (`59h20vp…-openal-soft-oss-brook-1.24.3`) is present but has no `lib/`. Restore the gated Dell HDA path (`/etc/dell-hda-audible`; see `docs/xps9343-bringup.md`) and stage OpenAL, then verify audibly with Ian. The SDL build has no OSS backend, so OpenAL is required. The `/data/home` yquake2 config also had a stale absolute `al_driver`.
- **Browser video and audio on the Dell (Ian, gen 363, 14 Nov):**
  - **Chromium YouTube:** audible, but music plays SLOW; frame-time p80 ~90 ms.
  - **Firefox YouTube:** video and audio both run FAST, with no obvious audible stutter; ~3–4 fps, frame-time p80 ~174 ms.
  - **Suspicion:** the playback clock or position reported to apps (GETOPTR/GETODELAY or the bridge) is wrong; see `docs/audio-clock.md`. Being investigated in `work/dell-audio`.
  - **Acceptance check to add:** fullscreen YouTube with no audio stutter, judged from renderer frame pacing plus all-zero packets in mixer output.
- **Volume persistence:** taskbar volume changes apply live but are only saved by Settings' Apply. Decide whether taskbar changes should persist automatically.
- **Loader F5 wait ignores input on the Dell:** works in QEMU/OVMF. Boot the timing loader (branch `work/boot-timing`), press keys during the wait and read `BOOT_TIMING_DETAIL wait_keys=`; zero suggests ConIn isn't connected on the fast-boot path. See `src/bootloader/src/DOCS.md`.

- **Automatic slot rollback in the guard:** after N consecutive desktop panics on a newly activated generation, select the previous verified slot instead of looping in recovery-only mode. Also expose a slot-select operation in the recovery-only bridge.
- **Split `src/kernel/src/syscall.cpp`** (17k lines) by area: memory, files, process/signal, sockets, epoll/timers and Brook-specific.
- **Remove or declare the twelve orphaned `tools/netsurf-build/*` gitlinks** (they have no `.gitmodules` entries).
- **Transparent kernel rendering interfaces,** so console and shell output uses the GPU path.

Smooth Chromium/YouTube playback (about 90% frame drops at 720p60), a Dell Steam client launch with audio, the bridge-loss watchdog (design only), the Xwayland `setitimer` interface, and promotion of the patched Iris build to the default launcher.
