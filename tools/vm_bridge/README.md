# Brook VM bridge

Follow-up roadmap: [efficiency, artifact delivery and UI control specification](../../docs/vm-bridge-next.md).
Reference-first artifact delivery and command jobs are implemented. Semantic UI
controls, observation bundles and the remaining roadmap are still proposed.

A local Python MCP server for explicitly enabled Brook QEMU
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
| `vm_observe` | Return PNG artifact reference and frame ID, native geometry, crop origin and scale. Optional native `crop`, `max_width` and opt-in `include_image`; leased completed frame, GPU readback at the frame boundary, and brief VM pause. Requires capture API 2 in the kernel. |
| `vm_input` | Ordered key/chord/text/pointer/scroll/relative/hold/wait batches. Coordinates require the current `frame_id`; crop/scale map back to native coordinates. US ASCII text only; holds release at batch end/cancel. Observe to verify reaction. |
| `vm_logs`, `vm_wait` | Bounded serial/QEMU byte cursors or bounded serial-substring waits. Logs survive VM exit. |
| `vm_guest_run` | Structured `argv`, optional `cwd`/`env`/`uid`, required `request_id`, and optional `wait_s` return a recoverable command job with isolated output and exit status. Default identity is Brook. The explicitly legacy `command: "run …"` form still returns launch ACK only and fits 220 bytes. |
| `vm_apps` | Published installed-app/validated-shortcut catalog, stable IDs, generation-bound references and build-time availability. Does not launch apps. |
| `vm_windows`, `vm_window`, `vm_ui` | Boot-bound native/Wayland discovery, semantic actions, bundled window state and read-only request recovery. |
| `vm_job` | Inspect/wait for an `operation_id`, with independent stdout/stderr byte cursors and real exit/descendant/EOF state. Does not execute or resume commands. |
| `vm_guest` | Existing window/process listings and focus by window index (the `pid` argument). |
| `vm_stats` | Usable RAM/free/used and process counts; p50/p90/p95/max of the recent 256 compositor-loop intervals using the millisecond guest clock. Optional `memory_details: true` adds a rolling address-space census. Not app GPU or physical scanout timings. |
| `vm_profile` | `start` with `duration_ms`, `stop`, `status`, `export`. CPU samples and context-switch events; export after IDLE to raw/Speedscope artifacts using the run's immutable kernel ELF. |
| `vm_debug` | Current vCPU registers or up to 1 MiB physical/virtual memory. Virtual addresses use the selected vCPU's current translation. RAM-only checks exclude MMIO; memory reads pause vCPUs and restore prior running state. Devices may still run. |
| `vm_record` | Asynchronous start/status/stop of bounded VM-only H.264/AAC recording through a private display stream. Requires the pinned host encoder; see [setup and acceptance](../../docs/vm-recording.md). |
| `vm_audio_capture` | 0.1–10 seconds of QEMU PCM WAV with format, frames and host monotonic interval. No microphone. Launcher watchdog bounds abandoned recordings. |
| `vm_audio_analyze` | Session WAV waveform PNG, RMS and silent bins. PCM silence alone does not identify stutter. |
| `vm_artifacts` | Describe scoped ignored artifacts by absolute path, MIME, bytes and checksum, including after VM exit. `include_image: true` opts in to a PNG preview. Binary exports are never embedded in JSON. |

### Media delivery (bridge 0.3.0)

Image-producing tools default to `include_image: false`. Text results contain
an `artifacts` array with session-scoped `artifact_id`, absolute `path`,
`mime_type`, `byte_length`, `sha256`, and image dimensions. Frame metadata keeps
its original epoch, coordinate extent, crop origin and scale. Waveform results
identify both the plot and source PCM. Existing files are reused without copying
or re-encoding. The adapter also extracts nested typed binary results into new
run-scoped files. Ordinary guest text is untouched, including base64 strings.

