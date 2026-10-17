# Brook VM bridge

A local, dependency-free Python MCP server for explicitly enabled Brook QEMU
sessions. It observes and injects input into the selected VM through private
QMP sockets and Brook's guest debug channel. It never captures the host desktop.

## Start and connect

Build/update the boot image and create experiment seed disks using the existing
[fresh-build instructions](../../docs/experiments.md). Then opt in a user launch:

```sh
BROOK_VM_BRIDGE=1 BROOK_AUDIODEV=none nix-shell --run './scripts/run_experiment.sh desktop'
```

`./scripts/run-qemu.sh --bridge` also enables discovery for a direct launch;
its disks are the caller's choice. Experiment launches always clone seeds.
The feature defaults to silent QEMU audio; HDA remains present and PCM can be
captured. Explicitly setting `BROOK_AUDIODEV=pipewire` enables speakers.

Register the checkout with Codex (replace the absolute checkout path):

```sh
codex mcp add brook-vm --env BROOK_ROOT=/absolute/Brook -- python3 /absolute/Brook/tools/vm_bridge/server.py
```

Reload the client or start a new conversation to discover newly registered
tools. This checkout is already registered on Ian's machine. The bridge works
for both user and agent launches; reconnecting does not restart an opted-in VM.

The directory also contains portable `plugin.json`, `mcp.json` and a workflow
skill. When copying this package elsewhere, configure `BROOK_ROOT` to the
checkout and, optionally, `BROOK_VM_REGISTRY` to its existing registry. The
verified integration here uses direct stdio registration, not marketplace
publication or installation of an existing desktop-control plugin.

Standalone diagnostic calls use the same tool implementation:

```sh
python3 tools/vm_bridge/server.py --call vm_sessions
python3 tools/vm_bridge/server.py --call vm_doctor --arguments '{"session":"SESSION_ID"}'
```

The host process needs access to the run's private Unix socket and `/proc`
identity. Sandboxed Codex shell tests require host permissions for sockets,
KVM/DRM and Nix; the installed local MCP runs as the user. No host shell tool
is exposed through the MCP. `vm_start` invokes the existing Nix experiment
launcher with a named scenario, rather than accepting arbitrary host commands.

## Tools

| Tool | Contract |
| --- | --- |
| `vm_sessions`, `vm_doctor` | Discover opted-in runs and inspect actual transport/guest capabilities. Select an explicit session. |
| `vm_start` | Clone seed disks and start `desktop`, `video`, `quake-soft`, `quake-gl3` or `office`; `gpu` is `software` or `gl`. Audio defaults to `none`. |
| `vm_control` | `pause`, `resume`, `reset`, `stop`, `takeover`, `release`, or controlled `panic`. Stop/reset wait for guest metadata sync; explicit `force: true` permits shutdown when sync cannot complete. Reset changes the generation and invalidates frames. |
| `vm_observe` | Return PNG and frame ID, native geometry, crop origin and scale. Optional native `crop` and `max_width`; leased completed frame, GPU readback at the frame boundary, and brief VM pause. Requires capture API 2 in the kernel. |
| `vm_input` | Ordered key/chord/text/pointer/scroll/relative/hold/wait batches. Coordinates require the current `frame_id`; crop/scale map back to native coordinates. US ASCII text only; holds release at batch end/cancel. Observe to verify reaction. |
| `vm_logs`, `vm_wait` | Bounded serial/QEMU byte cursors or bounded serial-substring waits. Logs survive VM exit. |
| `vm_guest_run` | Launch a Brook `run /absolute/guest/path args` command, optionally `--uid 1000`. Return PID; no process completion or isolated stdout/stderr yet. Entire framed command fits 220 bytes. |
| `vm_guest` | Existing window/process listings and focus by window index (the `pid` argument). |
| `vm_stats` | Usable RAM/free/used and process counts; p50/p90/p95/max of the recent 256 compositor-loop intervals using the millisecond guest clock. Optional `memory_details: true` adds a rolling address-space census. Not app GPU or physical scanout timings. |
| `vm_profile` | `start` with `duration_ms`, `stop`, `status`, `export`. CPU samples and context-switch events; export after IDLE to raw/Speedscope artifacts using the run's immutable kernel ELF. |
| `vm_debug` | Current vCPU registers or up to 1 MiB physical/virtual memory. Virtual addresses use the selected vCPU's current translation. RAM-only checks exclude MMIO; memory reads pause vCPUs and restore prior running state. Devices may still run. |
| `vm_audio_capture` | 0.1–10 seconds of QEMU PCM WAV with format, frames and host monotonic interval. No microphone. Launcher watchdog bounds abandoned recordings. |
| `vm_audio_analyze` | Session WAV waveform PNG, RMS and silent bins. PCM silence alone does not identify stutter. |
| `vm_artifacts` | Fetch scoped ignored artifacts, including images and exports after VM exit. |

