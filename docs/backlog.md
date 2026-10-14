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
