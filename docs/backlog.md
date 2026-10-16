# Recovery and VM-control backlog

## Workload findings, 16 October


### Native desktop user run


- Firefox's new-profile menu launch now opens with the compatibility UID and
  tested first-run/Wayland/ext2-temp setup. A fresh run rendered HTTPS and input,
  but normal window close hung until the browser's shutdown watchdog fired.
  Serial output includes `glean_core: Shutdown called before Glean is initialized.
  Waiting.` and `RunWatchdog`, followed by a user-mode intentional crash and a
  crash-handler fault. It eventually returned to ten tasks without kernel page
  ownership warnings. Keep this as failed normal-quit acceptance; capture the
  blocked shutdown phase and investigate Glean/profile initialization instead
  of disabling the watchdog or describing window disappearance as clean exit.
- Bridge captures occasionally show a partially painted launcher/window even
  when subsequent frames are complete. Verify capture reads a completed front
  frame or synchronizes publication before adding VM recording. Source audit
  confirms `CompositorDebugFrame` returns the CPU backbuffer and the bridge pauses
  QEMU later, allowing capture midway through the same mutable frame. Publish
  an immutable completed-frame snapshot through a compositor request, including
  GPU readback at the frame boundary; do not upscale
  or touch up incomplete frames as evidence of correct rendering.

- OpenTTD 15.3 hung during the user's confirmed in-game exit, with music
  continuing until SIGKILL. On the revised persistent-home/idle-DMA build, both
  menu exit and confirmed in-game exit with music enabled returned to ten tasks
  and inactive audio. Keep longer-session reruns and any recurrence capture open;
  the two changes have not been isolated as the cause of the original hang.
- Browser launcher entries must use the same tested compatibility configuration
  as command-driven acceptance. The user-run Chromium entry exited with a root
  sandbox error; Firefox faulted during generic startup instead of loading.
- Add launch feedback: an immediate pending taskbar item and animated busy cursor,
  correlated with process/application identity; resolve it on first mapped window,
  launch failure, process exit or a bounded timeout. Preserve normal cursor input
  and report failures visibly rather than leaving a permanent spinner.
- Add persisted desktop scale (start with 100/150/200 percent). Scale native
  taskbar, chrome, launcher, fonts, icons and hit targets together; coordinate
  Wayland output scale and buffer geometry for applications. Test at 1080p and
  4K, including multi-window input and settings preview.
- Identity/authorization audit: [current implementation and acceptance gates](identity.md).
  Persistent HOME/XDG now passes a fresh-volume reboot test; establish matching passwd identity,
  then audit access/open/stat/chmod/chown, supplementary groups, UID changes,
  fork/exec credentials, runtime directories and saved settings before implementing
  login/accounts. Several identity syscalls still stub success; a UID field alone
  must not be described as user isolation. Avoid migrating/deleting existing home
  files without preserving their data and ownership.

- O_APPEND on ext2: a shell `printf ... >> user.js` overwrote the first bytes
  instead of appending during Firefox audio setup. Preserve the corrupted
  profile as evidence and test append semantics through separate opens, dup and
  concurrent writers. Ext2's open callback currently ignores flags and VfsWrite
  does not receive the descriptor's append bit. Keep profile generation as one
  complete write during audio diagnosis; do not call the append behavior fixed.

- Evaluate LZ4-backed storage as a measured experiment, starting with immutable
  Nix-store content rather than changing writable ext2 semantics. Compare cold
  app loads, bytes read, CPU decompression/copy costs, peak memory and random-read
  latency against an uncompressed baseline, on both RAM-backed host disks and
  ordinary storage. Identify metadata/redundant-copy costs separately. Define
  independently decodable block indexing, corruption/error handling and cache
  ownership before broad deployment; writable compression needs separate
  allocation, crash-consistency and interoperability design.
- Plan pageable-memory reclaim: existing VM/page-fault infrastructure needs an
  eviction policy and backing ownership before swap. Investigate compressed
  anonymous-page storage first, then disk-backed swap with compression as useful.
  Cover pinned/device/shared pages, fork/CoW, concurrent faults, process exit,
  bounded compression memory and allocation-free low-memory I/O paths. Measure
  reclaim CPU cost and application latency; avoid treating larger memory limits
  as a substitute for a pressure/reclaim policy.

- Extend shutdown beyond connected Unix sockets: Internet sockets and unbound/
  listening Unix sockets currently return EOPNOTSUPP. Connected Unix shutdown,
  EOF/readiness, blocked-operation wakeups and lifetime semantics now pass the
  Linux/guest regression. Chromium MSE and YouTube startup, long pause/resume,
  seeking and audio pass with normal autoplay rules. Keep longer sessions,
  hardware rendering and sandbox support as separate acceptance work.