Input is serialized across clients. Human takeover interrupts subsequent input
and keeps observation available. Other mutations are still explicit tools;
takeover is not a blanket lock on reset or guest execution. A stale descriptor
or reused QEMU PID is rejected. There is no automatic retry of ambiguous guest
mutations and no automatic reset after panic.

Stop file-writing workloads before `stop` or `reset`. Both request synchronous
`bridge sync` and wait up to 30 seconds for its completion reply before sending
QMP `quit` or `system_reset`. A launch acknowledgement from running a userspace
`sync` command is insufficient. Missing replies, old kernels without this
command, paused guests, panic and unreachable guests abort the action. Resume
a paused guest first when possible. Explicit `force: true` allows the action
after a failed sync attempt and returns `guest_sync_completed: false` with its
reason; cached allocation metadata may be lost and filesystems corrupted.

Current kernels return `guest_sync_completed: true` and
`sync_guarantee: checked_filesystem_and_device_flush` after checking filesystem
writeback and device flush results. A failed acknowledgement aborts shutdown
before QMP; retry only after resolving the failure. Older kernels may report
`best_effort_metadata_flush`, which is a weaker legacy guarantee. The guest can
write again between sync and QMP shutdown, so this requires quiesced writers;
it does not provide journaled crash recovery or prove physical power-loss
behavior. The I/O acceptance gate injects failed flushes and verifies refusal,
retry and offline filesystem consistency.

## Crash and audio limitations

After panic, QMP state, logs, current registers and bounded RAM reads remain
available. Panic screenshots require one successful observation before the
crash to cache framebuffer geometry. Original fault registers remain in the
panic logs and differ from current panic-spin registers. Guest TCP execution,
profiling and normal input are unavailable after panic. The remembered kernel
panic command pipe and full dumps remain backlog items.

The audio backend is QEMU `none`, which is a suitable `/dev/null` equivalent
while preserving the guest device and PCM capture. The current capture uses
QEMU's deprecated HMP `wavcapture` facility. It is verified with QEMU 10.2.2;
private D-Bus listeners are the intended successor. Chunk-arrival timestamps,
guest queue counters and precise sample/visual synchronization are deferred.
Screenshots and memory pauses can themselves affect performance measurements.

All registry entries, logs, copied ELF files, memory, PCM, images and profile
artifacts live under ignored `build/vm-bridge/`. Licensed Quake assets stay in
ignored experiment disks. They are never included in the plugin package.

## Validation

```sh
python3 -m unittest discover -s tools/vm_bridge -p "test_*.py" -v
```

The 41 host tests cover fragmented tokenized replies, stdio MCP, PNG pixels,
cancellation cleanup, input preflight, crop-coordinate mapping, reset/panic
history, RAM-map selection and resume after failed memory reads. Shutdown
tests cover sync-before-QMP ordering, rejection of missing/negative replies,
timeout/panic/unreachable handling, explicit forced shutdown reporting, and
unchanged pause/resume behavior. Wrapper tests cover first-run controls,
startup demos, explicit commands, renderer selection and saved preferences.

### Optional memory census

```sh
python3 tools/vm_bridge/server.py --call vm_stats --arguments '{"session":"SESSION_ID","memory_details":true}'
```

The updated kernel's `mem census` command samples completed, referenced user
address spaces once each, including shared-VM threads. Each record identifies
the address-space generation and mapping kind. `allocation_pid` is historical
allocation metadata; PID reuse does not change the address-space identity.
`backed_bytes` counts user leaves, including inaccessible PROT_NONE leaves.
`mapped_ram_bytes` counts RAM-backed aliases and therefore double-counts shared
frames. `proportional_bytes` divides by user mapcount to approximate each
mapping's share; cache/staging references do not reduce it. Borrowed device MMIO
is excluded from RAM attribution, while borrowed memfd pages remain included.

