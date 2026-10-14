# Recovery and VM-control backlog

Updated 14 October 2026. The next focus is the VM-control MCP; branch recovery
investigations below are deferred. This is a local project backlog, not a set
of externally created issues. See [branch-recovery.md](branch-recovery.md) for
the surviving commits and [vm-control-mcp.md](vm-control-mcp.md) for research.

## VM control — first implementation and remaining work

The first implementation is in [tools/vm_bridge](../tools/vm_bridge/README.md).
VM-01 has live user/agent attachment and identity checks; VM-02 has accelerated
readback/crop/scale; VM-03 has cancellation and coordinate tests; VM-05 has
framed requests, launch PID and OS stats; VM-07 is registered through stdio;
VM-08/09 have verified QEMU post-panic screenshots/registers/bounded RAM.
VM-04 has silent WAV capture and waveform plots. The rows below retain the
full acceptance targets: D-Bus/rings/timing, exact guest input probes,
completion/stdout, correlated jobs, fresh-client tool discovery, kernel panic
pipe and full dump manifests are still outstanding.

| ID | Work | Acceptance evidence |
| --- | --- | --- |
| VM-01 | Resident local MCP with launcher-published discovery, private QMP socket, lifecycle and incremental serial/host logs | Attach to both user-started and agent-started feature-enabled VMs without changing their disks/display/audio; reconnect without restart; reject stale identities; leave another VM untouched. |
| VM-02 | Full-resolution accelerated frame capture, crop/scale metadata and frame IDs | Capture Intel virgl gameplay and console at 1280×720 without host screenshots; click a known guest target accurately after scaling/cropping. Test D-Bus display/DMA-BUF first. |
| VM-03 | Timed input batches, keys/chords, button holds, drag/scroll, relative movement and text | Input probe records the exact sequence; modifiers/buttons release on cancellation; unsupported text fails before partial typing. Advertise relative-input limitations honestly. |
| VM-04 | PCM ring capture and audio timing plots with host playback disabled | Export a bounded PCM/WAV segment plus waveform, sample timeline and format metadata. Inject a known gap, identify it visually, and distinguish missing guest output from delayed host callbacks using guest telemetry. |
| VM-05 | Adapt existing kernel TCP debug commands, then add a framed Brook diagnostics channel | Fix/request-test framing and reply routing; observe actual windows/focus/processes and capture command stdout/stderr/status. Do not expose ordinary TCP commands as panic-safe. |
| VM-06 | Correlated observations, event waits, jobs, cancellation and human control lease | One observation combines frame ID/time, log offsets and audio interval; long jobs return promptly; takeover blocks input without stopping observation. |
| VM-07 | Package the verified server as a VM-only local Codex plugin | Tools discover and harmless diagnostics work in a fresh client session. No existing desktop-control plugin, host display, accessibility bus or global input daemon is used. |
| VM-08 | Post-panic inspection and recovery of the remembered kernel command pipe | Trigger a controlled panic on a disposable clone; MCP remains attached; preserve original crash context; read known physical/virtual RAM and reject bad addresses without reset. Verify each operation independently; panic mode excludes normal guest execution/input. |
| VM-09 | Selected-region dumps, later budgeted full dumps | Export bounded regions with address-space/context/build/hash manifest and explicit unreadable holes; verify content against known guest data. Full-dump support is deferred and not currently claimed. |

## Branch recovery and runtime investigations — deferred