Use a local image viewer on the returned path. Clients supporting native MCP
image content can request `include_image: true`; previews are bounded to 8 MiB
total and four images. Do not dump that entire media-bearing envelope as JSON
text. Native MCP image transport still contains encoded bytes. Clients unable
to open local paths should opt in to that preview rather than treat the path
as a remote URL. Standalone CLI calls use the same reference-first default.

Artifact descriptions validate session access even after shutdown, reject
symlinks/nonregular files and known unfinished captures, and hash files with
bounded memory. Existing artifacts without a recorded monotonic creation time
report `created_host_monotonic: null`; `described_host_monotonic` is the current
description time. This does not invent capture timing from a file's wall clock.
`vm_doctor.bridge_capabilities.artifact_delivery` advertises revision 1. Reload
an already running MCP server to receive these source/schema changes.

### App discovery (bridge 0.5.1)

`vm_apps` reads the same immutable catalog used by the Applications menu. It
returns exact desktop-file identity separately from an opaque `app_id`, a
`catalog_generation`, `catalog_built_tick_ms` and a boot/session-bound reference.
IDs remain stable across catalog rebuilds; exact identity bytes and source
namespace matter. Legacy shortcuts use their path; older desktop manifests
without IDs fall back to command plus title. Duplicate/colliding IDs reject the
candidate catalog and preserve the previous menu. Distinct desktop entries with
the same display title remain distinct.

Availability describes executable/profile validation or shortcut requirements
**at catalog build time**, not a fresh per-query filesystem scan. `catalog_ready`
is false before first publication. The launch worker revalidates dependencies
and executable/profile availability before executing a request. `launch_eligible`
means the entry passed catalog validation, not that queue capacity or application
startup is guaranteed.

Launcher requests now retain their identity until dispatch completes even if a
window appears first. An early window cannot hide a later dispatch failure;
coalescing never overwrites an admitted command/icon, and an incompatible
catalog generation returns "App launch already pending". Absence of a live
launch-tagged process does not prove a single-instance handoff failed. It keeps
waiting for window evidence or the existing five-minute timeout.

`accept_apps.py --session SESSION --out build/apps-acceptance` tests discovery,
stable IDs after refresh and a real Mousepad menu click on an empty disposable
1920×1080 desktop at 100% scale (up to 40 initial entries). The optional
`--negative-tests` temporarily edits/restores the manifest as maintenance UID 0
to check equal titles, duplicate IDs and a removed executable. It must use private
test images. The negative cases retain their rejection log evidence.

### Semantic launcher (bridge 0.6.0)

Pass an exact `vm_apps` app `reference` and a fresh 32-character lowercase hex
`request_id` to `vm_launch`. This calls the same admission/worker path as a real
Applications menu click, including identity/environment, frozen command/icon,
coalescing, pending cursor/taskbar feedback and worker revalidation. Stale catalog
references reject with raw admission error `-116`; fetch `vm_apps` again rather
than silently targeting a different version of an entry.

The response separates `admission_state`, launch history and a fresh windows
observation. `wait_for` accepts `admitted`, `dispatched`, or `window` (default).
`dispatched` means the normal worker returned success; `window` additionally
requires the observed exact window incarnation to still exist. Neither proves
application readiness, client responsiveness or successful single-instance
activation. `correlation=app_id_presence` is weaker than `launch_tag` and can
refer to an existing window; `existing_window` labels that fact. Raw dispatch
failure remains authoritative even if a window appeared before dispatch ended.

A bounded inline wait (0–20 seconds, default 2) leaves the launch running.
Recover through `vm_ui` with the same operation ID and launch wait condition.
Identical resubmission recovers existing evidence; it never launches again.
Changed arguments or a different operation kind with the same ID reject. Guest
reboot invalidates the request; stopped VMs return labelled cached evidence,
retaining known dispatch facts without claiming a current window.

UI requests and their tombstones share a 256-record boot-lifetime limit, with at
most 16 frontend requests queued/dispatched. Bridge launch history separately
retains 256 launch tokens until reboot; exhaustion rejects before launch
admission. Ordinary user launches do not consume this history capacity. Window
correlation polls at 100 ms for at most five minutes after admission; its deadline
is reported. Raw worker completion remains recoverable beyond that deadline.
Read-only history polling does not itself dirty the compositor.

