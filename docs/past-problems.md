# Past problems and their fixes

This is a short record of failures that were root-caused and fixed, or that
recur with a known workaround. It is meant for the moment a problem comes
back: read the symptom, check the cause first, then the fix. Commit hashes are
on `main` or the named work branch; "the Dell" is the XPS 13 9343 test laptop.

## Boot, storage and recovery

**A new image panics at boot and the Dell loops in recovery-only mode.**
- **Cause:** A change to GPU-path kernel code meant that `SchedulerYield()` inside a new spin slice could return with interrupts off, so the next `MayYield()` failed. The guard then retried the same slot, and recovery-only boots have no command jobs.
- **Fix:** `8fc4bb07` restores IF after the yield. Recovery was a USB maintenance boot plus `scripts/hardware_promote_slot.py` to promote the previous slot.
- **If it returns:** remember the QEMU check never runs Intel GPU code. Deploy kernel GPU changes only when someone can reach the machine, and keep the previous slot as rollback.

**A new boot loader would have broken EFI power-off on every old kernel.**
- **Cause:** the boot-timing loader bumped the handoff protocol to v6, but `efi_runtime_inventory.h` and `platform_power.cpp` accept only v5.
- **Fix:** `992e0879` keeps the handoff at v5 and announces timing through an `extensions` marker.
- **If it returns:** test every old/new loader and kernel pairing in QEMU before promoting a loader.

**The GPU clock is stuck at 300 MHz after a forced restart (Quake drops from ~65 to ~47 swaps/s).**
- **Cause:** the render-clock request needs AC Online, and the Dell ACPI monitor refused the MADT. Some MADT NMI/LINT bytes are uninitialised firmware memory and change between boots, so the pinned hash no longer matched.
- **Fix:** `b821c97c` hashes a normalised MADT (`AcpiDellNormalizeMadt`); `67a58f76` logs which admission check failed.
- **If it returns:** read the admission log line for the failing table and diff the dumped table against the pin.

**`e2fsck` reports leaked blocks after mixed small and large appends.**
- **Cause:** legacy indirect-pointer writes updated the pointer cache but left the full-block cache stale, so reclaim missed the appended blocks.
- **Fix:** `e9d5cd20` invalidates the full-block image before each legacy pointer write.
- **If it returns:** run the mixed append/unlink/truncate cases of the I/O benchmark (`scripts/run_io_benchmark.sh --cache-probe --lifetime-probe`) and an offline `e2fsck -fn`.

**Files opened for append overwrite each other (a Nix manifest lost rows, Firefox prefs lost their prefix).**
- **Cause:** O_APPEND was ignored for ordinary vnodes; each descriptor wrote at its own offset.
- **Fix:** `9e31fa6d` picks EOF under the inode gate for every write; unsupported backends return EOPNOTSUPP.
- **If it returns:** check that the write path for that filesystem selects EOF per write, not at open.

## Memory and processes

