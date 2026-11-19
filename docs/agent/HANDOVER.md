# Brook handover

Current state and immediate goal. Keep this short and current: replace stale lines rather than appending. Put detailed run records in [AGENT_JOURNAL.md](AGENT_JOURNAL.md), the previous agent's ~1 MB append-only log (newest entries at the top). Decisions belong in [DECISIONS.md](DECISIONS.md), and working preferences in [WORKING_STYLE.md](WORKING_STYLE.md).

_Last updated: 19 November 2026 (early), by a Claude Code session; see "19 Nov early: resume here"._

## Workspace

- **Root:** `/data/Programming/Brook/`. `main/` is a fresh GitHub clone kept on `main`. Each agent or task gets its own worktree, `git worktree add ../<task> -b <branch>`, with its own `build/`. Never share a build directory or drive the Dell from two agents at once.
- **Shared private assets:** `/data/Programming/Brook/shared/dell/` (mode 700, never commit), with `SHA256SUMS`:
  - `hardware-update-key`;
  - `source-initrd-cc876d82.img`;
  - `rollback-gen355/` (the known-good pair);
  - `quake-probe-bundle.tar` (demo timing script, swap trace library and paks).
- **Legacy checkout:** `/data/Programming/Codex/Brook` and its `build/hardware/` evidence are reference only. Old disk images are being cleaned up separately.
- **Build:** `nix-shell --run './scripts/build.sh Release'` from a clean checkout, then `ctest --test-dir build/release/host_tests`. `test_audio_delivery` and `test_sched_lock_owner` can time out under parallel load; rerun them alone.

## Repository

- Work from `main` (`dc12f39e` at the time of writing). On 13 November at about 23:31 UTC it absorbed both lines of the previous agent's work: the main checkout's long-uncommitted changes (`d6724e11`), and the private checkout `/tmp/brook-main-BrURO4` (`dcb99b97`, `bd0ae24b`).
- `work/steam-desktop-hardening` is superseded. A stash backup of its final worktree, `stash@{0}`, is identical in content to `d6724e11`.
- The detached worktrees `Brook-gpu-lock-control`, `Brook-pre-timing-7d005ae0` and `Brook-timing-bisect`, and `/tmp/brook-main-BrURO4`, are historical. Their contents are in main. Do not develop in them.
- The `build/` tree holds hundreds of GB, mostly disk images. Never copy it wholesale.

## Active goal (set 14 November 2026)

**Met (Ian, 18 Nov, gen 439):** Ian has declared the performance goal met for now. The matched Q2 demo gives 66.8 swaps/s. Quake II fullscreen runs at 56 fps (640×480) and 38 fps (1080p, compositor-scaled). Stardew gameplay runs at 39 fps at 600 MHz, and the Torchlight II menu at 25 fps. The next focus is compatibility and desktop quality.


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

**19 Nov early: resume here.**
- **The Dell is on gen 440:** `work/integration33`, payload `dell-int33b-20261118`, session `e66205da44274cc59d7b75a0899ce4bc`. gen 440 = int32 plus:
  - the per-window hidden cursor (`work/cursor-hide`; Ian confirmed it fixes Q2 and Stardew);
  - Clearwater stage 1.1/1.2 (`work/clearwater-s1`: part table, edge-to-edge caption hits, outer resize margins, display lists, 144 goldens; Ian has not checked it yet);
  - the 600 MHz boot clock;
  - thermal-trip hysteresis (recovers after 5 s at ≤75 °C).

  int32 = int31 + app-driven cadence + hang diagnostics. The cadence is OFF on the Dell; I turned it off live.
- **Ready, not deployed:** `work/integration34` (b2a53743), pair `/data/Programming/Brook/integration27/build/dell/pair-int34`, payload `dell-int34-20261119`, QEMU PASS. It adds:
  - `work/app-present` 9618802f + 96994652: the cadence never defers over shell surfaces, popups, a pointer outside the game or a paused app, and defers at most 33 ms (Ian: aim for the next 33 ms slot);
  - `work/gpu-poll-starvation` 5a5da780: deadline grace plus stall instrumentation.
- **The gen 440 "GPU hang" was a false failure:**
  - **What happened:** the RCS job completed (started marker and breadcrumb both equal the nonce), but one `BdwYieldGpuWait` on the CPU-0-pinned gpu-submit thread returned after ~2.19 s. The 2000 ms deadline reset a finished job; it was marked sticky-failed, Mesa reported device lost and Q2 crashed. The renderer skipped the failed buffer, so the desktop flashed.
  - **Scope:** the gen 428 and 16 Nov `submit=10` failures share the signature.
  - **Fix in int34:** one more breadcrumb/CSB check at the deadline (`late_completions` in `gpu hang status`).
  - **Root cause still open:** Brook never preempts ring 0, so a ~2 s ring-0 run or an interrupts-off stretch on CPU 0 would do it; a `cp` read/write fallback was running. The new `gpu-hang:  longest yield` line, `from_pid`/`from_ran_us` and `apic_ticks` will name it.
  - **Evidence:** `display-perf/build/dell/q2-440-launcher/`; reading guide in `docs/gpu-hang-diagnostics.md`.
- **Next steps:**
  1. Deploy int34 while Ian is present.
  2. Q2 fullscreen with the cadence off (baseline 34.4 fps), then `desktop present app on` and the checks: launcher over Q2 with the pointer moving (Ian saw a ~5 fps stutter on gen 440, probably the starvation), a paused game, a popup over the game, Alt-Tab. Counters are `app_present.actions` (shell, window_above, pointer_outside, app_stalled, app_not_due, defer, deadline).
  3. Ian checks the Clearwater hit areas at 200%.
  4. If all is good, merge `work/integration34` to main (int32 and int33 are not on main yet; main has int31).
- **TRIM:** Codex finished `work/ssd-trim` 5afe1344 (in `/tmp/brook-ssd-trim`; docs/ssd-trim.md, QEMU pass, reviewed: 30 s DSM timeout, timing in the summary, multi-command fixture). Each physical step needs Ian's approval. Step 1 is a dry-run maintenance pair (IDENTIFY check plus the range list), reviewed against the partition table.
- **Stardew gameplay at 600 MHz:** 39 fps, or 29 fps when the old latched trip fired. It is GPU-bound: game 10–19 ms plus compositor 14 ms per frame.
- **Q2:** fullscreen 1080p 34–38 fps, 640×480 56 fps. Ian declared the performance goal met. Panel-fitter "true fullscreen" is in the backlog.