Takeover cancels queued frontend requests and bridge-only queued launches;
requests coalesced with a user launch are preserved. An already-started worker or
frontend request already selected for dispatch may continue. The separate
`already_dispatched` and `launcher_already_started_or_shared` counts describe
these boundaries; takeover does not terminate applications.

`accept_launch.py --session SESSION --out build/launch-acceptance` exercises
native and Wayland semantic activation, durable recovery, stale catalog and
takeover using an empty disposable 1920×1080 VM at 100% scale. Optional
`--negative-tests` temporarily edits/restores the manifest as maintenance UID 0
and verifies the raw worker failure after removing a published executable.
Real menu-click acceptance remains
separate in `accept_apps.py`; semantic activation does not certify hitboxes.

### Semantic windows (bridge 0.5.0)

`vm_windows` returns native and Wayland windows without taking a screenshot.
Copy a window's exact `reference` into `vm_window` with an `action` and fresh
32-character lowercase hex `request_id`. Supported actions are `focus`,
`minimize`, `maximize`, `restore` and graceful `close`; each row advertises its
supported subset. For restore, supply `restore_target`: `previous` preserves
maximized/normal state and unminimizes, `normal` restores the saved rectangle,
`maximized` maximizes and unminimizes. Maximize preserves minimization.

A reference includes window ID, uint64 creation incarnation as a decimal string,
host session generation and guest boot epoch. A first-wins UUID handshake binds
the epoch once per guest boot; a guest self-reboot invalidates references and
operation reads even if QMP reset was not used. This does not detect restoring
a whole VM snapshot that contains the old epoch; snapshot restore must change
the host generation. Jobs require guest job API 2 with these guards. Legacy
coordinate input and old `vm_guest` index-based focus do not yet use this new
epoch contract; prefer the semantic interface for window mutations.

Requests dispatch on the compositor through the chrome/taskbar focus, minimize,
resize and close paths. `completed` confirms WM state applied or graceful close
enqueued, not a client configure acknowledgement, completed redraw or process
exit. Native close sends SIGHUP to a terminal or SIGTERM to another native
provider; Wayland close sends the ordinary close event, never a signal to the
shared Wayland server. Unknown client ownership and kernel-service close are
unsupported. A full notification queue rejects the semantic action explicitly.

The response includes a fresh windows observation, with its host capture
interval and guest sample tick. The WM values are captured under one lock;
process liveness is sampled afterwards. No correspondence to a framebuffer is
asserted. `buffer_width/height` on API windows report the actual allocated VFB
extent, separately from requested geometry; a matching size is not a formal
client configure ACK. Frames remain a separate `vm_observe` artifact.

`wait_s` is 0–20 (default 2). `wait_for` defaults to `applied`; close can wait
for `closed` or `client_group_exit`. Results report these facts separately.
Wayland client metadata identifies the kernel-authenticated origin of an
accepted Unix stream, associated with the surface by its provider. It is not
proof of the current owner after socket inheritance/passing, nor authority to
signal that PID. Unknown origins remain null. Exact origin-group exit is not
proof that all application descendants have exited.

`vm_ui` recovers by `operation_id` without replay. Same-ID identical actions
recover evidence; changed arguments reject. Unknown transport delivery never
triggers an automatic mutation retry. The guest reserves 256 request IDs until
reboot, allows 16 queued/inflight and applies at most four per compositor turn.
Wait cancellation/timeout leaves accepted work pending. `vm_control` takeover
blocks new host UI submissions, cancels queued guest UI requests, and reports
already-dispatched requests; it cannot undo an event already sent to a client.
Takeover/release transitions serialize across bridge clients. If guest takeover
is unavailable, the host block still applies and the response says so.