- Firefox audio remains open: default audio IPC fails cubeb initialization.
  An isolated profile selecting ALSA and disabling content sandbox/audio IPC
  produces nonzero PCM, but has severe silence and lost-frame warnings. The
  current file-over-null ALSA route reports zero delay, so investigate actual
  device timing as well as the separate IPC failure. This is diagnostic evidence,
  not accepted playback or a shipped configuration. Repeated normal browser
  quits also left child tasks behind; preserve that cleanup failure and the
  recovered virtio-blk timeout from this run before reusing any candidate.
  Chromium's previously accepted ALSA-to-OSS output works after pause/resume.
- Fix kernel-symbol table generation on incremental/fresh builds: the current
  post-link generator writes addresses for the next link. Chromium's preserved
  panic labeled RIP `0xffffffff800af838` as KRwLockReadLock, while the exact
  frozen ELF proves KMutexUnlock's owner assertion. Use that ELF for attribution
  until the table is generated and validated against the final linked layout.
- Complete Unix socket contracts beyond automatic sender credentials: connected
  SOCK_SEQPACKET currently uses stream storage without full record boundaries;
  explicit SCM_CREDENTIALS and passing pipe/eventfd descriptors remain unsupported.
  Keep these distinct from the now-tested automatic SO_PASSCRED path. Replace
  remaining success-only setsockopt stubs with implemented semantics or errors.
- Extend MADV_DONTNEED beyond private anonymous/heap/stack ranges, and reclaim
  resident anonymous memory lazily instead of replacing populated frames eagerly.
  File/shared/device discard must remain unsupported until backing invalidation
  and fault repopulation are coordinated. Other content-changing/inheritance
  advice must not return false success.
- Chromium now passes the basic software-rendered local/HTTPS workflow; next
  investigate missing/delayed GitHub stylesheets and images (Google and Wikipedia
  render), client-side maximize placement extending beyond the display, longer
  sessions, hardware rendering and sandbox support. Capture actual per-resource
  network failures before attributing them to DNS, TLS or HTTP. Keep the existing
  regular-file writable MAP_SHARED guard until its full acceptance is complete.
- Chromium YouTube/DevTools on the 4-GiB candidate exhausted memory while loading
  another 477-MiB helper ELF (`ELF: out of memory at page 47517`). Subsequent user
  faults reached an unchecked signal-frame `memset` in HandleExceptionFull and
  panicked the kernel. Make exec failure transactional and signal-frame writes
  safe for unmapped/read-only/CoW/OOM alternate stacks; exercise both synchronous
  and syscall-return delivery. The failed disks are preserved and not reused.
  This invalidates full stability acceptance for that candidate, despite the
  preceding clean Chromium and Firefox workflow/quit results.
- Panic observability: simultaneous raw user-fault dumps interleaved with the
  kernel fault text, so the bridge's serial regex missed the panic and observation
  attempted a dead guest TCP query. Preserve structured panic state independently
  of serial text and serialize recoverable crash dumps without forcing UART
  ownership. Raw QMP register/memory reads still worked for this failure.
- Firefox rerun on `4e53a2a`: local page JavaScript executes (confirmed by its
  dynamically set Wayland title), but visible content stays blank and address-bar
  typing does not appear. Investigate display/update and input responsiveness
  from the saved profile rather than assuming all content processes fail.
- Firefox teardown ownership correction: a small concurrent SCM_RIGHTS test
  reproduced duplicate descriptor installation (577 from 512 sends) and the
  mapped memfd free. Coupling rights and bytes under the pipe lock passes the
  exact-count regression and two Firefox teardown cycles without warnings.
  See the latest Unix ownership section in `workloads.md`; browser display and
  interactive acceptance are still open.
- Audit memfd shrinking independently: `ftruncate` currently frees tail backing
  pages without invalidating live borrowed mappings. This is a separate source
  finding; the reproduced Firefox teardown free came from descriptor close.
- Codium follow-up (17 October): removing `--disable-dev-shm-usage` gets past
  its field-trial allocation breakpoint using the supported `/dev/shm` backend.
  It still starts helpers without an editor window; a ten-second profile is
  about 99% idle. Diagnose the startup/IPC wait next. Both control boots passed
  cleanup, heap and offline disk checks; no regular-file MAP_SHARED enabled.
- Codium's earlier clean-boot rerun fails its 256-KiB writable file MAP_SHARED
  request with EOPNOTSUPP and reaches the same startup INT3. Complete coherent
  shared writes/writeback; rerunning without that feature did not yield an editor.
- The boot filesystem's `/tmp` cannot unlink an open temporary file (EBUSY),
  which breaks stock Bash here-documents; fchmod also returns ENOTSUP there.
  The shell acceptance uses a data-backed TMPDIR. Provide suitable temporary
  storage or complete open-unlink semantics before changing shell defaults.
- Generic `stat` currently estimates allocated blocks from logical size. It
  reports nonzero blocks for inline symlinks; expose filesystem allocation data
  before relying on this field for sparse files or disk-usage tooling.