| ID | Investigation | Acceptance evidence |
| --- | --- | --- |
| REC-01 | Ext2 corruption and rename cleanup (`d9bbaf5`) | Reproduce directory/inode corruption on a clone; verify same-inode rename, destination replacement and open-file lifetime; stopped image passes `e2fsck`. Do not assume the old rename patch fixes all corruption. |
| REC-02 | Wayland keyboard focus/modifiers and SDL text input (`92bb917`, `0076f9d`, `c455b28`) | Guest input probes and Quake console receive expected characters/chords, including layout/modifier transitions. |
| REC-03 | Fullscreen/resize/opacity (`34dee1a`, `2791c87`) and remaining GL3 UI/text corruption | Actual drawable/window geometry agrees; 720p menu/console transitions render correctly across repeated runs, with screenshots and GPU logs retained. |
| REC-04 | Crash capture, fault-safe unwinding and profiler recovery | A controlled guest fault yields readable multi-page diagnostics; known module/user addresses resolve correctly; forced disk-full profile writes report failure. |
| REC-05 | mmap read-ahead (`13783b3`) | Compare page faults/load time on the same map and seeds; verify mapping boundaries, unmap behavior and content integrity. |
| REC-06 | Nix closure verification/repair and transitive library linking (`211dd34`, `fe75fe2`, `820b42a`) | Deliberately missing/corrupt dependency is repaired; nested dependency loads; rebuild source rather than import generated installer executables. |
| REC-07 | Diagnostic probes/API-gap harness and scheduler/AP-startup experiments | Recover reusable probes separately. Scheduler WIP (`6a1d563`) and AP stack changes (`8c75b03`) require baseline comparison and dedicated SMP stress verification. |
| REC-08 | Old SMBus and visual/window experiments | Review only when hardware support or visual polish becomes the focus; compare later implementations before resurrecting old patches. |
| RUN-01 | LibreOffice wrapper/DBus bootstrap | Real wrapper creates its runtime directory and opens Writer; investigate `/tmp`/mkdir behavior without bypassing the wrapper. |
| RUN-02 | Player source/window geometry and audio stutter | Preserve 720p aspect ratio and verify playback under frame load; use VM-04 captures and guest timing counters to locate periodic stutter. |
| RUN-03 | NVIDIA backend comparison and later Venus work | Compare the same controlled scenario after Intel behavior is understood; explicitly confirm hardware renderer and selected node. Venus follows GL correctness/control work. |
| RUN-04 | Quake relative mouse input and capture (after package-install fixes) | Capture the pointer while playing in the native SDL VM window, deliver continuous relative motion without desktop-edge clipping, and release capture on an explicit escape action, focus loss or app exit. Verify aiming and menus with the user; inspect guest Wayland relative-pointer/pointer-lock support and the QEMU input path. |

RUN-04 recovery check: all surviving refs were searched by commit message and
code history for relative-pointer, pointer-constraints and SDL relative-mouse
entry points. `5e392af` (Wayland input grabber/routing) is already an ancestor of
main and does not implement pointer locking. No surviving Wayland protocol
implementation for capture was found; the earlier experiment notes also list
relative-pointer support as absent. The Quake 1 port's SDL call is a client of
capture, rather than a compositor implementation to recover.

REC-01 overnight check: stable inode reporting (`8592ff2`), trailing-slash
resolution (`b5cb813`) and the sleeping-lock ownership/spurious-wakeup fixes
are already merged. The rename destination-inode cleanup (`d9bbaf5`) remains
unmerged and the corresponding orphaning code is still present. That patch
needs same-inode/hardlink no-op, destination type/emptiness and open-file
lifetime validation before adoption. The package-install reproduction also
exposed separate ext2 sector-accounting and stale superblock-total defects;
fixing those is not evidence that the rename problem is resolved.

### Bridge validation follow-up

The 14 October GL3 run later recorded a user page fault and the guest TCP
channel stopped replying; QMP remained responsive for registers, bounded RAM
and explicit reset/stop. Reset changed the bridge generation, but guest requests
did not recover within 40 seconds; the disposable VM was then stopped.
Preserve this under REC-03/REC-04 rather than treating
successful bridge screenshots as proof of workload stability. Evidence remains
in ignored `build/vm-bridge/030fc50fc5614fd28cf9bad92665e8f3/` and
`build/experiments/results/mcp-final-validation.json`. CPU-rendered controlled
panic capture was separately verified on session
`e095f8182f684792aaf815cc82cc8a54`.

### Native-window follow-up

14 October: corrected chrome/input ordering and added GPU completion fences;
restored first-run WASD rather than Yamagi ESDF; fixed shell argv truncation
and initial exec-stack overflow; restored truthful OSS short writes. Installed
OpenAL/controls fixtures avoid disappearing runtime-created config files.
The filesystem mkdir/write defect remains REC-01/RUN-01. RUN-02 still needs a
real SDL/PipeWire tempo comparison: silent dummy-SDL PCM timing is not proof
that the intermittent no-pitch-change symptom is gone. KVM pvclock is active;
no two-times clock was measured. REC-02 retains SDL3 text-input activation:
the recovered probe reproduces per-window text input remaining disabled while
physical key events are correct. REC-03 retains intermittent GL3 startup/user
faults and UI/text corruption; one new seed run faulted at RIP=0 during graphics
initialization and is preserved under `build/vm-bridge/651ab826f9c5497ba7e41c6eb9a69025/`.