**Steam panics the kernel about 90 s in (#PF in `PipePollGroup::~PipePollGroup`).**
- **Cause:** a `memfd_create` double free when the fd table was full (EMFILE), a regression from the shared open-file description work Steam hit EMFILE through a PulseAudio autospawn loop.
- **Fix:** `77aee955`; pipe and socketpair creation now publish all-or-nothing.
- **If it returns:** fetch the previous boot's log with `scripts/lastlog_fetch.py` and look for fd exhaustion before the panic; audit new fd-allocation failure paths.

**A large GPU job (Chromium, YouTube) panics the GPU worker at "stage=prepare" or "publish".**
- **Cause:** the PPGTT table limits were too small (128 pages), and prepare or publish failure panicked instead of failing the job.
- **Fix:** `67a27e3d` raises the limit and refuses unmappable jobs with ENOSPC; `ae899bce` publishes tables in 64-page batches up to 1,024.
- **If it returns:** decode the panic with `tools/crash_decoder.py` and check the job's page count against the limits.

**Chromium hangs forever after its GPU process dies.**
- **Cause:** the dead process's fds were closed only at reap, so the browser never saw the channel close and blocked in a synchronous Mojo call. Wayland configures then went unacked.
- **Fix:** `c0df8bfc` closes a dead thread group's fds at last-task exit (SIGKILL and fault deaths included).
- **If it returns:** run `proc blocked <pid>` on the browser and check waylandd's configure sent/acked counts for the window.

**Memory use climbs to ~1 GB after a few app launches and never comes back.**
- **Cause:** not a leak. It is the executable page cache (shared ELF text, up to 512 MB), reclaimed under pressure.
- **Fix:** The kernel now reports it as `Cached` and its reclaimable part in `MemAvailable`; `mem breakdown` shows `classes.exec_cache`.
- **If it returns:** check `mem breakdown` and `mem exec-cache` before hunting a leak. A real waylandd heap growth was seen once and not reproduced.

## Scheduler and timing

**Browser audio drifts (Chromium slow, Firefox fast) and fps figures are ~10% too high under load.**
- **Cause:** on the Dell, CLOCK_MONOTONIC counted LAPIC ticks, which ran 11.5% slow under load.
- **Fix:** `6c1912c5` uses the invariant TSC as the elapsed clock on bare metal.
- **If it returns:** run `clock status` (expect `tsc`/`invariant-tsc`) and `bridge hardware clock-compare` under load.

**QEMU guests freeze or crawl when the host is busy; all vCPUs sit in `SchedLockWaitForTicket`.**
- **Cause:** FIFO ticket-lock waiters preempted by an overcommitted hypervisor (lock convoy), not long critical sections.
- **Fix:** `b09d19e5` uses unfair CAS claims when the hypervisor bit is set, lets idle CPUs skip the ready lock, and adds `sched lock-stats`.
- **If it returns:** run `sched lock-stats on` and read contended wait against hold time; check host load first.

**A finished GPU job is reset as a "hang", Mesa reports device lost and the game crashes.**
- **Cause:** the CPU-0 GPU poll was starved for over 2 s, so the deadline reset a job that had completed. The starvation came from 2–5 s ring-0 stalls in Chromium's QUIC network service (UDP dequeue without the socket lock).
- **Fix:** `5a5da780` rechecks the breadcrumb at the deadline (`late_completions`); `a02725a9` dequeues UDP under the socket lock.
- **If it returns:** read `gpu hang status` (`late_completions`, `longest yield`, `from_pid`). A job that never started (head = tail = 0, no CSB) is a different, still-open failure.

## Graphics, GPU and display

**Glass drops to the opaque look every ~10 s while scrolling.**
- **Cause:** the renderer's present lease lapsed during runs of refused (ESTALE) or direct-scanout presents, so the backend fell back and dropped the blur.
- **Fix:** `8b7aa052` keeps a reserving renderer's lease through refused presents; `44095dbe` keeps backend blur for the renderer's own present.
- **If it returns:** run `gpu glass status`; `backend_blur_changes` should stay at 1, with `lease_live_renewals` rising and `presents.lease_gaps` at 0.

**The taskbar flickers with zero-copy scanout.**
- **Cause:** first per-context ring histories, then render jobs writing zero-copy images through write-back memory (PAT slot 3 was never UC).
- **Fix:** `a60d679c` leases PPAT slot 3 UC for render jobs that write zero-copy images.
- **If it returns:** check `gpu zerocopy status` and the cache mode (`wt` is the default).

**The renderer restarts repeatedly during games (visible hitches).**
- **Cause:** two bugs: a client-buffer CPU-transition race, and hasvk adding every live VkDeviceMemory to each execbuffer, so cached ring imports referenced old outputs.
- **Fix:** `c975d4a7` and `b9d05868` (per-frame import of ring targets).
- **If it returns:** count renderer restarts in the desktop stats and look at the `QUAD_UNCERTAIN` reason.

**The screen flashes under heavy GPU load (SuperTuxKart plus several windows).**
- **Cause:** `image-not-reserved` sent the zero-copy ring back to GOP instead of applying back-pressure.
- **Fix:** `413523a6` keeps the zero-copy front during scanout back-pressure.
- **If it returns:** watch `gop_returns` in `gpu zerocopy status`; it should stay flat.

**A scaled or fullscreen game is slow and the composite cost is high.**
- **Cause:** `c76814b7` offered only exact-size buffers, so scaled games went through waylandd CPU composition; carried damage also never shrank.
- **Fix:** `811879bf` offers scaled game buffers as direct opaque layers and bounds carried damage.
- **If it returns:** check waylandd `layered.scaled_single` and `offer_skipped.geometry`.

**Xwayland dies when Steam opens a window.**
- **Cause:** Iris maps shared and scanout buffers with `I915_MMAP_OFFSET_WC`, which the kernel refused.
- **Fix:** `f4be9ee0` serves WC requests as write-back, like legacy GEM_MMAP.
- **If it returns:** look for a refused GEM_MMAP_OFFSET ioctl in `compat census`.

**Chrome freezes (audio keeps playing) when an ad overlay appears.**
- **Cause:** a layered GPU offer superseded by a CPU snapshot was never PRESENTED, so frame callbacks were held.
- **Fix:** `bd271051` retires superseded offers; `e2747d64` never holds callbacks indefinitely.
- **If it returns:** check the waylandd surface entry for held callbacks and unpresented offers.

## Desktop, Wayland and input

**A GTK4 app (Nautilus) freezes when a tooltip appears.**
- **Cause:** waylandd sent `xdg_popup.configure` before `repositioned`. GTK4 reuses its tooltip popup through reposition and blocked in `gdk_wayland_surface_present_popup`.
- **Fix:** `6a4eaa00` sends `repositioned` first.
- **If it returns:** trace the popup's event order with `WAYLAND_DEBUG=1`.

**A stray X11 "X" cursor appears over Terminal or the taskbar while Steam is open.**
- **Cause:** Xwayland set its root cursor after POINTER_LEFT, with no pointer focus. The hovered window adopted it, or it was shown everywhere because owner 0 meant "everywhere". Regression from `a243a68c`.
- **Fix:** `8506a56b` ignores `set_cursor` without pointer focus and never shows an ownerless cursor image.
- **If it returns:** check which surface owns the cursor image and whether the client had pointer focus when it set it.

**Minimise and Maximise are greyed out in GTK4 apps.**
- **Cause:** waylandd advertised `xdg_wm_base` v3, so GTK4 never got `wm_capabilities` (added in v5).
- **Fix:** `c2079740` advertises v5 with Maximise, Minimise and Fullscreen capabilities and logical configure bounds.
- **If it returns:** check the bound `xdg_wm_base` version in the client's protocol log.

**Nautilus restores from maximise cropped or with doubled shadow margins.**
- **Cause:** the restore configure sent the physical buffer size as window geometry and flagged it as an interactive resize, so GTK added shadow margins again.
- **Fix:** `e9ee4efb` sends a 0×0 normal-state configure after leaving a managed state.
- **If it returns:** compare the configure size with the logical (not buffer) geometry at 200% scale.

**Settings crashes when choosing a custom colour.**
- **Cause:** the GTK3 ColorChooser GSettings schema was not installed.
- **Fix:** `505ab256` compiles the GTK3, GTK4 and GNOME schemas into the Brook profile; this needs the profile staged, not just an image.
- **If it returns:** check for the `org.gtk.Settings.ColorChooser` schema in the profile's compiled schema directory.

**Launcher scrolling stutters over an idle desktop.**
- **Cause:** the renderer applied its 250 ms overlay-only throttle even while the launcher was focused.
- **Fix:** `a9063ac4` lifts the throttle while `focus.shell` is set and scrolls by fine 1/120 ticks.
- **If it returns:** check whether shell focus is set and whether overlay-only frames are throttled.

**Launcher pins are never saved (`launcher.ini` never exists).**
- **Cause:** the launcher's legacy `VfsOpenMode` temporary-file create failed on the guarded home bind mount.
- **Fix:** `c7c82b31` uses retained VFS paths, creates missing config directories and retries failed writes.
- **If it returns:** run `launcher status` for the save stage and errno; failures also reach the kernel console log.

**The taskbar and launcher are missing (blank desktop).**
- **Cause:** the image was built without `--clearwater-shell`, so the user-mode shell and its supervisor were not included.
- **Fix:** always pass `--clearwater-shell` to `scripts/build_dell_image.py`.
- **If it returns:** check the image for `CWSHELL.ELF` and the build command line.

**A waylandd fix staged into the store has no effect on the Dell.**
- **Cause:** the Dell's Wayland server is `/boot/INTLWL.ELF`, built into the image, not `/nix/profile/bin/waylandd`.
- **Fix:** ship waylandd changes with an image deploy.
- **If it returns:** check which binary the running waylandd process was started from.

**A game regrabs the pointer after Super and taskbar clicks.**
- **Cause:** the WM bumped the "physical click" serial for clicks anywhere, and waylandd's entered-surface state went stale over the taskbar.
- **Fix:** `aa42f690` re-engages only on a click in the game's content; `69d17e6e` adds Alt+Tab and taskbar activation.
- **If it returns:** run `test_pointer_capture` and check the engage serial.

**Chromium stops drawing after a resize, or acknowledges only its first configure.**
- **Cause:** layered windows kept a stale VFB group after a resize; earlier, configure serials came from two counters, so activation could reuse serial 1.
- **Fix:** `60254311` resizes the VFB and never leaves a stale group; `5802d238` uses one serial allocator.
- **If it returns:** compare configure sent/acked counts in the waylandd surface diagnostics.

## Audio

**Chromium audio flutters and plays slow on the Dell (~22% silence).**
- **Cause:** Chromium's 1,920-frame OSS ring could not cover the HDA queue's ~30 ms lead, so the queue padded silence and the stream drained at ~0.78×.
- **Fix:** `6dd7267e` enables the 4,096-frame downstream allowance by default and builds the browsers with the downstream-aware OSS plugin.
- **If it returns:** compare the stream's frames/s with 44.1k and check gap counts; `/etc/audio-staging-off` must not be present.

**Quake II has no sound.**
- **Cause:** without the OSS OpenAL library SDL falls back to ALSA, which fails; without `alsoft-oss.conf`, `SNDCTL_DSP_SPEED` fails because only 48 kHz is allowed once the ring is sized.
- **Fix:** workaround: launch with the OSS OpenAL `LD_LIBRARY_PATH` and `ALSOFT_CONF`; the Quake II launcher does this.
- **If it returns:** check `/dev/dsp` exists and the OpenAL store path has a `lib/`.

**All audio stops after one HDA stream fault until reboot.**
- **Cause:** after a queue fault the driver used to restart in a storm; it now quarantines HDA instead.
- **Fix:** `2810e966` records the fault and stops the storm; `/etc/dell-hda-fault-recovery` opts in to recovery.
- **If it returns:** read the HDA fault record, then reboot to restore audio.

## Networking

**The network dies while the desktop stays alive ("ARP resolve failed" loops).**
- **Cause:** ARP waited inside receive processing, and the table could refuse replies.
- **Fix:** `3b058a3e` never waits in receive processing and always accepts replies.
- **If it returns:** run `bridge net arp` and `bridge net stats`; pull the previous boot's lastlog.

**Steam hangs at shutdown with a helper stuck on a socket lock.**
- **Cause:** `SockClose()` cleared the whole socket, ticket counters included, while lookups could still use the slot.
- **Fix:** `7bf94486` fixes slot reuse across RX, close and deferred wakeups.
- **If it returns:** sample the stuck thread; a ticket lock that is "free" but past its waiter means slot reuse.

**Downloads are slow (tens of Mbps on a 900 Mbps line).**
- **Cause:** ECM receive depth 8 and a full TCP out-of-order store (`ooo_full`).
- **Fix:** `fe8c1c1e` deepens ECM receive and reassembles the full TCP window; `e913c29f` keeps ACKs moving.
- **If it returns:** check `rx_depth` (128), `ooo_full` and `tx_busy` in `bridge net stats`.

## Apps and packaging

**App changes do not appear after an image deploy.**
- **Cause:** store packages and `/nix/profile` are not updated by image deploys.
- **Fix:** stage the profile delta with `tools/launcher-stage/` (`install.py … --check`, then the real run; `--rollback TX` undoes it).
- **If it returns:** after any change under `tools/*-pkg`, `nix/*.nix` or `data/nix-wrappers`, diff the profile against the Dell's store.

**A staged launcher entry (Steam) vanishes after the next package install.**
- **Cause:** `tools/nix-install/nix-install.c` regenerates `applications.idx` only from profile `.desktop` files, dropping entries added by `install.py --apps`.
- **Fix:** workaround: restore the line from the `/nix/.brook-apps-rollback-*.idx` record.
- **If it returns:** check whether the entry has a `.desktop` file in the profile.

**Steam spins forking PulseAudio and runs out of fds.**
- **Cause:** libpulse autospawn of a missing binary at ~25 per second.
- **Fix:** workaround: start a private `pulseaudio -n` and set `PULSE_SERVER`; never raise fd limits first.
- **If it returns:** run `compat census` and look for repeated exec failures.

**Steam's nested X server fails ("Linking lock file … Operation not supported", XKB keymap errors).**
- **Cause:** tmpfs had no `link()`, and the FAT root had no `/bin/sh` for popen.
- **Fix:** `47212131` adds tmpfs hard links; `487237fb` provides `/bin/sh` and `/usr/bin/env`.
- **If it returns:** run `compat census` for ENOSYS and missing paths.

**Steam's CEF renderer hits a breakpoint in Library; Torchlight II aborts on D-Bus.**
- **Cause:** `prlimit64` shared the stack limit with data, CPU and file size, so Chromium saw an 8 MiB data limit; Steam's containers also lacked a machine ID.
- **Fix:** `bf6bf5eb` fixes the limit defaults; `fe4f65cf` gives the containers a machine ID and basic `/etc` files.
- **If it returns:** compare `getrlimit` results through both syscalls with Linux.

## Tooling

**An image was deployed although its QEMU check failed.**
- **Cause:** a manual deploy ignored `passed:false` (OVMF load stalls).
- **Fix:** `scripts/dell_deploy.py` refuses to deploy without a passing verdict; use it for every deploy.
- **If it returns:** rerun the QEMU check; OVMF stalls usually pass on retry.

**A deployed image is missing recent fixes.**
- **Cause:** a work branch was based on an old main, so the image built from it lacked newer fixes.
- **Fix:** merge work branches with current main before building an image.
- **If it returns:** compare the pair's commit with `main` before deploying.

**A deploy restart sits in "Closing apps" forever.**
- **Cause:** windowless processes started over the bridge, a supervisor started by hand, or a window that ignores close.
- **Fix:** `4ace7567` escalates windowless processes (TERM, then KILL) and names blockers; windows that ignore close still need a manual kill.
- **If it returns:** `hw_power cancel`, kill the named pid, then request the restart again.