- Native terminal rendering leaves zsh's prompt-end marker visible between
  commands. Verify cursor/erase behavior and terminal capabilities before making
  zsh/starship the default. Kitty, job control and broader interactive use remain
  separate acceptance work.
- Implement real `statfs`/`fstatfs` capacity and free-space reporting. The current
  all-zero success stub makes OpenTTD show “0 B free” despite successful saves.
- Investigate GTK Save As path-completion popups losing subsequent typed input.
  Typing `/data/home/gtk-text-input.txt` stops at a directory prefix; cancelling
  restores body typing, and a simple filename saves correctly. Reproduced with
  both the old compositor and text-input-v3 candidate on separate private VMs.
- Audit scheduler stall diagnostics before interpreting their “real strand”
  claim: scanning Ready state outside the ready-queue lock, retaining a raw
  process pointer, and comparing two samples can race with other CPUs and process
  lifetime. Messages occurred during zstd imports in both successful clean
  bootstrap and OpenTTD installation. Establish a coherent referenced snapshot
  and distinguish detector races from an actual persistent scheduling fault.

## Mounted filesystem module unload

Checked VFS unmount retains a mount when synchronization fails. Module unload
still calls a void module-exit callback and then frees the module's code; the
ext4 exit callback also bypasses VfsUnmount. Hot-unloading a mounted filesystem
therefore cannot honor failed-unmount refusal or protect active mount callbacks.
Keep mounted filesystem modules loaded. A future fix needs checked unload and
mount/module lifetime ownership; normal bridge sync and VFS unmount do not
establish hot-unload safety.

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
| RUN-05 | Wayland window-close timeout targets compositor instead of client | With Writer and Quake open, an unresponsive Quake close must terminate only Quake; keep waylandd and Writer alive. Track the actual client owner separately from the process providing the shared VFB. |

Showcase session `4267280270f145d9b82dd4b6749207c5` reproduced RUN-05 on
15 October: the Quake window was registered with `pid=7`, the shared Wayland
server. A 300 ms mouse hold delivered its close request successfully;
`CLOSE_REQUESTED -> xdg_toplevel.send_close` was followed by the WM's SIGTERM
and force-kill timeout fallback. Both Writer and Quake windows were destroyed.
The bridge remained responsive; a subsequent `procs` listed no waylandd or
application processes, and `wm list` reported no windows. Serial evidence is
retained under that ignored session directory. This is a window/client
ownership failure, separate from the old short bridge mouse clicks that could
be missed between compositor polls. Do not claim general clean app shutdown
from a successful screenshot.

RUN-04 recovery check: all surviving refs were searched by commit message and
code history for relative-pointer, pointer-constraints and SDL relative-mouse
entry points. `5e392af` (Wayland input grabber/routing) is already an ancestor of
main and does not implement pointer locking. No surviving Wayland protocol
implementation for capture was found; the earlier experiment notes also list
relative-pointer support as absent. The Quake 1 port's SDL call is a client of
capture, rather than a compositor implementation to recover.

REC-01 lifetime and replacement accounting are now fixed and validated by all
24 guest probe checks, actual inode reuse and a synced offline-fsck-clean disk
with allocation counts restored. See [bootstrap evidence](bootstrap.md). Keep
partial-I/O rollback, crash consistency and concurrent mount teardown leases
as follow-ups rather than reviving the old patch wholesale.

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
| PERF-07 | Reduce shared-inode mapping-gate contention during compiler startup and user-buffer checks | Corrected outgoing-stack profiles identify waits in FileObjectGetPage and FileMappingGateSet. Isolate batched primary/cache-neighbor lookup from AS-only read checks; preserve canonical pins, mutation ordering and generation validation; compare matched rebuilds plus race/coherence/cancellation gates. |


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
callbacks. `mkdir` also ignores its requested mode. FAT and ext2 now implement
truncate/ftruncate with mapped-backing protection; ext4 resize and concurrent
mapping mutation still need work. Writable regular-file MAP_SHARED mappings are now
rejected with EOPNOTSUPP because their previous eager-copy path silently lost
output. Implement coherent shared file pages, dirty tracking/writeback and
the remaining mapping-lifetime guarantees before enabling them;
memfd/framebuffer sharing remains supported. Keep these separate from the validated file
creation, chmod and timestamp fixes.

Candidate CMake regeneration still logs failed CMakeCache.txt temporary-file
renames before completing through fallback. Reproduce this specific overwrite
case and inspect its error/fallback behavior; the general semantic probe does
not establish that every build-tool rename works. Evidence:
`build/workloads/mapping-transaction-20261015/guest-serial.log`.

Bootstrap rename-overwrite, open-unlinked lifetime and directory accounting
now pass the expanded semantic probe and synced offline fsck. Remaining work
is injected-I/O failure handling, rollback auditing and crash consistency;
normal synced shutdown is not a power-loss durability guarantee.