The reusable `accept_ui.py --session SESSION --out build/ui-acceptance`
workflow expects a native Terminal and clean Mousepad already open, exercises
both, captures them and gracefully closes them. Run it only on a disposable VM.
It checks idempotent recovery, actual Wayland buffer dimensions, disappearance,
exact client-group exit and survival of the unrelated app/shared server.

Actual installed-app discovery/semantic launcher activation remains the next
phase. Running an executable through a command job does not exercise launcher
feedback or single-instance activation.

### Guest command jobs (bridge 0.4.0; boot guards in 0.5.0)

Structured requests go through the guest's job protocol, without a host shell
or serial-log scraping. For example, provide the following arguments to
`vm_guest_run`, substituting the explicit session and a fresh 32-character
lowercase hexadecimal request ID:

```json
{
  "session": "SESSION_ID",
  "request_id": "0123456789abcdef0123456789abcdef",
  "argv": ["/boot/BIN/BASH", "-c", "printf 'hello\\n'; printf 'warning\\n' >&2; exit 7"],
  "cwd": "/data/home",
  "env": {"CUSTOM": "literal value"},
  "wait_s": 10
}
```

`argv[0]` must be a real absolute guest path; the legacy shell's fallback
lookup is not applied. On the acceptance image Bash is `/boot/BIN/BASH`;
`/bin/bash` is absent even though the old shell can resolve that spelling.
Default cwd/home is `/data/home`, UID 1000 and primary GID 100. `uid: 0` is an
explicit maintenance option. Environment overrides merge with the desktop's
framework, font, TLS and XDG defaults. Stdin is `/dev/null`. Job launches do
not create an automatic framebuffer window or exercise the Applications UI;
semantic launcher activation is separate future work.

Requests support 1–64 NUL-free arguments, 64 environment overrides and a
32 KiB total serialized payload, uploaded in bounded checked chunks. Paths
for the executable and cwd are at most 255 bytes. The guest retains 256 job
identities per boot, permits eight active/uploading jobs, and retains the last
64 KiB per stream. Capacity exhaustion is explicit; IDs are never evicted and
reused. Worst-case retained output/request storage is about 40 MiB, plus active
pipes and process overhead. The capability handshake reports these limits.

The returned `operation_id` is the request ID. Its host record binds the ID
to session generation, guest boot epoch and an argument fingerprint before any mutation. An
identical resubmission returns the existing job. Changed arguments or a new
generation/guest boot with the same ID are rejected. After interrupted staging, an
explicit identical resubmission observes the guest first, verifies duplicate
chunks against its immutable request and resumes only unfinished staging.
If the guest already started, commit is skipped. A failed observation remains
unknown; it never triggers a blind launch retry. Pure status/capability/output
reads can retry transport errors within their deadline.

`vm_job` takes `operation_id`, optional `wait_s` (0–20), `stdout_offset`,
`stderr_offset` and `output_limit` (1–16384 bytes per stream). The inline
observation budget is 20 seconds; longer commands keep running. Cancelling a
wait does not terminate the workload. Control has a separate executor so four
long waits cannot prevent shutdown/takeover. Kernel job cancellation and
general event waits remain future work.

Results distinguish leader exit, collected exit code/terminating signal,
remaining tagged descendants, original POSIX process-group tasks, and each
stream's EOF. Fork/threads inherit an immutable job tag across exec; reusing a
PID cannot join another job. Overall `completed` requires collected leader
status, no live tagged descendants and both pipe EOFs. The leader can exit
while descendants retain output, and streams can close while descendants run.

Each output result contains `text`, `next_offset`, `eof`, explicit
`dropped_bytes`/`gaps` and pending UTF-8 bytes. Follow `next_offset`; an incomplete
UTF-8 tail is not consumed. Decoding resets across retention gaps so disconnected
bytes cannot invent characters. `guest.stdout_eof` describes the source pipe;
`stdout.eof` also requires reaching the end of the requested retained output.
Full-buffer counters count blocking attempts, not measured sleep durations.