This is a rolling diagnostic, not a simultaneous physical-memory inventory.
It excludes construction, staging, cache-only pages, kernel allocations, page
tables, queued teardown and quarantine. The unaccounted remainder does not
establish a leak. Mapping topology is locked while each AS is sampled; PT locks
are released after bounded batches of actual table visits. Empty virtual
subtrees are skipped. AS pins are released before potentially slow output.
Collection can perturb workloads; `round_trip_ms` includes transport and guest
processing, and these samples must not be treated as application timing tests.
The ordinary `vm_stats` path does not run this census. Old kernels report
`supported: false` when they return an unknown-command response. The normal TCP
dispatcher remains unavailable after panic.

Clicks and double clicks now hold their mouse button for 200 ms by default.
Brook samples button state in its compositor loop; the old 20 ms pulse could
fall between frames and disappear at 8–13 FPS even though QMP accepted it.
Set `hold_seconds` on `click`, `double_click` or `keypress` actions to override
the press duration (0.01–1 s). Keyboard chords retain their 20 ms default.
Combined explicit waits and holds are bounded to 20 seconds, and cancellation
releases held input. The contract tests model a 100 ms guest poll and verify
that the default click spans it while an explicitly short pulse is preserved.
This improves input delivery; it does not certify the app's response or solve
window ownership and close-timeout bugs.

Live validation on 14 October 2026 attached to a user-started CPU-rendered VM
and an agent-started Intel virgl GL3 VM. Captured native 1920×1080 frames and
scaled/cropped previews; launched a guest app; exported a profile with 404 CPU
samples and 1,391 context switches; recorded silent-output PCM and waveform;
queried OS stats. A controlled panic preserved a readable panic screenshot,
registers and bounded physical RAM inspection. These checks validate the
bridge, not the deferred GL3 rendering, ext2 or audio-stutter fixes.

See [VM design](../../docs/vm-control-mcp.md),
[backlog](../../docs/backlog.md) and [month roadmap](../../docs/roadmap.md).


### Executable-sharing counters

The guest debug command `mem exec-cache` reports entries, capacity, hits,
misses, admissions, full/probe refusals, exact-comparison hash collisions and
trimmed entries. The immutable executable cache holds at most 131,072 frames
(512 MiB), with a 4 MiB fixed table and at most 32 probes per adoption.
Every user alias is COW; unequal bytes never share a frame even on hash collision.
AS teardown and executable-staging release trim idle entries in bounded lock
batches. Capacity exhaustion leaves a normal uncached COW mapping. Hits include
same-frame aliases, so `hits * 4096` is **not** a memory-saving measurement.
Use physical free-memory measurements and the rolling census together. Active
entries survive trimming; released frames may await PMM quarantine drainage.


## Completed-frame capture

The bridge no longer reads the live render backbuffer. `bridge frame request`
returns a token in Pending state; the compositor services it after presenting a
frame. CPU capture copies the presented front buffer. GPU capture reads the
completed scanout and copies its pixels. `bridge frame status TOKEN` exposes the
immutable snapshot only in Ready state, with its frame epoch and guest tick.
The bridge pauses QEMU for RAM extraction, restores its previous running state,
revalidates token/geometry/epoch, and sends `bridge frame release TOKEN` even
when extraction fails. A different or expired lease is never published as an
image. A second request returns Busy; a wrong release cannot invalidate an
existing capture. Tokens are not reused within a boot.

An abandoned request/snapshot expires after 30 seconds of guest time. The one
snapshot allocation is reused (about 8 MiB at 1080p); no copy/readback occurs
without a request. Metadata rejects runtime display remaps during the captured
frame. Scanout flips and CPU front-buffer capture share the geometry mutex.
The snapshot mutex can wait through readback/allocation/copy; debug commands
never wait for another compositor frame, since they may run on that thread.

Normal images report `source: guest_completed_frame`, `frame_epoch` and
`frame_tick_ms`. These identify a completed render, not GPU timing or the host
capture instant. Panic capture still reads the cached physical framebuffer and
reports null frame epoch/tick, rather than borrowing an earlier image's time.
It is a separate best-effort crash view. Reset invalidates cached metadata.

Use the updated kernel and bridge source together. An older kernel produces an
explicit unsupported-capture error; an already running MCP process needs a reload
to pick up `server.py`. The source CLI and showcase helper use the updated code
immediately. No desktop automation backend is introduced.

CPU and GPU lease acceptance, failure tests and remaining recording work are
recorded in [completed-frame capture](../../docs/completed-frame-capture.md).
This still-capture protocol is intrusive and is not a continuous video recorder.