The readiness probe found that `UserBufferReadable` accepts mapped `PROT_NONE`
pages; enforce access permissions consistently in user-buffer validation and
add protected-page probes. Raw `alarm` (syscall 37) is implemented; musl's `alarm` wrapper
uses unimplemented `setitimer` (syscall 38). Keep a host or parent watchdog for
deadlocks that prevent fatal-signal completion. Temporary pselect/ppoll signal
mask regressions now pass; TCP urgent-data readiness remains a follow-up.

Bootstrap cache/lifetime follow-ups: ext2 mmap cache identities now include a
unique mount namespace, fixing collisions between equal inode numbers on
different disks. Writable ext2 regular files now bypass shared mmap caching
after a real Git update reproduced stale source bytes. Restore cache sharing
and read-ahead only with synchronized write/truncate/unlink/inode-reuse
invalidation; fresh mappings now pass append, pwrite, O_TRUNC and actual inode
reuse probes. Open directory paths still truncate at 62 bytes; add long-dirfd
regressions. Multi-mount mmap and unlink-while-open probes now pass. The phantom Git references were separately
traced to the missing-directory `/boot/lib` fallback, not proven cache aliasing.

Ext2 directory type, emptiness and link accounting now pass with offline fsck.
Access variants still do not evaluate requested permission bits; classic
relative readlink needs a cwd audit.

The bootstrap SIMD probe exposed syscall entry clobbering user XMM/YMM state:
anonymous mmap and brk zero-fill used XMM0 without preserving it. Stack XSAVE/
XRSTOR now protects ordinary syscall entry/return and blocking switches. Audit
fork's inherited vector snapshot and signal ucontext FP-state restoration
separately; the syscall guard does not implement those broader semantics.

Bootstrap allocator/stdio probes also confirmed and fixed whole-page heap
regrowth returning stale bytes after brk shrink, and fstat/ioctl classifying
redirected fd 0–2 by number. Heap regrowth now zeroes released full pages while
preserving the partial boundary page and fork COW owners. Standard descriptors
now follow their actual object; Python can execute large scripts from file
stdin without entering the REPL. These are independently validated defects;
none alone establishes the cause of the intermittent Clang test crash.

Large Bash heredocs also expose the open-unlinked-file lifetime gap: Bash
opens its temporary script then unlinks it, but ext2 frees the inode while the
fd still needs its contents. A Python `-` checker consequently read EOF and
silently did nothing (previously also entering REPL due to wrong TTY metadata).
The module validator now uses a named Python script and checks nonempty outputs.
Delayed reclamation now passes open-descriptor, fork and mapping regressions.
Add a real large Bash heredoc/stdin regression alongside those lower-level probes.

The uncached mmap path exposed process cleanup dropping file PTEs without
releasing their physical references. Cleanup now unmaps and unreferences each
present file page, preserving cache/fork co-owners. Guest exit and exec churn
each faulted 128 MiB and retained only 4 KiB afterward; inherited parent pages
remained readable after child exec/exit. The earlier long build stopped
responding after roughly two million file page faults, consistent with the
leak exhausting an 8 GiB guest; its command bridge could not provide a final
free-memory snapshot. Test long builds and browser restarts for sustained
reclamation, alongside the separate mmap coherency/lifetime work above.

Native bootstrap runtime follow-up: the common startup fault is traced to
musl's `__malloc_alloc_meta+0xc8`, introduced by a missing Nix `libc.so`
lookup aliasing the boot disk's musl. Guest linking therefore produced a
`DT_NEEDED libc.so` alongside glibc dependencies. Nix store paths now have
exact lookup semantics. After rebuilding, CTest passes its first three tests
and a direct `test_kmutex` run passes all eight checks. CTest nevertheless
stalled after threaded tests exited: its mutex-test child was terminated,
reapable and had zero live threads, while CTest remained in epoll_pwait.
The final thread now notifies the parent when the leader becomes waitable,
fixing that stall. Positive sub-millisecond nanosleep now rounds up and checks
its monotonic deadline; reader overlap is explicitly coordinated and rejects
an exclusive-lock mutant. Descriptor workers rendezvous and yield outside the
real fd-table operations, preserving every round and assertion while avoiding
creation-order starvation. All 13 guest tests now pass on one vCPU in 63.90
seconds. Absolute clock_nanosleep deadline rechecks/overflow handling and
kernel/user scheduling under oversubscribed spinlocks remain follow-ups.
A separate user-PF diagnostic incorrectly forced
panic serial locking, stranding UART writers and command control. Removing
that force-lock call is validated by a reaped SIGSEGV child, subsequent fd3
UART sentinel and healthy bridge procs/stats, including after the complete
13-failure CTest run; real kernel panic handling stays unchanged.