### Overnight package/runtime follow-up

Ladybird installed and rendered HTTPS pages through guest `nix-install`.
The native VLC compatibility build installed through a loopback Nix cache and
played Big Buck Bunny at 720p with nonzero captured OSS audio on one vCPU.
Four-vCPU startup still intermittently fails `wl_shm.create_pool` descriptor
duplication (`7720b42496964487ac23f2225e44d2d0`), despite the separately proven
shared fd-lock and atomic fcntl-dup fixes. Keep SMP startup under REC-03;
one-vCPU success is not proof that concurrency is resolved. File-backed writable
MAP_SHARED, truthful O_TMPFILE support and native ALSA remain follow-ups;
the VLC build uses memfd and OSS explicitly. OSS3 pause drains queued audio.

Corrected absolute `clock_nanosleep` and monotonic futex deadline conversion
were required to unfreeze VLC's playback clock. That does not certify the
intermittent Quake SDL music-tempo symptom as fixed. An OSS delay probe showed
8192 queued bytes decreasing to 3412 after 750 ms; final drain/tail behavior and
periodic stutter still need deeper measurement. The native-player two-second
WAV capture contains nonzero PCM and no entirely silent waveform bins, but
silence bins alone cannot certify smooth audio.

The close-button test during concurrent Ladybird installation later triggered
a VLC worker user page fault at RIP `0x1e45703f`, CR2 `0x35000020`; guest TCP
stopped replying while QMP remained usable. Retain this as a shutdown/load
case under REC-03/REC-04 (`2ffc99f1d67543a3a4f3cec2b6b6ab9c`). The successful
playback capture does not certify clean shutdown or simultaneous workload
stability. The stopped installer image was preserved before host repair.

An alternative VLC launch with `--no-avcodec-dr` also faulted a Wayland-client
worker on a cold checkpoint boot (`ea0840b5a30047709804b82088d6900b`). That
setting was removed; the default helper retains the settings demonstrated in
the successful untraced dual-app run. Video viewport/cropping polish also
remains outstanding: a narrow noisy strip was visible at the bottom of one
playback capture. Keep the current player as an experimental baseline.

The shared memfd fork defect was subsequently reproduced and fixed: normal
and worker-originated fork writes now remain shared, and sustained playback
stayed correctly rendered across eight helper forks. This supersedes the
initial bottom-strip/full-frame corruption diagnosis. Window-close still
faulted a VLC worker after that fix (`2efcc298b41a4c4494a1367c2143c964`), so
shutdown and SMP startup remain open. The prepared launcher uses one vCPU.

### Browser performance follow-up