**18 Nov late, gens 432–438 (autonomous, Ian away; current image gen 438 = `work/integration31` 39bf431f, payload `dell-int31-20261118`, session `c074200fc09541a4a0897600dc5bf5c5`):**
- **Gen 430 merged to main** (`b6dfbb0d`; Ian confirmed fullscreen).
- **Xwayland died when Steam opened a window (gen 432, int27 tiled buffers):** iris maps shared/scanout BOs with `I915_MMAP_OFFSET_WC`, which the kernel refused. WC is now served write-back like legacy GEM_MMAP (`f4be9ee0`).
- **GPU clock stuck at 300 MHz since Ian's forced restart (Q2 demo ~65 → 47 swaps/s):** the render-clock request is gated on AC Online, and the Dell ACPI monitor refused admission. Cause: the MADT's processor-4 LAPIC NMI flags/LINT# bytes are uninitialised firmware memory and changed (0x6ceb/4 → 0x6cef/5). The pin now hashes a normalised MADT (`b821c97c`, `AcpiDellNormalizeMadt`, host test); admission logs which check failed. AC online again; Q2 demo 66.8 swaps, mean 22.0 ms, jobs at 500 MHz (`display-perf/build/dell/q2-436`).
- **Per-page mapping-mutex convoy removed** (`work/mapping-convoy` d8cd5d26: lock-free `UserMappingPageAccess` for present user leaves).
- **Global epoll mutex replaced by per-instance leaf locks** (`work/epoll-instance-lock` 42bdd722); the graph lock is only for epoll-in-epoll ADD.
- **Torchlight II menu (800×600, Steam open):** ~9 displayed fps (gen 433) → 23.2 (gen 437: clock + mapping) → **25.3** (gen 438: + epoll), with no epoll or mapping mutex left in the blocking profile. GPU ~75% busy now. Evidence: `display-perf/build/dell/tl2-{433,437,438}-menu/`. Gameplay not yet measured.
- **Stardew fullscreen 3200×1800 (gen 433, GPU at 300 MHz):** 22.6 displayed fps (was ~19 maximised): tiled buffers cut the game's GPU frame 30.8 → 19 ms. It is GPU-bound and the compositor spends 21.8 ms per frame redrawing a fullscreen opaque window. **Gen 438 at 500 MHz: 32.5 displayed fps** (p50 29.3, p99 47.5 ms; game 14.2 ms, composite 14.2 ms per frame, GPU 95% busy; `display-perf/build/dell/stardew-438-fs`). **Next:** direct scanout of fullscreen X-tiled client buffers to drop the composite.
- **Intermittent BDW submission failure hit Xwayland** on gen 438's first TL2 launch: the job never started (ring head=tail=acthd=0, no CSB), verified reset, sticky failure, so Mesa lost the device and Xwayland aborted (Steam exited). Same signature as gen 428. The second launch was fine. This is now the top stability bug.
- **Automation:** Steam games start without Ian by writing `<client>/steamrt64/steam steam://rungameid/<id>` to `~/.steam/steam.pipe` (uid 1000). The TL2 Mod Launcher is dismissed with the store's xdotool (`DISPLAY=:0`, windowactivate plus `key space`).
- **Known flake:** `test_intel_bdw_buffer` segfaults about 1 in 30 runs on the base as well.
- **To merge after Ian's check:** `work/integration31` (int26 + xwl-pacing + WC + ACPI + mapping + epoll).

**18 Nov night:**
- **Gen 428 merged to main** (`6776790d`, Chromium fixes; Ian confirmed resize works).
- **Graceful restart with a hung Chromium (YouTube):** windowless processes were escalated as designed (blockers 27 → 6), but Chromium pid 124 kept its window, ignored the WM close, and a bridge SIGTERM job timed out (jobs restricted while closing?). Ian had to force power-off.
  - **Gap:** windows that stop responding to close need a force-close step (e.g. after N s, offer or apply SIGTERM/SIGKILL to window owners, with the process named in the prompt), plus a way for bridge root jobs to run during closing.