Bootstrap performance baseline follow-ups: the fresh SDK install profile
places 77% of samples explicitly in virtio used-ring completion polling. Large
ext2 files update a four-byte indirect pointer per data block; the block layer
performs a synchronous sector read/write for each update. Measure metadata
request batching/writeback and allocation zeroing separately from bulk SG
writes, retaining synced-fsck and lifetime gates. The 500 MB LLVM library NAR
installed successfully in 325.709 seconds, so this capture shows slow progress
rather than a proven storage stall. Final two-path timings belong in bootstrap.md.

Heap diagnostic accounting was corrected in `06f7db3`: coalescing recovers
header/footer payload, and separate expansion blocks consume their own overhead.
The integrity walk now checks the incremental free counter against actual block
payloads. The focused filesystem-exit/truncation guest gate passed this check.

The I/O gate review found success-only `ftruncate` stubs. Real FAT/ext2 resize
landed in `9942505`, with shrink/grow zeroing, cached-descriptor and mapped-backing
regressions in `truncatecheck`. The subsequent `e70ff31` gate passed those tests
on FAT and 4 KiB ext2, reboot verification and clean offline filesystem checks.
Repeat resize-specific coverage on 1/2 KiB ext2 before claiming that matrix.

The fast RAM-backed I/O gate now reproduces both extraction-sized sequential
writes and namespace-heavy small-file operations with persistence and fsck
checks in roughly 23 seconds. The candidate on
`experiment/ext2-indirect-batching-20261015` cuts bulk requests approximately
10-fold but regresses small-file and fragmented phases. Profile those CPU/
scheduling costs before merging it; preserve failure propagation and inode
accounting checks while tuning. Resume complete two-path bootstrap acceptance
after this prerequisite; the clean checkpoint is diagnostic, not a pass.

## Mapping lifetime before coherent shared files

Astra's source review identified a prerequisite to Codium's regular-file
`MAP_SHARED`: one mutation protocol for the complete address space. A file-fault
generation check alone cannot cover anonymous/device replacement or COW.

- The separately referenced address-space object and process-context cleanup
  worker are implemented. The current worktree now integrates one transaction
  protocol across mmap/MAP_FIXED, munmap, mprotect, mremap, brk, fork, memfd,
  file faults, CoW, remote stacks and graphics aliases. The legacy fixed arrays
  are removed. Candidate `83cf2e49` passed focused guest validation; this does
  not replace the deterministic concurrent-fault/cancellation gate below.
  Consuming waits finish a finite worker cleanup batch before returning; add a
  deterministic cancellation test paused inside this cleanup wait.
- Authoritative ranges represent reservations, permissions, sharing and retained
  backing/leaf ownership. PROT_NONE preserves inaccessible populated leaves;
  sparse removal and remapping operate on allocated tables. ELF/interpreter,
  stack and TLS constructors register their ranges. Native and terminal pixel
  sources use retained page descriptors and coherent source snapshots. Native
  mapping removal completes TLB acknowledgement before releasing AS ownership;
  only the independent kernel-buffer owner waits for compositor epochs.
  Host and guest-rebuilt helper tests cover interval oracles, OOM, hidden pages,
  CoW, sharing, sparse remap, backing-selected cleanup and delayed ACK. Use an
  ascending descriptor cursor for bulk retirement to avoid O(leaves × ranges)
  lookup under the PT lock. Huge user leaves remain unsupported.
- File/memfd faults retain a complete range snapshot while preparing outside
  the mutation lock, then validate before primary and adjacent publication.
  `VmmTryMapPage`, sparse detach and permission-checked CoW are integrated;
  detached managed references survive remote TLB acknowledgement and final
  filesystem callbacks run outside mapping locks. An opt-in test kernel now
  passes actual file/memfd fault barriers at retained-snapshot and prepared-page
  boundaries: competing same/adjacent faults, MAP_FIXED, permission removal,
  unmap and cancellation followed by consuming wait/file-pin release, on FAT
  and ext2. This caught and fixed a real stale file-to-anonymous retry failure.
  Frame-entry compositor pauses now pass 64 file-cleanup cases; actual terminal
  exec, 256 KiB output and two resizes pass with an old-alias canary surviving
  retirement. This found and fixed exec pixel-source loss and incoherent TTY
  geometry snapshots. Cancellation inside blocked disk I/O or retirement and
  old renderer-snapshot/in-flight GPU retention remain outstanding.
- Keep lock order mutation mutex, page-table lock, then short PMM internals.
  No filesystem I/O or user-buffer access under the mutation mutex, and no TLB
  acknowledgement wait under an IRQ-disabled page-table lock.
- Require deterministic pause-between-snapshot-and-commit tests, simultaneous
  same-page faults, MAP_FIXED replacement, partial splits, fork/exec, cancellation,
  memfd remap, GUI resize and remote-TLB frame retention. A writable MAP_PRIVATE
  upgrade must copy cached backing rather than alter another reader's page.