14 October measurements and artifacts are recorded in
[experiments.md](experiments.md#ladybird-performance-investigation-2026-10-14).
Unconditional RequestServer tracing was removed; ordinary filtered strace
remains available. The initial fixed local transfer improved from 5.824 to
1.720 seconds, while later reloads with another browser open still took
3.488–4.694 seconds. Keep these test conditions distinct.

| ID | Investigation | Acceptance evidence |
| --- | --- | --- |
| PERF-01 | Replace whole-heap first-fit scans with indexed free blocks; avoid redundant poison fills on already-free split/coalesced regions while retaining forensic/UAF diagnostics | Host samples and unprofiled local workload improve; allocation/free/realloc/coalescing/expansion integrity checks and concurrency validation pass; runtime opt-in poison/forensic checks remain functional after toggles. |
| PERF-02 | Reduce synchronous block-I/O and repeated browser cache metadata writes | Correlate write paths with caller stacks and request counts; compare cold startup and warm transfer separately after allocator work. |
| PERF-03 | Reduce guest profiler observer overhead and account for IRQ-masked critical sections | Bounded captures do not spend substantial recording time allocating/writing their own FAT output; compare host and guest samples; preserve long-capture durability and report dropped events. |
| PERF-04 | Diagnose invalid compositor interval maxima | Reproduce `max_ms=4294967295`, establish clock/concurrent-frame-update cause, and validate telemetry before using it for stall percentiles. |
| PERF-05 | Compare QMP keyboard/wheel delivery with guest debug injection and native input | Exact event-sequence probe, settled-page typing/scroll tests and load-time latency measurements; distinguish routing defects from allocator-induced starvation. |


PERF-01 update: at the user's request, heap poison is now a runtime flag off
by default. The 14 October matched loaded-browser median fell from 3.953 to
1.873 seconds; lighter transfers changed little. The off-to-on transition
reseeds free regions without changing live allocations, and the allocator
regression plus live heap checks passed. First-fit indexing remains open.

PERF-06 — Quake GL3 at 720p: reduce synchronous GPU control-queue round trips
and spin waits. Correctly resolved driver snapshots put about 55–57% in
SubmitCommand/notification during stationary gameplay. Measure command counts
and GPU completion timing, preserve fences/shared-buffer lifetime and test
720p rendering plus capture correctness. Startup's extra pak-miss serial
logging is now opt-in; its sampled serial work fell from 95/300 to 3/300.
The bounded follow-up measured a 32.2-second map-ready launch and repeated both
recorded demos at 720p. Fusing the fence into the final chunk did not improve
throughput and was archived/reverted. Moving-scene snapshots still put about
half the samples in queue submission/notification. Next work should measure
queue/fence timing and prototype asynchronous completion with proper buffer
ownership and a sleep-capable lock; do not yield while holding the current
spinlock. Keep guest audio MMIO overhead as a separate follow-up.

PERF-03 update: bridge profile export can lose the output marker once startup
logs exceed its 256 KiB tail. Add persisted output-path discovery. Kernel-only
symbolization also needs the run's archived module ELFs and load addresses;
otherwise GPU module RIPs appear as false large-offset LZ4 frames. The current
captures were recovered/remapped explicitly; integrate that into normal export.


PERF-06 measurement update: repeatable `brook-benchmark.cfg` and
`tools/profile/q2_demo_benchmark.py` now alternate the licensed demos and collect
per-recording results without sampling/capture overhead. APIC rate adjustments
occurred during GPU-heavy runs; corroborate guest FPS with host timing before
claiming small wins. Investigate failing Mesa shader-cache renames separately:
these occur during launch and may prevent useful warm-cache behaviour.

RUN follow-up: QMP `system_reset` did not restart a test VM launched with the
current `-no-reboot -no-shutdown` flags; the bridge should expose the resulting
state and validate reset/reboot semantics. Restarting the owned QEMU process
provided clean benchmark boots. Quake process teardown also emitted map-count
underflow/user faults during an overlapping-process experiment; keep those
reproductions separate from the successful single-process demo comparisons.
## Replace BusyBox command fallbacks

At the user's request, backlog removing the synthetic BusyBox applet paths and
shipping real Linux utilities instead. The bootstrap SDK already supplies
coreutils, GNU grep/sed and findutils. Define a minimal default closure with
working `/bin/sh` and `/usr/bin/env`, update fresh-image provisioning and PATH
handling, then remove the stat/exec fallback together. Check terminal scripts,
app launchers and recovery commands before retiring BusyBox from existing images.

Bootstrap metadata follow-ups: the chown family still selects the first ext2
mount and `fchown` is a stub; route ownership changes through VFS path/fd
callbacks. `mkdir` also ignores its requested mode. General truncate/ftruncate
support remains incomplete. Keep these separate from the validated file
creation, chmod and timestamp fixes.

Bootstrap Git retries also exposed ext2 rename-overwrite leaks: replacing
`.git/config` removes its old directory entry without releasing the destination
inode/blocks. A synced stopped disk then reports unattached old config inodes
in `e2fsck -fn` (sizes 36/53/67/92 bytes), despite correct allocation bitmaps.
Fix replacement accounting, same-inode rename and failure rollback, including
open-destination semantics; verify repeated replacements followed by offline
fsck. This is separate from the unsynced-stop corruption fixed in the bridge.

The readiness probe found that `UserBufferReadable` accepts mapped `PROT_NONE`
pages; enforce access permissions consistently in user-buffer validation and
add protected-page probes. `alarm` (syscall 38) also returns ENOSYS; the current
readiness probe uses a forked parent watchdog instead. Temporary pselect/ppoll
signal masks and TCP urgent-data readiness remain compatibility follow-ups.