- **Integration slip:** `work/launchers-fullscreen` was based on main da26b714, so its pair (deployed as `dell-fullscreen-20261118`) lacked gens 427/428. Corrected image: `work/integration26` = main + launchers-fullscreen. **Always integrate agent branches with current main before deploying.**
- **Quake II and VSCodium launchers staged** (bundle tx `apps1118`, store `0s7bcrci…-brook-dell-app-launchers`, rollback `install.py --rollback apps1118`). `brook-quake2 --check` OK. The menu reloads at next boot (the installer's 0xB03 notify needs ctypes, which the minimal python lacks).

**18 Nov evening, Dell gens 427/428 (main `678b009d` = integration24; gen 428 `dell-int25-20261118` adds `work/chromium-fixes`, session `fab0e3daae964a16aef47e58ed99acf7`):**
- **Gen 427:**
  - Xwayland windows are GPU sources (`work/xwl-dmabuf`); epoll no longer zeroes ~100 KiB per wait; kernel-region enter/leave is lock-free (`work/kregion-lockfree`).
  - Ready-lock acquisitions during the matched demo fell from 23.9M to 5.76M. Demo: 67 swaps, displayed mean 21.8 / p50 20.0 ms (~46 fps).
  - Evidence: `display-perf/build/dell/{goal-int24,lockstats-int24.json}`.
- **Torchlight II:** `src gpu`, all frames direct, but ~13 displayed fps. GPU 75% idle, CPU 76% idle: latency-bound, probably Xwayland Present pacing.
- **Stardew maximised (3192×1632):** GPU-bound (30.8 ms per game frame into LINEAR buffers, plus a 14.8 ms composite), ~19 fps; windowed is fine.
- **Both being investigated on `work/xwl-pacing`:** Present pacing and tiled modifiers.
- **Gen 428:**
  - Ian confirms the resize freeze is fixed (Stardew/Chromium). Not yet merged; merge after Ian's Chromium cursor/ads check.
  - **New:** Stardew stopped rendering before any resize. A GPU job (pid 223, now a zombie) never started (ring head=tail, no CSB). It was reset, and Mesa reported device lost; then DESKTOP_SCANOUT_STARVED repeated. Evidence: `display-perf/build/dell/stardew-hang428/` (klog, stats, waylandd). Handed to the `work/xwl-pacing` agent as priority.
- **Stardew audio:** still the null-sink Pulse in Steam's launcher (desktop Pulse server needed).

**Dell gen 426 `dell-xwayland-hidpi-20261118` (session `036dae3a34b34a55bf6c6cfdf12c13ed`; branch `work/xwayland-hidpi` `85496659`, not merged):**
- **brook-xwm staged:** `/nix/store/h9x75map…-waylandd-brook-0.1-brook`. Rollback record: `/nix/.brook-profile-rollback-xwm20261118.json` (previous `4p01fkxz…`).
- **Steam launcher `rootless-4` installed** (`ui_scale` 2; rollback via the bundle's `install.py --rollback`).
- **Ian to test:** Stardew fullscreen and mouse; Steam UI scale; X11 apps' sharpness/size (LibreOffice, VLC).
- **Then:** merge `work/xwayland-hidpi` to main, and `work/steam-rootless` + `work/steam-rootless-hidpi` (the launcher source).

**18 Nov later (Dell gen 425, main `c9e54176` with `work/brook-value` merged; Ian wants a docs pass at some point):**
- **`sched lock-stats` during the matched demo** (bare metal): ready lock 23.9M acquisitions/~76 s, 9.7% contended, 855 ms wait. KernelRegionTryEnter and SchedulerKernelRegionLeave are 11.2M each, 93% of the traffic, ~630 ms of the wait. Evidence: `display-perf/build/dell/lockstats-demo.json`. Fix in progress: `work/kregion-lockfree`.
- **Ian's Dell feedback:**
  - Chromium: the cursor sticks on the resize cursor after crossing the window edge; rendering stops after a resize; ads/iframes don't render. Fix in progress: `work/chromium-fixes`.
  - VSCodium errors on launch (needs a `--no-sandbox` wrapper).
  - No Q2 launcher yet. Both launchers need /nix/store staging approval (asked).
- **Approved:** staging the new brook-xwm. `work/xwayland-hidpi` is being rebased; then stage the bundle and deploy the pair.
- **Next step agreed:** Xwayland GL games onto dma-buf/direct layers (`work/xwl-dmabuf`, in progress).
- **AHCI spin budget:** keep 100 µs; A/B 25 µs on the next big Steam download (`bridge hardware ahci-spin-us N`).

**18 Nov: Dell gen 424 `dell-int23-20261118` (session `c59d4ec167b645e4b9449e8acc2c2aa9`); main = `work/integration23`** (shell-opaque, storage-io, sched-convoy, and the zero-copy auto-arm retry thread `f03e922e`).
- **Zero-copy active from boot** with no manual arm (the auto-arm retry works).
- **AHCI MSI works on the Dell's Samsung PM851** (vector 64; 4,754 of 17,941 waits slept on the interrupt).
- **Network healthy** (0 ARP failures).
- **Matched demo:** 63.7 swaps; displayed mean 22.8 ms / p50 21.5 (~44 fps), the best yet.
- **Q2 with the launcher open:** Ian confirmed no drop. Composite 4.24 ms (was 9.25), shaded 0.98 Mpx (was 3.1), ~66 displayed fps (p50 12.8 ms). Evidence: `display-perf/build/dell/int23-launcher/` and `goal-int23`.
- **To do:**
  - read `sched lock-stats status` (lock-stats is enabled on this boot) during Q2/Steam to confirm the KernelRegion enter/leave suspect;
  - Ian's hands-on checks: clickpad, power confirmation, Chromium caption colour;
  - `work/compositor-refactor` is rebased onto main 67922774 (head `495f0875`; stages 0.1–0.5 now also cover the power footer and the chrome-csd-style caption strip; QEMU CPU path pixel-identical; 369/369 host tests). Pair `compositor-refactor/build/dell/pair-refactor` (`dell-refactor-20261118`). **Merged after Dell gen 425 (`dell-refactor-20261118`, session `3009030bf1204b94a56f5210652ca594`):** the desktop renders normally, zero-copy is active from boot, and the matched demo is unchanged (65.6 swaps, displayed mean 22.8 / p50 21.0 ms). Ian still to eyeball the launcher power footer and the Chromium caption colour on it. Next refactor stage: 1.1 (part table for drawing and hit-testing).

**Ready, not deployed: `work/compositor-refactor` (`c5fab410`, rebased on main 73409c2a; trial merge with work/shell-opaque is clean).**
- **Stages done:**
  - 0.1: one shell-surface list;
  - 0.2: theme tokens and one metrics source;
  - 0.3: window.cpp split into `chrome_raster.{h,cpp}`, `wm_shell_taskbar.inc`, `wm_shell_launcher.inc` (4234 → 3155 lines, pure move);
  - 0.4: `chrome_alpha.h`, one opaque-frame rule for virgl and VKRENDER;
  - 0.5: shell damage driven by the list.
- **Checks:** QEMU CPU path pixel-identical; virgl glass path identical outside blur halos from the build label and caret. Soak: no freezes in 9 boots (QEMU stalls track host contention; see `work/sched-convoy`).
- **Next stage:** 1.1, a part table for drawing and hit-testing, absorbing chrome-csd's caption overlay. Stage 1.3 should extend shell-opaque's band opacity and keys to window frames.

**Suggested next integration (`work/integration23`):** main + `work/shell-opaque` + `work/compositor-refactor` + `work/storage-io` + `work/sched-convoy`, plus the zero-copy auto-arm fix (flip probe, then arm at renderer registration), then:
1. Dell measurements: Q2 with the launcher open vs closed (expect ~72 both ways); `bridge hardware ahci-ro` (MSI working?); `sched lock-stats` during Q2/Steam.
2. Ian's hands-on checks: clickpad, power confirmation, Chromium caption colour.
3. With Ian's approval for the brook-xwm store staging, add `work/xwayland-hidpi` (+ `work/steam-rootless-hidpi`) and retest Stardew.
4. Then the Xwayland dma-buf presentation investigation (Torchlight II and Stardew at ~10 fps through CPU copies).

**Ready, not deployed: `work/sched-convoy` (`b09d19e5`, `177f5240`).** Note: `docs/sched-lock-convoy.md`.
- **Cause of the QEMU freezes:** FIFO ticket-lock waiter preemption under an overcommitted hypervisor. Ready lock 86.7% contended; 588 s total wait vs 1.6 s total hold. Not long critical sections.
- **Fixes:**
  - unfair CAS claim when the CPUID hypervisor bit is set (FIFO fallback after 65,536 checks; `sched lock-mode ticket|unfair|status`);
  - idle CPUs skip the ready lock on ticks when nothing is queued (`sched idle-skip`, default on, also on bare metal);
  - new `sched lock-stats on|off|reset|status`.
- **Result:** unmodified main froze in every overcommitted run; the fixed kernel never froze (sleep loop 0.5–0.9 s, query p50 0.04–0.11 s).
- **On the Dell:** it stays FIFO; only idle-skip applies.
- **Leading suspect for the Dell's SchedLockRelease skid:** KernelRegionTryEnter/Leave take the ready lock twice per user copy. Not fixed. Confirm with `sched lock-stats` on the Dell.
- **Recommend:** boot the pair on the Dell (`sched-convoy/build/dell/pair-sched-convoy`, QEMU check PASS) and run `sched lock-stats` during Q2/Steam, then merge. It also makes QEMU testing reliable for agents.

**Ready, not deployed: `work/storage-io` (5 commits on f388d5ac).**
- **What it does:**
  - AHCI completion waits spin up to 100 µs, then sleep on MSI (vector 64, with a self-IPI to wake on the boot CPU);
  - automatic fallback to polling, and `/etc/ahci-polled` forces polling;
  - the panic journal keeps its polled path, plus a gate takeover when the owner is asleep;
  - ext2 doubly-indirect lookups use the indirect-block cache;
  - mapping-range walks start at the index lower bound;
  - the taskbar R/W now counts AHCI and USB mass storage (plus `/proc/diskstats`, and `storage` in `bridge stats`).
- **QEMU:** throughput unchanged; writer CPU per byte −16% (100 µs spin) and −35% (20 µs).
- **Risks:**
  - MSI on the Dell's 8086:9c83 is untested; test with `bridge hardware ahci-ro`;
  - the boot-CPU-panic journal path is host-tested only.
- **Ian's decision:** spin budget (`bridge hardware ahci-spin-us N` at runtime; 20–30 µs may suit CPU-bound downloads).
- **Not fixed:** the FileMappingGateSet design (it locks every mapped file per mmap call), and ext2's flush every 256 allocations.
- Pair: `storage-io/build/dell/pair-storage-io` (QEMU check PASS).

**Q2 attract demo on gen 423 (zero-copy armed by hand, Steam open behind it):** 65.2 displayed fps (1435 frames, mean 15.3 / p50 14.2 / p90 16.8 / p99 30.9 ms), all direct frames, Q2 3.2 ms, composite 5.5 ms (4.5 layers with Steam behind), GPU 38% idle, CPU 67% idle. Profile: `display-perf/build/dell/q2-423-prof/`. Ian saw 72 fps in game. Torchlight II has no sound: Steam's private Pulse uses a null sink (desktop Pulse server is a next step).

**Torchlight II runs on gen 423 (rootless Steam with machine-id) but at ~10 displayed fps.** Profile: `display-perf/build/dell/tl2-play-prof/`.
- **Not CPU- or GPU-bound:** app GPU jobs 11/s at 11 ms; CPU 72% idle; GPU 76% idle.
- **Its X window reaches the compositor as a CPU source** (`src cpu`, all nondirect frames, ~99 ms mean interval). Xwayland is presenting through SHM/CPU copies, not DRI3 dma-buf.
- **Kernel profile:** memset 9.3%, likely zero-filling freshly allocated GPU buffers (per-frame BO churn?), and SchedLockRelease 5.5% (see `work/sched-convoy`).
- **Next:**
  1. Why Xwayland GL games don't get direct dma-buf presentation (waylandd linux-dmabuf v4 feedback, LINEAR-only, Present/flip vs copy).
  2. Who allocates and clears buffers per frame (GEM create/zero in the i915 path).
- The window is 800×600 and tiny; the `work/xwayland-hidpi` fullscreen/DPI changes apply.

**Ready, not deployed: `work/xwayland-hidpi` (`817f8894`, `94b6fd27`, `98c4ff02`).**
- **Stardew offset cause:** brook-xwm ignored `_NET_WM_STATE_FULLSCREEN`. It now honours EWMH fullscreen (chrome-less, at the output size), sends synthetic ConfigureNotify, and restores the previous state on exit.
- **HiDPI:** Xft.dpi/XSETTINGS/`-dpi` hints by default; per-app compositor upscale via `BROOK_XWAYLAND_SCALED_APPS` (WM_CLASS list).
- **Limit:** Xwayland 24.1 has no rootless per-surface scaling.
- **Needs both halves:** the INTLWL in the pair (`xwayland-hidpi/build/dell/pair-xwayland-hidpi`) and a new `/nix/profile/bin/brook-xwm`. The store staging bundle is `xwayland-hidpi/build/dell/xwm-stage-20261116/bundle.tar` (stages `h9x75map…-waylandd-brook-0.1-brook`); **it needs Ian's approval**.
- **Steam UI scale follow-up:** `work/steam-rootless-hidpi` (`132ff9e5`, on top of `work/steam-rootless`).
- **Still wrong:** the taskbar draws over fullscreen games (kernel compositor follow-up); apps that ignore DPI hints stay tiny unless listed.

**Gen 423 deployed (`dell-int22-20261116`, session `2074aa4179ba4cb3a864f2539379acc6`; `work/integration22` merged to main):**
- **Network:** ARP 2 entries, 0 failures, a queued frame released correctly. Bridge stable.
- **Zero-copy auto-arm doesn't fire at boot:** `auto_arm: true` but "not requested". Auto-arm only triggers at renderer ring registration, before any flip probe has passed. A manual `gpu flip probe` + `gpu zerocopy arm` works (I armed it on this boot).
  - **Fix next:** run the flip probe automatically at registration (or at boot), then arm.
  - Until then the matched demo runs on the copy path (~33 displayed, 43.5 swaps).
- **Ian to test on this boot:** clickpad hard click (bottom-right = right); launcher Restart/Shut down confirmation; Chromium caption colour (waylandd log `caption style … strip=#…`).
- **Steam launcher `rootless-3` installed** (machine-id + /etc files for Steam's containers): retry Torchlight II (app 200710). Ian's Steam launch option `%command% > $HOME/t2.log 2>&1` writes to `/data/home/steam-20261022/user/t2.log`.
- **Ready, not deployed:** `work/shell-opaque` `3f916b91` (opaque keyed shell bands, so the launcher costs no game frames; measurement steps in its commit/report: Q2 with the launcher open vs closed, expect ~72 displayed). Pair `shell-opaque/build/dell/pair-shell-opaque` (main f388d5ac + fix). Rebase onto main and rebuild with the zero-copy probe fix.

**16 Nov wrap-up (usage limit; finish in-flight work, start nothing new). Dell on gen 422 `dell-int20-20261116`, session `c6affe0f76244436bd0aa9c0aedac0d0`, Steam closed.**

Done today, on main up to `f388d5ac`:
- **60 fps:** live yquake2 with zero-copy armed displays 72 fps (p99 18 ms). The matched demo is ~40–43 displayed (~33 without zero-copy, ~12 on gen 419).
- **Also on main:** occlusion and direct scaled sources, pointer relock and Alt+Tab, Steam working (rootless launcher installed on the Dell), the Quake audio recipe, and the decisions recorded in DECISIONS.md.

**In flight — finish these, in order:**
1. **`work/integration22`** = main + `work/restart-harden` + `work/clickpad-buttons` + `work/zc-default` + `work/net-arp` + `work/chrome-csd-style` + `work/power-guard`.
   - Build, run `check_dell_image_qemu.py`, deploy (the Dell is idle).
   - Verify: `gpu zerocopy status` shows active from boot; the matched demo; bridge/ping stay healthy (`bridge net stats`, `bridge net arp`).
   - Then Ian tests: clickpad hard click (bottom-right = right, `input touchpad status` shows range and soft_buttons), launcher power confirmation, Chromium caption colour (waylandd log `caption style … strip=#…`).
   - Merge to main if good.
2. **Agents still running.** Each will report a branch plus a Dell pair; integrate the next image from main + these:
   - `work/shell-opaque` (`3f916b91`+): opaque launcher/shell bands, so an open launcher costs no game frames. Measure Q2 with the launcher open (it was 49–64 fps, now expected ~72).
   - `work/steam-rootless` (`fe4f65cf`+): machine-id and /etc files in Steam's container. This is the fix for Torchlight II's D-Bus abort. Install with the bundle's `install.py` (rollback: `install.py --rollback`), then retry Torchlight II.
   - `work/xwayland-hidpi`: first the Stardew X11 geometry and input offset (synthetic ConfigureNotify, borderless/fullscreen hints, pointer translation), then HiDPI (Xft.dpi/GDK/Qt/Steam scale hints by default, per-app opt-in compositor 2× upscale).
   - `work/storage-io`: AHCI interrupt-driven completion (the guard/panic paths keep polling), the file-mapping lock hot spot, and the taskbar R/W graph (it showed nothing while ~14.5 MB/s was being written).
   - `work/sched-convoy`: scheduler-lock hold/contention stats and fixes. Behind the QEMU freezes under host load and the Dell's `SchedLockRelease` profile skid.
   - `work/compositor-refactor` (after shell-opaque): rebase, idle-host soak (2/4 candidate boots froze under load before), then stages 0.3/0.4.

**Next steps after that (not started):**
- **Matched demo to 60 displayed:** composite into the uncached zero-copy images costs ~12 ms at 2560×1440. Options: honour `xdg_toplevel.set_fullscreen`, then direct scanout of a fullscreen game (design in the occlusion report); or composite into cached memory; or hardware sprite planes for shell surfaces.
- **Compat batch 2:**
  - xattr syscalls should return EOPNOTSUPP, not ENOSYS (Python `shutil.copy2` breaks);
  - `strace exec NAME` (attach at execve) and lift `strace`'s pid < 256 limit;
  - dma-buf EXPORT_SYNC_FILE (VKRENDER hits ENOSYS per frame, 9,887 in one boot);
  - netlink uevent sockets, `/dev/input`, `/sys/class/net`, SIOCGIFHWADDR (Steam census);
  - `/proc/self/exe` status → EOPNOTSUPP (OpenAL);
  - `oom_recovery` thread using ~1.8% CPU with no memory pressure.
- **Launchers (Ian's approval needed for /nix/store):**
  - Quake II Apps entry, using the recipe in the gen 419 entry;
  - VSCodium (`--no-sandbox`, or a working Chromium namespace sandbox);
  - desktop PulseAudio session server with Dell HDA output.
- **32-bit (i386) support:** absent. Not prioritised (Ian); Steam's i386 helpers fail with 126.
- **Direction docs:**
  - `work/brook-value` (`1671e404`) holds Fable's "What is Brook for?" with Ian's decisions; merge it after Ian's review;
  - commit the untracked `docs/discrete-gpu-bringup-plan.md` from /data/Programming/Codex/Brook;
  - hardware target is the RTX 3080 desktop, booting from removable media with no writes to internal drives (DECISIONS row).

**16 Nov night (Dell gen 422, `dell-int20-20261116` = main + `work/occlusion` `811879bf`, session `6138c7fa7e564e65821833a64ed2fe19`):**
- **Occlusion and direct scaled sources (merged).** Matched demo, displayed frames (the goal metric):
  - gen 419: 162 in 24 s, mean interval 84 ms (~12 fps), swaps 52, composite 19.0 ms;
  - gen 422: 536, mean 30.1 ms / p90 44 / p99 50 (~33 fps), swaps 43.5, composite 10.5 ms.
  - With an idle Qalc behind: composite 10.9 ms and 42.6 swaps (no penalty).
  - waylandd `layered.scaled_single` counts every game frame; `offer_skipped.geometry` is 0.
  - Cause: c76814b7 (15 Nov) only offered exact-size buffers, so scaled games went through waylandd CPU composition and were never opaque. Separately, carried damage never shrank.
  - Evidence: `display-perf/build/dell/occl-{alone,idle}-{1,2}`.
  - Next for 60 fps: present blit 7.8 ms and submit→fence 29.7 ms on direct frames; honour fullscreen plus zero-copy/scanout (design note in the occlusion report).
- **Alt+Tab pointer re-engage:** confirmed by Ian, merged (`5f9d13cd`).
- **Network-dead-but-desktop-alive (16 Nov, Chromium video):** the lastlog record shows ARP replies from 192.168.85.8 arriving while "ARP resolve failed" loops; also DNS sockets full and dropping, the debug listener's accept queue full, and VKRENDER hitting dma-buf EXPORT_SYNC_FILE ENOSYS 9,887 times. Evidence: `display-perf/build/dell/hang-1116-lastlog/`. Fix in progress on `work/net-arp`.
- **Ready to integrate:**
  - `work/restart-harden` `4ace7567` (TERM/KILL escalation for windowless processes, blockers in status, tcp traces behind `log level tcp debug`);
  - `work/brook-value` `1671e404` (revised with Ian's direction).
- **In progress:** `work/clickpad-buttons` (hard right-click on the clickpad), `work/chrome-csd-style` (caption glyphs over Chromium's strip), `work/steam-rootless`.

**16 Nov late (paused at the usage limit). Dell on gen 421 `dell-int19-20261116` (Alt+Tab pointer change), session `feb450f6f3a648f0b935b97d0f6535ef`; Q2 running for Ian's test:**
- **Ready, not yet deployed:** `integration20/build/dell/pair-int20` = gen 421 + `work/occlusion` (`811879bf`), QEMU check PASS.
  - Fixes: scaled games are offered as a direct opaque layer (waylandd), and carried damage no longer grows (`DESKTOP_PIPELINE_WAIT_CARRY`).
  - Measure: matched demo alone vs with an idle Qalc behind, against gen 419 (steps in the occlusion agent's report and `docs/waylandd-composition.md`). This may also explain the 70→55 in-game drop, since c76814b7 stopped direct offers for scaled games.
- **Merge pending:** merge `work/integration19` to main once Ian confirms Alt+Tab, and `integration20` after measuring.
- **Stopped mid-way at the usage limit; resume from their worktrees:**
  - `work/restart-harden` (was about to build its Dell pair);
  - `work/steam-rootless` (had just started inspecting the guest).
- **`work/brook-value` (`9a342754`):** Fable's "What is Brook for?" critique, sent to Ian, not merged.

**16 Nov evening (Dell gen 420, `dell-int18-20261116`, session `6601d8cda6cd41d3b5e73b4900b22b45`):**
- **Pointer re-grab after Super is fixed, and Ian confirmed it on the Dell** (`work/pointer-relock` `aa42f690`, merged).
  - Revokes disengage the window. Only a click in the window's own content re-engages it, and acquire also requires the pointer to hover that content. The volume popup blocks a lock as the launcher does.
  - Root causes: the WM bumped the "physical click" serial for clicks anywhere (taskbar, launcher), and waylandd's entered-surface state went stale over the taskbar.
  - Ian's follow-up, in progress on the same branch: Alt+Tab back (a deliberate activation) re-engages without a click, as on Windows.
- **Gen 419 Steam and Stardew:** the GPU queue fix held (17,158 waits, 0 refused, Xwayland stayed up). Frame times were poor: the Xephyr detour runs Stardew on llvmpipe, then Xwayland glamor turns the image pushes into ~286 tiny GPU jobs/s (GPU 73% busy) and composites starve to ~18/s. `work/steam-rootless` moves Steam onto the desktop Xwayland with Iris GL.
- **A background window costs the game about a quarter of its frame rate:** an idle Qalc behind Q2 took composite from 9.1 to 15.9 ms and the game from 53 to 39 frames/s. `work/occlusion` is investigating; it also covers fullscreen direct scanout.
- **In flight:** `work/restart-harden` (TERM/KILL escalation for windowless processes, blocker names in status; bridge per-connection logging moved to a debug category) and `work/brook-value` (Fable's critique of brook-directions.md: Brook's value, Linux's unfixable DNA, and the do-over list).

**16 Nov afternoon (Dell gen 419, `dell-int17-20261116` = main + `work/prof-path` + `work/gpu-queue-wait`, session `26fb3e21dbc4414f8b3cde5e5022f349`):**
- **Quake II audio works (Ian confirmed). Launch recipe:**
  - `brook-run <brook-iris-run> <yquake2 quake2> -datadir /data/home/quake2 +set s_openal 1 +set al_driver libopenal.so.1`
  - env `LD_LIBRARY_PATH=/nix/store/.brook-audio-stage/59h20vp…-openal-soft-oss-brook-1.24.3/lib` and `ALSOFT_CONF` = the stage's `alsoft-oss.conf` (oss, 44100, stereo int16, periods 4 × 1024).
  - Without the conf, OSS `SNDCTL_DSP_SPEED` after SETFRAGMENT fails with EOPNOTSUPP (only the 48 kHz hardware rate is allowed once the ring is sized; deliberate since c86c7d43). Without the library, OpenAL is not found and SDL falls back to ALSA, which fails.
  - Ian wants a Quake II Apps launcher.
- **The matched demo regression (48–49 vs 52) was boot state, not code.** A fresh boot gives 52.2 (lastlog on) and 51.2 (off). On the post-Steam boot, bridge polling flooded serial (about 6 lines per bridge connection: DEBUG_TCP plus the tcp TX/RX traces), and the lastlog writer then rewrote and fsynced every 5 s, costing about 3–4 swaps/s. Backlog: make the per-connection bridge logging a debug category, off by default.
- **Profiler:** `prof start MS /nix/var/log/brook` saves PROF_*.TXT to the writable store.
- **Pointer re-grab after Super** (Q2 re-locks before taskbar clicks): `work/pointer-relock` in progress.

**16 Nov (Dell gen 417, `dell-int16-20261116`, session `f7b40b4bb49041aa9ea8301f6466b518`; main `d95e1b22`):**
- **Main now has:**
  - `work/bin-sh` (/bin/sh, /usr/bin/env);
  - `work/compat-batch1` (setitimer/alarm, kcmp, madvise DONTFORK/WIPEONFORK, sysfs cpu lists and /proc/<pid>/cgroup, TCP_INFO, quiet census set);
  - `work/pipe-poll-uaf` (the gen 415 panic was a memfd_create double free at EMFILE, an H-20 regression; also all-or-nothing pipe/socketpair publication, and live/dead markers on pipe poll nodes);
  - `work/steam-container` (/dev is a boot tmpfs with real char-device inodes, so bwrap `--dev-bind /dev /dev` works, nested too);
  - `work/chrome-csd` (Chromium tab strip plus Brook caption overlay sized to the 40-unit strip).
  - The new brook-chromium launcher is staged on the Dell (store `svypzdsv…-brook-hardware-network-launchers`; rollback `/nix/.brook-profile-rollback-csd1116a.json`).
- **Steam opens on the Dell, in Xephyr; Ian launched Stardew.**
  - The final blocker was libpulse autospawn: a fork/exec loop of a missing `libpulseaudio-17.0/.bin-unwrapped/pulseaudio` at ~25/s, ending in fd exhaustion.
  - Worked around by starting a private uid-1000 `pulseaudio -n` (native socket `/tmp/brook-pulse/native`, null sink) and launching `brook-steam` with `PULSE_SERVER=unix:/tmp/brook-pulse/native`. Script: scratchpad `steam_pulse.py`.
- **Next:**
  1. A proper desktop PulseAudio session server (Dell HDA output; Chromium also logs "PulseAudio is not available").
  2. Move the Dell Steam launcher to the rootless Xwayland path (as in the QEMU review, `docs/xwayland-window-integration.md`) now that the Dell has GPU Xwayland.
  3. `prof start [ms] [path]` with output to the writable store (/boot is full, Ian).
  4. VSCodium needs `--no-sandbox` (Electron SUID sandbox abort): launcher, argv.json, or a working Chromium namespace sandbox — Ian to choose.
- **Census on Steam:** netlink uevent sockets (EPROTONOSUPPORT ×555), /dev/input, /dev/bus/usb, /sys/kernel/tracing/trace_marker, /sys/class/net, SIOCGIFHWADDR ioctl.

**16 Nov early (Dell gen 415, `dell-int14-20261116` = main + `work/bin-sh`, session `a9eefa8322d248edb6e96789ba3785ba`):**
- **/bin/sh and /usr/bin/env now exist** (kernel tmpfs mounts). Steam's nested Xephyr starts, xkbcomp works, and the client starts inside pressure-vessel; processes reached 156.
- **Kernel panic ~90 s into Steam:** #PF CR2=0x21 in `PipePollGroup::~PipePollGroup` ← PollWait ← sys_ppoll. A pipe poll-waiter node was linked after being freed (use-after-free).
  - The persistent last log survived the panic, as designed. It shows Steam pid 195 stuck in "exhausted 1024 fds" with a fork/exec loop of a missing libpulseaudio `.bin-unwrapped/pulseaudio` (PulseAudio autospawn; see the "Steam startup controls" entry in DECISIONS).
  - Suspect: fd-allocation failure paths, possibly H-20's SCM_RIGHTS retain path. Being fixed on `work/pipe-poll-uaf`.
  - Evidence: `display-perf/build/dell/steam-int14-panic/`, `display-perf/build/dell/steam-int14-lastlog/`.
  - The guard recovered to the desktop by itself. **Don't launch Steam again until the fix lands.**
- **Next for Steam:** stop the PulseAudio autospawn loop (point PULSE_SERVER at the desktop server or set autospawn=no in the launcher). Then `work/steam-container` (/dev inside bwrap, /proc/self/fd/N/ traversal).

**15–16 Nov night (Dell gen 414, `dell-int13-20261115`, session `e13ab3d15f70488e927538eac8361068`):**
- **Main `2c53f154`** adds `work/open-file` (H-20: dup/fork/SCM_RIGHTS share offset and status flags, as on Linux), `work/tmpfs-link` (tmpfs hard links) and `work/compat-census`:
  - `compat census [N]` / `compat census reset` count failing syscalls, ioctls, options and missing /proc, /sys and /dev paths, per app;
  - `lastlog status|flush|previous` keeps a 64 KiB record (census top 40 plus serial tail) in `/nix/var/log/brook/lastlog{0,1}.bin`, so the previous boot's log survives a hang (`scripts/lastlog_fetch.py --hw SESSION --output DIR`).
- **Checks on gen 413/414:** Quake 50.3/52.4 swaps/s (unchanged); Chromium local video 29.5 fps, 19 drops; Firefox unchanged (~18 drops/s).
- **Per-app census** (`display-perf/build/dell/census-apps/*.json`):
  - setitimer (38) from Xwayland, 1000+ hits with VLC and LibreOffice;
  - kcmp (312) from Xwayland and Chromium;
  - madvise MADV_DONTFORK, 273 hits from VSCodium, which opened no window in 40 s;
  - TCP_INFO from Chromium;
  - /sys/devices/system/cpu/{possible,present};
  - /proc/self/cgroup and /sys/fs/cgroup;
  - dma-buf EXPORT_SYNC_FILE (0xc0086202), ENOSYS, from Chromium;
  - rseq noise.
  - `work/compat-batch1` is fixing the first six.
- **Steam:** past the lock now. The nested Xephyr dies at "XKB: Failed to compile keymap", because the FAT root has no `/bin/sh`, so popen and system fail for every program. `work/bin-sh` adds kernel-provided /bin/sh and /usr/bin/env. Next Steam blockers seen in the census: `/proc/self/fd/N/<subpath>` traversal (pressure-vessel, ENOENT ×72) and `/dev/null`, `/dev/urandom` and `/dev/tty` missing inside the srt-bwrap container.
- **Ready for Ian:**
  - `work/chrome-csd` (`d63b7a80`, Dell pair built): Chromium keeps its tab strip and Brook overlays min/max/close. It needs the new brook-chromium launcher (button-layout keyfile), which means a /nix/store write and Ian's approval. Also retire `work/chromium-frame`.
  - `work/compositor-refactor` (`4a4349f4`): the plan is in `docs/design/clearwater-next/compositor-refactor-plan.md`, with stage 0.1 done (single shell-surface list, pixel-identical in QEMU). It has five questions for Ian.
- **QEMU bridge under host load 40+:** several agents saw guest TCP SYN-ACKs go unanswered and vCPUs in `SchedLockWaitForTicket`, with one in virtio_net `SpinLockAcquire`. Evidence: `compositor-refactor/build/refactor-evidence/qemu-hang-virtio-net/`. Not investigated.

**15 Nov late (Dell gen 412, `dell-int11-20261115`, session `ea6c017f812d4390940513230c87c32c`):**
- **Deployed `work/integration11` (`2dbbf101`)** = main + `work/tap-click` (`a1855614`, Settings › Touchpad "Tap to click", default off; `input touchpad tap on|off`) + `work/bridge-indicator` (`e932018d`, taskbar agent icon; `bridge presence [beat N|detach]`; the MCP server heartbeats every 10 s, CLI calls never attach). Release build plus 359 host tests pass (three load timeouts passed alone). The robot icon shows on the Dell. Tap-to-click still needs Ian's hands-on check.
- **Clearwater Next approved by Ian** and merged (`4d467a91`): Glass / Opaque Light / Opaque Dark, 13 hues, side dock (rail and peek), launcher simplification.
- **Steam's current blocker:** the nested Xephyr dies with "Linking lock file (/tmp/.X14-lock) in place failed: Operation not supported" because tmpfs has no `link()`. `work/tmpfs-link` is implementing hard links. After that, expect the earlier hang once steamwebhelper starts (`docs/steam-dell-software-glx-checkpoint.md`).
- **Diagnostics:** `work/compat-census` (per-app census of unsupported syscalls/ioctls/options plus a previous-boot log tail that survives a hang) is in progress. It is the proposed top priority for running real software.

**15 Nov evening (Ian's review; Dell gen 411, session `f9d5a5d80c9240239a1f464cbdc8fd00`):**
- **Dell hang:** after Ian's Firefox crash the machine showed the Firefox crash reporter ("failed to generate crash information") but was off the network (no ping, no bridge). No panic record; Ian rebooted. A private-profile Firefox rerun did not crash (one content child exits on signal 9 every launch, and the crash-reporter background task exits 3). Not reproduced yet.
- **Chrome is GPU-accelerated:** the installed launcher uses ANGLE/GLES on Iris, Ian's profile has hardware acceleration on, and a private-profile local 720p30 H.264 loop played at 29.6 fps with rAF 61.6 Hz and 16 drops in total. App GPU 30 jobs/s (22% busy), compositor 40% plus present 24% of GPU time, CPU 85% idle. Video decode is software by design. Evidence: `display-perf/build/dell/chrome-rate-local/`.
- **Firefox is the weak one:** on the same clip it reports 30 decoded/s but ~18 dropped/s (`build/dell/ff-probe2/`).
- **Chrome's default frame:** with "Use system title bar" off, Chromium already draws its tabs at the top with no Brook bar, but shows only a Close button. Ian rejected the system-title-bar look (`work/chromium-frame` is not wanted); `work/chrome-csd` is getting min/max/close (Brook-styled if possible) into the CSD mode.
- **In flight:** `work/ui-drawer` (Glass/Opaque themes, rail-and-peek, launcher), `work/bridge-indicator` (taskbar agent icon), `work/tap-click`, `work/open-file` (H-20 shared open-file description, Ian: match Linux), `work/chrome-csd`.

**15 Nov, gen 411 (prompt GPU wakeups on by default, autonomous):**
- **Main is `d84d2c2f`.** M4a prompt wakeups now default to on during Dell boot; `/etc/dell-gpu-no-prompt-wake` opts out, and `gpu wakeups on|off` still switches at runtime.
- **Why:** after the syscall fix pass, sync_file polls wake on GPU retire, so M4a now pays off. Gen 410 A/B, 3 alternating rounds with zero-copy armed:
  - swaps/s: 56.5–59.0 on, 47.6–50.1 off;
  - demo wall time: 20.3–21.3 s on, 22.8–23.8 s off.
  - Evidence: `display-perf/build/dell/wake-ab-*`.
- **Soak (gen 410, wakeups on):** 8 Quake rounds with Chromium playing 4K YouTube throughout. No refusals, no new panic, `if_restores` 0, swaps 47–53/s. Rounds 0–2 showed a displayed p90 of ~1 s while the video played; this contention needs a look.
- **Dell: gen 411** `dell-prompt-wake-20261115`, session `814f822e2d5a4021b0438ae42a997ef2`. Wakeups are enabled at boot; Quake (zero-copy unarmed) gives 51.0–52.6 swaps/s and a composite of 18.8–19.3 ms.

**15 Nov, gen 410 (syscall fixes on main, autonomous):**
- **Main is `c23acf3c`:** it now includes `work/syscall-fixes`. Release build passes; host tests 357/357, with `test_sched_lock_owner` timing out once under -j8. That test is untouched by the fixes and never hung in 360 runs under heavy parallel load; treat it like `test_audio_delivery`.
- **Dell: gen 410** `dell-syscall-fixes-20261115`, session `4f0e6eebea1c4e58af1ba91ffa1b7790`, saved in the scratchpad `session` file.
- **Smoke tests:**
  - Quake: GPU wakeups off gave 43.2 swaps/s (composite 20.4 ms); `gpu wakeups on` gave 51.2 swaps/s (composite 19.4 ms). These are single runs and noisy; repeat them before changing the default.
  - Firefox and Chromium on YouTube with private profiles stayed up with no refusals. The Chromium job count varies with page state: 199 on the first run, 2706 on the second.
  - Kernel log is clean; the only panic record is the old seq 41.
  - Evidence: `display-perf/build/dell/fixes-*`.
- **Still to do:**
  - Run an A/B of `gpu wakeups on` as the default.
  - Decide H-03/H-21 authorisation (Ian).
  - Schedule H-20 and H-22.
  - Move the remaining direct user dereferences (VfsRead/VfsWrite on regular files, iovecs, sockopts, WM get_* and GPU submit) to `Copy*User`.

**15 Nov, afternoon (syscall fix pass, merged above):**
- **Branch head `ea753d08` plus a docs commit.** It has 11 fix commits on top of `7f5638aa`. The index and fix-pass summary are in [REFACTOR_ISSUES.md](REFACTOR_ISSUES.md) ("Fix pass status"); each issue's **Status** line is in its chunk file under [refactor-issues/](refactor-issues/) (open) or in [refactor-issues/done.md](refactor-issues/done.md) (fixed).
- **Fixed:** 23 of the 27 high entries:
  - raw user pointers: input/WM, getdents64, open, inet sockets, the keyboard read at count 0;
  - abstract AF_UNIX names;
  - `CLONE_SETTLS` validation, plus the context switch now always writing FS;
  - the vfork lost wakeup, and `CLONE_CHILD_SETTID`;
  - read on O_WRONLY;
  - no user copies under pipe, memfd or serial spinlocks;
  - the /dev/tty pins;
  - about 90 validate-then-dereference sites;
  - sync-file, pipe POLLOUT and eventfd wakeups.
- **Partly fixed:** H-04 is hardened only. H-03 and H-21 are authorisation policy for Ian; the proposal is in the log. H-20 (shared open-file description) and H-22 (GPU_ATTACH_WINDOW lifetime) are deferred, with designs.
- **New host test** `test_syscall_user_memory` compiles the real `syscall_*.cpp` handlers against a fake user address space, with a raced-unmap mode. The Release build passes, and so do 357/357 host tests (`test_audio_delivery` and `test_sched_lock_owner` can time out under -j8; rerun alone).
- **Dell pair:** `syscall-fixes/build/dell/pair-syscall-fixes` (payload `dell-syscall-fixes-20261115`, commit `ea753d08`, flip, persistent map, zero-copy and video engine ON). The QEMU guarded repair-to-desktop check passed on the first attempt. It has not been deployed to the Dell.
- **60 fps relevance:**
  - poll/epoll on a pending Intel sync_file now wakes on the GPU Retire event when `gpu wakeups on` is set. Otherwise it polls every 1 ms instead of every 50 or 100 ms.
  - Several eventfd waiters all wake.
  - Measure on the Dell before relying on either.

**15 Nov, ~07:00 UTC (gen 409, autonomous):**
- **Main is `f4b508f8`:** the syscall.cpp split, fast-forwarded on top of `cc07051a`. It is 36 `syscall_*.cpp` files plus headers; the map and the old→new line table are in [syscall-split-plan.md](syscall-split-plan.md).
  - The agent checked symbols, the 269-entry dispatch table, every commit's build, and 356/356 host tests.
  - An independent diff check found every removed line re-added except the 8 definitions whose default arguments moved to headers. The headers add no logic.
- **Dell: gen 409** `dell-syscall-split-20261115b`, session `051819644a284c3e857af9c6b729d923`, saved in the scratchpad `session` file.
  - Smoke test: the Quake benchmark ran (swaps 46/s, composite 20.1 ms, zero-copy off).
  - Firefox and Chromium on YouTube with private profiles both stayed up (147 and 587 GPU jobs in 90 s, no refusals), and no new panic was recorded.
  - Evidence: `display-perf/build/dell/split-quake2/` and `split-browsers/`.
- **Next for the refactor:**
  - The fix pass is on `work/syscall-fixes` (see the afternoon entry above). It needs a Dell smoke test and a merge.
  - Then the `net.cpp` debug-command split.
  - Rebase any in-flight kernel branches using the line table.

**15 Nov, midday (gens 407–408, autonomous):**
- **Main was `255d4b15`.** It adds `work/multilayer-main` (opaque regions clamped to the surface, per-window region state, the SSD override opt-in via `BROOK_WAYLAND_SSD_APPS`) and `work/integration10-lane`: the VA-API 4 asynchronous VCS lane (`gpu video tune async on|off`, **default off**), lane latency counters and `gpu video tune spin on|off`. With async off, the only paths that change are refactored, not altered: the sync VCS job is split into submit/conclude, and the worker's parked-job checks are no-ops. Release build plus 356/356 host tests pass (`test_audio_delivery` rerun alone).
- **Dell: gen 408** `pair-int10-lane2` (= main code), video gate off, session in the shared session file.
- **Lane results** (details in `docs/bdw-video-engine.md`):
  - ffmpeg decode with async on is bit-exact, 300/300 at 720p and 1080p, with 0 lane failures. Worker blocked per job: 555 µs, against 1.9 ms (720p) and 3.8 ms (1080p) with sync.
  - Chromium 1080p, per 20 s: async drops 40–47 frames, sync 76–77, software 7. Async uses 4–5 points less CPU than software.
  - Async still drops because the breadcrumb is first seen ~8.7 ms after submit, though decode takes ~3.3 ms: the worker only polls between jobs.
  - The `--hang` probe with async on quarantines VCS at 2 s, and the desktop never stalls.
- **Decision:** async stays off by default and software decode stays the browser default. A lane is only useful once completion is polled inside the render engine's wait.
- **Panic (gen 408, seq 41)** during a 1080p async + `spin on` run: a Chromium render job (pid 542, 7314 pages) was submitted but never started (no CSB, `head=0`), the PAT restore did not complete, and the guard recovered. It is unknown whether a lane job was in flight. The previous boot ran 6754 lane jobs (3371 overlapping render work) with spin off, cleanly. Decoded record: `browser-vaapi/build/hardware/vcs-lane-int10c/`.
- **Next for VA-API:**
  1. Separate async-only and sync-only Chromium soaks.
  2. Log the lane state in the worker panic message.
  3. Poll the lane breadcrumb inside the render completion wait.
  4. Then Firefox VA-API, VP9 0.16× and the YouTube H.264 preference.

**15 Nov, morning (gen 403, autonomous):**
- **Main was `aac1d429`** = everything through `work/multilayer-int9`: multilayer v3 + the gen 402 freeze fix (offers superseded by CPU snapshots are retired; frame callbacks are released ≤100 ms after queueing whatever the offer state), touchpad, popup fix, worker recovery/containment, proc-cleanup, the Chrome warning policy and VA-API 4 tuning. Release build plus 355/356 host tests pass.
- **Dell: gen 403** `dell-multilayer-int9-20261115`.
  - Before this deploy it was unreachable after the usage-limit pause, with no new panic record (seq 40 is old); AC-cycled.
  - A private-profile Chrome probe showed offers 416/416 presented and no layered waits, but stalled on the YouTube consent page, so ad overlays are untested.
- **Chrome opaque region:** Chrome sends a single 1050×880 rect over an 832×642 logical surface; `opaque_single` stayed 0 (exact-match check). Being relaxed to "covers the surface" on `work/multilayer`.
- **Clearwater Next is final and on main** (`513a78d9`, squash-merged): glass chrome, Mixed default, a 13-choice accent row (incl. hot pink and phosphor), launcher organisation, the edge-to-edge caption hit spec, the theme architecture T0–T5 and the implementation order. Files: `docs/design/clearwater-next/`.
- **syscall.cpp split:** merged since (see above).
- **VA-API 4 measurements** and the async-lane decision are with the VA-API agent.
- **Worker site=2:** waiting for a recurrence; there are 3-line diagnostics now.

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
- **User-mode process dump (Ian, 19 Nov; Codex `work/proc-dump`):** `proc dump <pid> <path>` writes a gdb-readable ELF core (PRSTATUS per thread, NT_FILE, PT_LOAD), and `scripts/dell_proc_dump.py` fetches it and prints the gdb command. Follow-ons (Ian, 19 Nov):
  - an automatic core on crash signals: uid 1000 by default, `RLIMIT_CORE` honoured, written to `/data/home/.local/share/brook/cores/`, with a budget and rate limit and a `proc autodump` toggle;
  - a kernel hook for hang dumps, which waylandd will call when a window ignores xdg ping for 10 s. Motivation: the gen 442 Chromium resize freeze (all threads asleep; we could not see where they were blocked). The procfs `syscall`/`wchan` and `proc blocked` work is on `work/chromium-freeze` (Claude agent).
- **"True" fullscreen modes through the panel fitter (Ian, 18 Nov; investigation):** scan out a game's lower-resolution fullscreen buffer (e.g. 1080p or 1600×900) and let the display engine upscale it, instead of the compositor's 13.6 ms per-frame scale (Q2 1080p fullscreen: `display-perf/build/dell/q2-439-fs`, direct-scanout fallback `scaled`). Open questions:
  - restoring the panel mode when the program crashes, hangs or loses focus;
  - the cursor and overlays without a hardware cursor plane;
  - Alt-Tab;
  - the PIPESRC/PF_WIN_SZ change without a full modeset on BDW.

  This sits on the direct-scanout kernel work in `docs/app-driven-present.md` section 4.
- **Q2 above 1080p falls back to windowed:** yquake2 offers no 3200×1800 mode. Try `r_mode -2`, or `r_mode -1` with a custom size, to find the limit.
- **Deeper idle states (Ian, 18 Nov):** IdleLoop uses only `hlt`; the package sits in C2 (idle 56–58 °C at 3.3 W). Package C3+ would cut idle heat and battery use.
- **GPU clock 600 MHz (18 Nov):** sustainable in Stardew (69–75 °C, ~10 W, no trips over 60 s; 27–29 → 39.2 displayed fps). 700 MHz reached 98–99 °C with an 11 W CPU burst and tripped the 90 °C guard. Fans stay near silent, so the EC is not ramping much. Candidate default for AC.
- **CPU power while GPU-bound:** the cores draw 4.5–5 W at ~18% busy and 2.7 GHz turbo, sharing the package budget with the GPU.

- **Clearwater side drawer (Ian, 15 Nov):** an edge drawer for social and messaging apps, where any window can be stashed for quick access. Sketch in `docs/design/clearwater-next/DESIGN.md` § Ideas backlog.
- **Keyboard shortcuts (implemented on `work/shortcuts`, QEMU-verified, not yet on Dell):** Super tap toggles the launcher (Escape closes it), Super key-down revokes pointer capture, Alt+F4 sends the normal advisory close. State machine `src/kernel/src/global_shortcuts.h`, host test `test_global_shortcuts`; evidence `build/shortcuts-check/`. Wayland lock release on hardware with a current waylandd and yquake2 is still unobserved (the QEMU seed's waylandd predates pointer constraints).
- **Dell application audio for yquake2 (stale, superseded: Ian confirms Q2 audio has worked for most runs; launch without SDL_AUDIODRIVER=dummy):** the gen 363 boot has no `/dev/dsp`, and the OSS OpenAL store path (`59h20vp…-openal-soft-oss-brook-1.24.3`) is present but has no `lib/`. Restore the gated Dell HDA path (`/etc/dell-hda-audible`; see `docs/xps9343-bringup.md`) and stage OpenAL, then verify audibly with Ian. The SDL build has no OSS backend, so OpenAL is required. The `/data/home` yquake2 config also had a stale absolute `al_driver`.
- **Browser video and audio on the Dell (Ian, gen 363, 14 Nov):**
  - **Chromium YouTube:** audible, but music plays SLOW; frame-time p80 ~90 ms.
  - **Firefox YouTube:** video and audio both run FAST, with no obvious audible stutter; ~3–4 fps, frame-time p80 ~174 ms.
  - **Suspicion:** the playback clock or position reported to apps (GETOPTR/GETODELAY or the bridge) is wrong; see `docs/audio-clock.md`. Being investigated in `work/dell-audio`.
  - **Acceptance check to add:** fullscreen YouTube with no audio stutter, judged from renderer frame pacing plus all-zero packets in mixer output.
- **Volume persistence:** taskbar volume changes apply live but are only saved by Settings' Apply. Decide whether taskbar changes should persist automatically.
- **Loader F5 wait ignores input on the Dell:** works in QEMU/OVMF. Boot the timing loader (branch `work/boot-timing`), press keys during the wait and read `BOOT_TIMING_DETAIL wait_keys=`; zero suggests ConIn isn't connected on the fast-boot path. See `src/bootloader/src/DOCS.md`.

- **Harden graceful restart (Ian, 16 Nov; stuck "closing" several times).** On 16 Nov a deploy restart sat in "Closing apps - resolve save dialogs, or Cancel" indefinitely. The cause was windowless user processes started over the bridge (a private `pulseaudio -n`; Steam leftovers are also suspects), which have no window to answer a close. Needed:
  - SIGTERM then SIGKILL for windowless processes after a bounded grace period;
  - a status that names the processes still blocking;
  - prompt cancel;
  - `dell_deploy.py` reporting the blocker instead of "needs_inspection".
- **Automatic slot rollback in the guard:** after N consecutive desktop panics on a newly activated generation, select the previous verified slot instead of looping in recovery-only mode. Also expose a slot-select operation in the recovery-only bridge.
- **Split `src/kernel/src/syscall.cpp`:** done as a pure move and merged to main (`f4b508f8`, 15 Nov). It produced 36 single-responsibility `syscall_*.cpp` files plus internal headers; see [syscall-split-plan.md](syscall-split-plan.md) for the map and old→new line table. The 182 problems found along the way (27 high) are indexed in [REFACTOR_ISSUES.md](REFACTOR_ISSUES.md) and split into work chunks under [refactor-issues/](refactor-issues/), which drive the separate fix pass. Not done yet: moving the debug channel and commands out of `net.cpp`, which [refactor-issues/14-single-responsibility-refactors.md](refactor-issues/14-single-responsibility-refactors.md) describes.
- **Remove or declare the twelve orphaned `tools/netsurf-build/*` gitlinks** (they have no `.gitmodules` entries).
- **Transparent kernel rendering interfaces,** so console and shell output uses the GPU path.

Smooth Chromium/YouTube playback (about 90% frame drops at 720p60), a Dell Steam client launch with audio, the bridge-loss watchdog (design only), the Xwayland `setitimer` interface, and promotion of the patched Iris build to the default launcher.