The first implementation gate must exercise the same transaction across
`process.cpp`, `syscall.cpp`, `idt.cpp`, `memory/virtual_memory.cpp`,
`window.cpp` and `compositor.cpp`; adding a generation only to file mappings
would leave anonymous/device replacements outside the protocol. Fork must
initialize independent mutation state instead of copying a live mutex. Fault
snapshots must include all range, backing, offset and protection fields, including
speculative adjacent-page installs. Cancellation cleanup must own allocated and
detached frames as well as the current vnode pin.

Use explicit barriers after snapshot and before commit, with no mutex or
IRQ-disabled lock held while parked. Replace that address using anonymous
`MAP_FIXED`, unmap it, or apply `PROT_NONE`/read-only protection before releasing
the fault; replacement bytes and permissions must survive. Also synchronize two
faults on one absent page, replace an adjacent speculative page, and hold a
remote translation through shootdown to verify frames cannot be recycled early.
Follow with private cached-page writable upgrade, fork/exec, cancelled I/O,
memfd replacement and GUI resize. Check both byte contents and balanced frame/
backing ownership. These gates precede enabling regular-file shared writes.

Existing sequential mapping probes do not establish these concurrency properties.
Coherent shared-file pages, read/write visibility, dirty writeback/msync and
resize integration follow this prerequisite; keep unsupported mappings explicit
until those semantics pass real workload and filesystem gates.

## Next workload and showcase sequence (15 October 2026)

Priority is performance and bootstrap acceptance first, then real workload
coverage, then recording and visual polish. Keep application failures and their
reproducers alongside each experiment rather than treating launch as success.

- Work with Astra on the taskbar, app launcher and window chrome: improve
  spacing, typography, icons and visual consistency.
- Improve file-browser icons, text readability and font rendering. Ship open
  source fonts such as Noto Sans for Brook tools; preserve a suitable monospace
  font for terminals and code.
- Add a launcher/configuration flag for the guest timezone, supporting an IANA
  zone such as Europe/London rather than hardcoding today's UTC offset as
  permanent DST behavior.
- Exercise Vim, Emacs or Helix in a shell with editing, saving and reopening.
- Run Rust and C++ compile/edit/run workloads; prepare a live coding example for
  the showcase, with actual compiler output and a running result.
- Test VS Code or VSCodium as a usable editor, then Blender as an interactive
  workload. Record missing syscalls, rendering/input failures and performance.
- Prioritize a usable Firefox or Chromium browser after the preceding
  compatibility checks, including real page navigation and interaction.
- Implement VM-only video recording with synchronized audio, then rehearse the
  live tour described in [showcase.md](showcase.md).


### RUN-06 — GPU resource mapping accounting during SuperTuxKart teardown

SuperTuxKart 1.5 completed a timed Lighthouse AI race on the eight-core, 4 GiB
virgl VM `0ac11e61f9d245468d1c7a452301397b` with kernel
`af3d143c36e7b93a1e5d0c38df466deeb46973cd94cf527538cd8971f5a97948`.
After the game window closed, guest `procs` contained neither the main process
nor its workers. That run did not preserve a numeric application exit code.
Cleanup logged exactly 2,976 `BRO176-MAPLEAK` warnings followed by 2,976
`BRO176-MAPUNDERFLOW` warnings. Sample physical pages are `0x124852000`,
`0x15c5b9000`, and `0x126a77000`; the leak messages identify Device-tagged pages
with mapCount=1, while subsequent decrements see mapCount=0. Current warning
instrumentation does not print caller traces, so the responsible mapping and
unmapping paths remain unproven.

The VM remained responsive, completed guest sync, and both data and Nix disks
passed offline `e2fsck -fn` with status 0. This does not establish correct GPU
resource lifetime accounting. Preserve the original images/logs and investigate
map/ref ownership on import, release and process teardown after the current
storage performance work. Local evidence is under
`build/showcase/stk-fixed/game-ext2/`, including warning counts, all warnings,
post-race process state, stop acknowledgement and filesystem checks.

## Compatibility limits audit (requested 15 October 2026)

Audit fixed limits across the kernel, drivers, userspace helpers and VM bridge
before they become the next large-workload blocker. Codium exposed a concrete
example: executable readers silently clipped a 193 MiB binary to 32 MiB. Earlier
argument/environment, GPU-object and thread/resource limits show this is a
recurring compatibility issue. This is a separate backlog investigation; the
current Codium and showcase work continues.

Inventory each limit with its source location, units, scope (per process,
thread group, mount, device or system), allocation cost, error/overflow behavior
and workloads likely to reach it. Distinguish required architectural/ABI bounds
from implementation budgets and historical convenience constants. Check both
producer and consumer sides: raising one array or counter can expose a smaller
queue, buffer, field or address range elsewhere.

Prioritize:

- Silent truncation, ignored short reads/writes, clamped counts reported as
  success, and integer-width/alignment overflow.
