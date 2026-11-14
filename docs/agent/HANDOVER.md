# Brook handover

Current state and immediate goal. Keep this short and current: replace stale lines rather than appending. Put detailed run records in [AGENT_JOURNAL.md](AGENT_JOURNAL.md), the previous agent's ~1 MB append-only log (newest entries at the top). Decisions belong in [DECISIONS.md](DECISIONS.md), and working preferences in [WORKING_STYLE.md](WORKING_STYLE.md).

_Last updated: 14 November 2026, by a Claude Code session started after the Codex agent was retired._

## Repository

- Work from `main` (`dc12f39e` at the time of writing). On 13 November at about 23:31 UTC it absorbed both lines of the previous agent's work: the main checkout's long-uncommitted changes (`d6724e11`), and the private checkout `/tmp/brook-main-BrURO4` (`dcb99b97`, `bd0ae24b`).
- `work/steam-desktop-hardening` is superseded. A stash backup of its final worktree, `stash@{0}`, is identical in content to `d6724e11`.
- The detached worktrees `Brook-gpu-lock-control`, `Brook-pre-timing-7d005ae0` and `Brook-timing-bisect`, and `/tmp/brook-main-BrURO4`, are historical. Their contents are in main. Do not develop in them.
- The `build/` tree holds hundreds of GB, mostly disk images. Never copy it wholesale.

## Immediate goal: 60 fps displayed on the Dell

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

### In progress (14 November)

- Two independent code reviews, of the compositor/presentation path and of the Intel submission/memory path. Each will also request targeted Dell measurements.
- The reviews and measurements then go to a Fable reviewer for a combined verdict, and from that to a strategy for 60+ fps.
- No code changes until that strategy is agreed with Ian.
- **Measurement rule:** count distinct displayed frames (direct commits), not only completed swaps.

## Dell state (14 November)

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

Smooth Chromium/YouTube playback (about 90% frame drops at 720p60), a Dell Steam client launch with audio, the bridge-loss watchdog (design only), the Xwayland `setitimer` interface, and promotion of the patched Iris build to the default launcher.