Host records survive server reconnect; output remains in the guest's bounded
ring. Reset invalidates old operations. After shutdown, only saved status/exit
metadata is returned with `cached: true`; full output artifacts and automatic
job-output retention after shutdown remain future work. Check `observation_error`
and per-stream `read_error`/`read_timed_out` before treating a snapshot as fresh.
Normal CLI exit or stdio EOF waits for an in-flight upload to finish staging;
the queued response is flushed immediately. A forced server termination may
still need explicit same-ID staging recovery.

Run real-VM acceptance against an explicit disposable session:

```sh
python3 tools/vm_bridge/accept_jobs.py --session SESSION_ID --out build/job-acceptance
```

The script leaves the VM running. `--resume` reuses completed same-session,
same-request evidence following an interrupted test run. It does not mean an
observation timeout stopped the VM or its workloads.

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
the separate `vm_record` tool uses the QEMU RFB audio stream for video/audio
masters. Guest queue counters and precise source sample/visual synchronization
remain unavailable.
Screenshots and memory pauses can themselves affect performance measurements.

All registry entries, logs, copied ELF files, memory, PCM, images and profile
artifacts live under ignored `build/vm-bridge/`. Licensed Quake assets stay in
ignored experiment disks. They are never included in the plugin package.

## Validation

```sh
python3 -m unittest discover -s tools/vm_bridge -p "test_*.py" -v
```

The host tests cover fragmented tokenized replies, stdio MCP, PNG pixels,
cancellation cleanup, input preflight, crop-coordinate mapping, reset/panic
history, RAM-map selection and resume after failed memory reads. Shutdown
tests cover sync-before-QMP ordering, rejection of missing/negative replies,
timeout/panic/unreachable handling, explicit forced shutdown reporting, and
unchanged pause/resume behavior. Wrapper tests cover first-run controls,
startup demos, explicit commands, renderer selection and saved preferences.
Media tests cover default/explicit previews over stdio, nested payloads,
coordinate preservation, legitimate guest text, waveform/PCM references,
large binary exports, preview bounds, stopped sessions, unfinished captures,
and symlink/path/FIFO rejection.

Reference delivery acceptance (17 October 2026): 80 host tests, 75 passed and
five optional encoder-runtime tests skipped. A live eight-vCPU/4 GiB VM passed
full-resolution and cropped/scaled capture, byte-identical explicit preview,
PCM/waveform references, and artifact recovery after shutdown. The 1920×1080
frame's response was 1,784 bytes; its old base64 image payload alone would be
5,626,572 bytes. This measures response size, not faster GPU capture. Guest
checked sync, QEMU exit and all three offline filesystem checks passed. The
test used the source server; the already-connected Codex MCP still needs reload.

Command-job acceptance (17 October): 96 bridge tests, 91 passed and five optional
encoder tests skipped; all 27 hosted kernel suites passed. Ten live scenarios
passed on the eight-vCPU/4 GiB kernel, followed by a public MCP `vm_guest_run`
stdio check and a clean missing-executable rejection. The parent-exit case
reported exit 9 with two remaining tasks and open output; another case reported
both EOFs with one task still running. The 140,000-byte output test explicitly
reported 74,464 discarded bytes. TERM/KILL producer stress recorded 20 stdout
and 11 stderr full-buffer blocking attempts, then zero descendants and both
EOFs. The final process list returned to nine baseline services. Checked sync,
QEMU exit, serial error screening and all three offline filesystem checks passed.
Evidence is under ignored `build/workloads/gallery-fresh-20261017/job-acceptance/`
and `job-final-check/`. Reset/panic, interactive stdin and post-shutdown full-output
retention are not covered by this live gate.

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

## Continuous recording

Prepare the optional pinned encoder before VM launch with
`nix-build tools/vm_bridge/recording-runtime.nix -o build/vm-recording-runtime`.
Use `vm_record` start/status/stop instead of looping still captures. The MCP
server itself needs only Python; encoding runs in the prepared runtime. See
[recording setup, evidence and limits](../../docs/vm-recording.md).