- Success-only syscall stubs and untouched output parameters. Firefox exposed
  `fallocate` returning success with a zero-length memfd, followed by
  `getsockopt(SO_SNDBUF)` returning success without writing the capacity.
  Inventory remaining stubs (including socket setters), implement required
  behavior or return a supported error that lets callers choose a fallback;
  verify actual state/output rather than only a zero return value.
- Executable/interpreter size and mapped span, brk/mmap/stack address ranges,
  file mappings, segment counts and argument/environment buffers.
- Processes, threads, file descriptors, futexes, waiters, timers and signal
  queues; exhaustion, cleanup and recovery under concurrent churn.
- Filesystem file/directory sizes, indirect blocks, paths/names, mount/open-file
  tables, package-index and NAR/decompression buffers.
- GPU resources, command/transfer sizes, fences and outstanding submissions;
  display dimensions, windows, terminals and input queues.
- Network sockets/backlogs, packet and message sizes, Unix ancillary data;
  syscall/procfs interfaces that advertise capacities to applications.

Rank findings by correctness risk and expected impact on Codium, Blender,
Firefox/Chromium and concurrent showcase workloads. For each important bound,
choose safe dynamic growth, a measured/configurable budget, or a justified
fixed bound with an explicit error. Add boundary and exhaustion/recovery checks
where failures could corrupt state or silently change results. Record remaining
limits and make advertised capabilities agree with actual implementation.
Do not resolve the audit by raising every constant indiscriminately.


## Desktop settings and package discovery (requested 15 October 2026)

- Show real application icons in window chrome, the launcher and an icon-based
  taskbar, with readable hover labels and focus/minimized indicators. Preserve
  Wayland application IDs so documents with changing titles retain the right icon.
- Provide shared desktop themes and a Settings app with a window-chrome preview.
  Save the selected theme as persistent user settings and restore it on boot;
  missing or invalid settings must fall back to the default theme. Theme the
  Brook desktop surfaces consistently; third-party toolkit themes are separate.
- Generate the launcher from installed applications, validate executable targets,
  and suppress unavailable custom shortcuts. Refresh safely after install/remove,
  without doing filesystem scans in the compositor or freeing icons it is using.
- Add a Nix-backed graphical app store: search/browse available packages, show
  installed state, install/remove with progress and actionable errors, and launch
  installed desktop applications. Reuse the real package installer and its
  dependency handling. Verify large-package failure/retry, cancellation, disk
  exhaustion, launcher updates, and persistence across reboot.

Icons, theme switching/persistence, installed-profile discovery and safe live
launcher refresh passed guest acceptance on 15 October; see
[desktop-settings.md](desktop-settings.md). The graphical app store, runtime
conversion of icons not preconverted by the image builder, terminal-only
desktop-entry launching and smaller-display acceptance remain outstanding.


- Investigate profiling export timeouts and dropped records under the bootstrap
  workload. The prepared `e5dd9aa` run retained 2,280 compile CPU samples but
  reported 12,060 dropped records; the clean run disabled further captures after
  an export timeout. Preserve exact loss accounting and avoid treating retained
  proportions as an unbiased performance profile.


## Ordinary-file exclusive creation — fixed

The 15 October GNU tar overwrite bug was missing O_CREAT|O_EXCL semantics:
openat("item", 0x809c1) incorrectly succeeded on an existing file, so tar never
entered its overwrite path and a six-byte entry retained the old 46-byte tail.
Ordinary VFS creation now rejects existing entries atomically, including final
symlinks, under the namespace mutation gate. Checked EEXIST and truncate errors
survive the legacy boot alias. Sixteen rounds of eight concurrent creators passed
on FAT and ext2, alongside symlink and actual tar regressions; see workloads.md.

FAT default GNU tar extraction still reports unsupported timestamp/mode
restoration, including fchmodat2 (452). Data overwrite passes with explicit
`--touch --no-same-permissions --no-same-owner`; implement truthful FAT metadata
semantics or document filesystem-specific extraction policy separately.
General path resolution/backend errno propagation remains limited; this fix
is not comprehensive Linux open/pathwalk acceptance.


The shared-file foundation now has canonical ext2 live-inode pages, ordinary-I/O
coherence, weak per-range file registrations and ordered file-gate publication.
PMM counters use checked 32-bit counts, with 70,000 hosted aliases and 512 guest
framebuffer aliases covered. The ABI-14 registration checkpoint passes seventeen
hosted tests, focused guest coherence, 44 mapping races, 64 renderer cleanup cases
and the I/O/compatibility/lifecycle gates. See `file-shared-mappings.md` and
`build/workloads/file-registry-20261016/audit.json` for scope and limits.

Next implement mapped resize/EOF invalidation, including retiring registrations,
then dirty writeback/msync and durability/error tests. Regular-file writable
MAP_SHARED remains disabled; mapped resize still returns EBUSY. Renew both full
bootstrap paths and browser acceptance after the connected implementation.
FAT retains its prior uncached path.


## Current real-workload and desktop follow-ups (2026-10-16)

Preserve this scope while the mapping/I/O work and both bootstrap acceptance paths
are completed. A prior pass is not acceptance of the current candidate.

- Exercise shell editors and Rust/C++ compilers, then Codium, Blender,
  Firefox/Chromium, GIMP and OpenTTD. Record exact package, launch command,
  observable interaction, failure and cleanup for each; a visible window alone
  is insufficient. OpenTTD follows the user's named game request.
- Put the existing Brook mark on the launcher. Investigate corruption during
  boot display transitions. Complete Quake relative mouse/capture work in RUN-04.
- Implement fastfetch with the Brook mark; test zsh, kitty and starship. Change
  shell/terminal defaults only after successful interactive verification.
- Prepare direct VM recording and a rough one-take tour script, with readable
  editor text, progressively more demanding applications and deliberate window
  activation. Music is the user's local `Brook 2000.mp3`. Do not start showcase
  production yet; no copyrighted game data or private media enters git.
- Apply the Astra chrome review in `ui-chrome-concepts.md`: separate proportional
  UI font atlas, clear controls, consistent spacing, file-browser icons/text,
  caption release/cancel semantics, nearby taskbar tooltips and UTF-8 titles.
  The rendered design sheet is a concept rather than a running desktop capture.
- Investigate GTK/Qt theme exports after resolving actual HOME/XDG config paths.
  Check GTK3, GTK4/libadwaita and Qt separately; do not assume one CSS file or
  environment variable provides live, identical styling across toolkits. Native
  settings preview should reflect the same semantic palette.

Normal caption-close no longer automatically kills a WM client. A bounded guest
probe survived two real close clicks and ten seconds afterward. Still test an
actual unsaved document's confirmation, cancellation and eventual normal exit.


Fresh-image environment follow-up: remove the unconditional
`GIMP3_PLUGINDIR=/nix/share/gimp-lite` from default process environment. Experiment
images and guest installation do not stage that optional reduced tree. Legacy
lite scenarios already select an explicit gimprc; ordinary package tests should
exercise the package's own plugins. Source correction is staged; full GIMP
startup, plugin discovery and image editing/export acceptance must be rerun.
Persistent HOME/XDG consistency remains separate: without a dedicated home
volume, `/home` resolves to the writable FAT ramdisk, whereas native settings
persist under `/data/home` on ext2. Existing wrappers differ in how completely
they redirect HOME and XDG paths. See `ui-chrome-concepts.md` for the source audit.


Observed diagnostic limits to revisit: the bridge's 220-byte guest command line
includes its internal prefix, leaving less room for the user's launch command.
Return the usable size clearly or add a guest script-transfer/execution path.
During full GIMP startup, one compositor interval saturated at UINT32_MAX;
distinguish clock discontinuity/underflow from actual long refresh gaps instead
of presenting an unexplained saturated value as a measured frame time.


GIMP follow-up after allocator indexing: full plugin discovery still reports
`exit_group timeout ... cpu=-1, retaining pending thread` for some helper
processes. Inspect pending protected kernel operations and their wakeups; do not
release filesystem/mapping state just because the bounded quiesce wait elapsed.
The diagnostic source intentionally defers termination until protected operations
unwind. Separate this from the now-attributed allocator search cost.


GIMP basic workflow now passes on the interruptible-wait checkpoint: new image,
brush editing, XCF save/reopen, PNG export/import, normal quit, and reboot
persistence. Next: optional-plugin file-mapping write faults during discovery;
separate zombie address-space retirement from wait metadata to reduce transient
memory pressure. Mainline full bootstrap pair must be rerun on this checkpoint.

Profiler follow-up (16 October): owned-stack walking is fixed and verified on
real compiler stacks; see [bootstrap.md](bootstrap.md#recover-compiler-profiler-call-chains-2026-10-16).
The recorded context-switch chains expose file/mapping mutex waits previously
hidden behind truncated DoSwitch frames. Dropped events and IRQ-masked timer
blindness remain; their counts cannot establish elapsed-time percentages.
PERF-01's free-block index and allocation-size guards have shipped; the later
file-page hash-growth and user-fault IRQ experiments were slower and remain
unapplied. Keep those experiments separate from the accepted CR2 address fix.


Firefox follow-ups after the rendering/input acceptance (16 October): investigate
long-session normal shutdown retaining workers after the window closes. The
Google/GitHub/Amazon run required guest SIGKILL; a shorter fresh-boot Google run
quit normally. Preserve the dropped-record count when interpreting the saved
shutdown profile. Correct maximized Firefox client-side shadow/content geometry
(the buffer includes 52 pixels beyond the requested content dimensions). Renew
hardware-rendering and longer browsing acceptance separately. The software
rendering, multi-seat input and pinned initrd CA fixes are accepted as recorded
in workloads.md; the default app checkpoint remains unchanged.
