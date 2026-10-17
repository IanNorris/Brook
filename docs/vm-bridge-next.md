# VM bridge efficiency and UI control specification

Status: partially implemented, 17 October 2026. Bridge 0.3.0 implements the
reference-first media adapter, optional native previews, recursive binary
extraction and artifact capability revision 1. Existing artifacts without a
recorded monotonic creation time report it as unknown. Bridge 0.4.0 adds
structured command jobs, bounded separate output, exit/descendant/EOF tracking,
durable request identities, verified staging recovery and reserved control
capacity. Bridge 0.5.0 passes [isolated acceptance](vm-bridge-ui-acceptance.md) with boot-epoch guarded jobs and semantic
window discovery/actions, provider/connection-origin identity and per-action
window observations. Bridge 0.5.1 adds read-only app catalog discovery and fixes transient launcher
coalescing/completion races. Bridge 0.6.0 adds semantic launcher activation with
bounded boot-lifetime dispatch history, window correlation and durable recovery.
Bridge 0.7.0 adds [accepted selectable observations](vm-bridge-observations.md)
for frames, windows, tasks, launches and serial errors, with per-component timing
and boot-guarded frame leases. Bridge 0.8.0 adds optional post-action observation
bundles without replaying mutations. Bridge 0.9.0 adds automatic 320/960px previews,
saved native crops and optional native-coordinate ruler/grid overlays, requiring
no additional VM capture. Bridge 0.9.1 adds [accepted verified reset/recovery](vm-bridge-reset.md) with temporary
reboot-policy override, early reference invalidation and explicit current versus
historical evidence. Exact frame/WM revision correspondence, a lossless
guest event stream, output artifacts after shutdown, job cancellation and
remaining sections are still proposed.
Current behavior is documented in the
[bridge README](../tools/vm_bridge/README.md). Preserve the existing VM-only
architecture and [completed-frame capture contract](completed-frame-capture.md).

## Purpose and scope

Make a normal launch, window action, command, profile or audio check return its
useful evidence without several follow-up calls or ad hoc host Python scripts.
Support feature-enabled user-started and agent-started VMs equally, with an
explicit session and generation. Continue using private VM transports; host
mouse, keyboard, desktop, clipboard and microphone are outside this interface.

Required outcomes:

- Binary/image payloads never appear in bridge-owned JSON/text fields.
- A command can return bounded stdout, stderr and its real exit status.
- An observation can include the image, windows, processes and new errors.
- An action can wait for a named result and return that observation together.
- Minimize, maximize, restore, focus and graceful close have explicit hooks.
- Launcher activation exercises the installed-app launch path and its feedback.
- Reconnects/timeouts cannot silently repeat mutations or target reused IDs.
- Panic retains the supported diagnostic subset and promptly rejects commands
  requiring a functioning guest scheduler.

This work does not require a general GTK/Qt accessibility tree. Explicit Brook
window-manager and launcher hooks come first. Pixel input remains available to
exercise actual menu hitboxes and client UI.

## Findings motivating the changes

`vm_guest_run` currently returns a launch PID, not completion or isolated
output. Tests add shell exit markers and separately scan serial logs. Image
capture, window queries and process queries are separate calls. Input injection
reports delivery without proving a reaction. Profiling and audio capture need
separate status/export/analysis calls. New source capabilities have also required
host Python because an already-running MCP process exposed older tools.

`call_tool` already separates screenshots into MCP image blocks and text
metadata. Serializing that entire envelope as JSON can expose its image data.
More directly, `get_artifact` currently returns base64 inside JSON for non-image
binary artifacts. Both paths need an explicit delivery contract.

Current window queries often identify the shared Wayland server as the window
provider. That is insufficient to identify the application to terminate. Slot
indices and PIDs can also be reused. Resolve ownership and generations before
adding semantic close or process-correlated waits.

## Common response and capability contract

Add an independently versioned bridge protocol and per-feature capabilities to
`vm_doctor`. Include the MCP package/server revision, guest build identity,
session generation and capability revisions. Capture API 2 remains a distinct
capability; its number is not the version of this entire protocol.

Report unsupported, disconnected, panicked and stale-generation states
explicitly. An unavailable capability must not silently use a different action
path. For example, semantic close must not fall back to killing a process.

All operations return a compact common envelope, conceptually:

```json
{
  "session": "...",
  "generation": "...",
  "operation_id": "...",
  "state": "completed",
  "effect": "confirmed",
  "result": {},
  "artifacts": [],
  "events": {"next_cursor": "...", "dropped": 0},
  "timing": {"started_host_monotonic": 1.0, "ended_host_monotonic": 1.2}
}
```

These are proposed fields, not current wire output. Distinguish operation state
from effect: queued, dispatched, confirmed, rejected, unknown. A transport
failure after dispatch produces an unknown result, not a claim of no effect.
An accepted close request, disappearance of a window and exit of an application
are separate observable facts.

Long-running work returns an operation ID after a bounded inline wait (maximum
20 seconds). Waiting timeout does not terminate the workload. Persist operation
identity and its last confirmed stage under the run registry so reconnecting to
the MCP process can recover status. Guest reboot invalidates previous-generation
targets and marks unfinished operations lost. Do not reconstruct success from
an old file or PID alone.

Every mutation accepts a caller `request_id`. Bind it to session generation,
operation kind and canonical arguments. Repeating the same request returns its
existing operation/result; changed arguments with the same ID are rejected.
Deduplication state must survive server reconnect. If state was lost, report
unknown and require inspection rather than executing the mutation again.

Reserve transport/executor capacity for control and cancellation: several long
waits or a recording must not prevent takeover or stop. Bound jobs, waiters,
retained events, output and artifacts; report exceeded bounds explicitly.

## Artifact and image delivery

Default to references for screenshots, waveforms, PCM, memory exports, profiles
and recordings. Each artifact descriptor contains:

| Field | Meaning |
| --- | --- |
| `artifact_id` | Stable, session-scoped artifact identity |
| `path` | Absolute local path to the completed artifact |
| `mime_type`, `byte_length`, `sha256` | Format, size and identity |
| `width`, `height` | Image dimensions when applicable |
| `created_host_monotonic` | Capture/export completion time |

Image descriptors also retain frame ID, epoch, crop origin, coordinate extent
and scale. A path is not a replacement for coordinate provenance. Waveforms
retain the source PCM artifact and analysis units.

Add `include_image: false` by default to image-producing tools. When explicitly
true, return a bounded native MCP image content block alongside the descriptor,
never an image data URI or base64 string inside text/structured JSON. Encoded
image bytes still exist in MCP's image transport; they must not be dumped into
language-model text. When local paths cannot be consumed by a client, expose the
explicit preview mechanism rather than fabricating an accessible path.

Use one shared response adapter for all tools, including `vm_artifacts` and
nested results. It must extract tool-owned image/binary fields before JSON
serialization and publish missing artifacts within the selected run. Existing
artifacts should be reused without copying/re-encoding. Prevent symlink/path
escapes, return only completed files, and retain current session access checks
for stopped VMs. Do not accept arbitrary host paths as artifact inputs.

Do not heuristically delete strings from guest stdout: a program may legitimately
print base64. The guarantee applies to payloads owned by the bridge's schema.
Update the local skill/examples to handle references or explicit image blocks
without serializing a complete media-bearing MCP envelope as text.

## Observation bundles

Extend `vm_observe` with selectable components, for example:

```json
{
  "session": "...",
  "include": ["frame", "windows", "processes", "errors", "launches"],
  "since_event_cursor": "...",
  "include_image": false
}
```

Return artifact references, current focus, window geometry/state, correlated
processes, launch state and bounded events/errors since the cursor. Callers
may omit expensive components. Reuse valid cached metadata with an explicit
age; do not perform another screenshot just to list windows.

Each component needs its capture interval, state revision and freshness. The
frame epoch and window revision should identify whether geometry corresponds
to the frame. Cross-component snapshots are not globally atomic merely because
they share a tool response. Report inconsistent/stale components explicitly;
use guest-owned snapshots and revision validation where available. Never fetch
guest commands while QEMU is paused for memory extraction.

Retain full-resolution capture by default and explicit crop/scale options.
Treat minimized, unmapped and offscreen windows as states rather than missing
applications. A rendered frame does not prove that a client is responsive.

## Window and process identity

A window reference binds session generation, a stable window ID and window
incarnation. A process reference binds PID/TGID and incarnation. A title or a
slot index may be used to discover candidates, never as the final mutation
identity. Selectors matching several windows must return candidates or an
ambiguity error, not act on the first match.

Window metadata must distinguish:

- Window provider/compositor process, including `waylandd`.
- Actual client/application process group and its incarnation, when known.
- Application ID and correlated launcher request.
- Parent/transient relationships and primary versus popup/tool windows.

Propagate trustworthy client ownership through the Wayland-to-WM bridge. Unknown
ownership stays unknown; title matching cannot authorize terminating the server
that provides many clients' windows.

## Explicit window and launcher hooks

Add `vm_window` with a generation-bound window reference and one action:

| Action | Required behavior |
| --- | --- |
| `focus` | Activate the specified window using the WM's normal focus path |
| `minimize` | Set minimized state; retain its taskbar entry |
| `maximize` | Set maximized state idempotently; configure the client |
| `restore` | Restore from the requested minimized/maximized state and report resulting geometry |
| `close` | Deliver the normal graceful close request to the actual client |
| `move`, `resize` | Optional negotiated capabilities with explicit coordinate units |

Restore must specify the intended target state when minimized and maximized
state coexist. Preserve the normal restore rectangle. Separate fullscreen from
maximize; advertise fullscreen only when actual display geometry and client
configure behavior work. Hiding client controls is not fullscreen acceptance.

Call the same kernel WM functions used by chrome/taskbar/keyboard actions,
including existing minimize/restore and idempotent maximize helpers. Requests
must enter the normal WM event/ownership context; do not manipulate `Window`
fields from the debug TCP thread. Graceful close waits independently for
request delivery, window disappearance and optional process-group exit.
A timeout returns the remaining state. Any force-terminate action is separate,
explicit, and targets the actual application, never the shared Wayland server.

Add installed-app discovery to the bundle or `vm_apps`, including stable app
ID, catalog generation, installed executable availability and launch eligibility.
Add `vm_launch` with `app_id`, catalog generation and `request_id`. Dispatch
through the same installed-app launcher activation path as the Applications UI:
Brook UID/home/environment policy, shortcut resolution, worker queue, pending
cursor/taskbar feedback, errors and single-instance handling must all execute.
Return a launch reference immediately and correlate its child processes and
windows as they appear. Shelling out to the executable is a different test and
must not be reported as exercising the launcher.

A launch can resolve to an existing single-instance window; report that outcome
without inventing a new process. Preserve pending/failed/still-waiting states.
Expose taskbar activation for an existing window through its normal restore/focus
path. Semantic launcher activation does not certify the pixel menu hitboxes;
retain separate coordinate-input smoke tests for those.

## Guest command jobs

Extend `vm_guest_run` with structured `argv`, `cwd` and `env`, retaining an
explicitly labelled legacy Brook command form. Structured arguments must not be
joined through a host shell. Default to Brook's desktop identity and persistent
home; maintenance identity is an explicit option consistent with existing policy.

Capture stdout and stderr through guest job channels, not an interleaved serial
substring. Return a process reference, exit code or terminating signal, output
cursors and whether each stream reached EOF. A leader may exit while descendants
still run or retain output descriptors; report these independently. A launched
browser's parent exit is not proof of application shutdown.

Use bounded retained output with explicit truncation/loss counters and optional
artifact references. Cursors count bytes, not decoded characters; handle UTF-8
boundaries without silently skipping data. A short inline wait may return the
exit result, output and selected observation in one response. Longer operations
remain recoverable by operation ID. Exceeding the current small debug-command
line limit requires bounded framing/chunking, not unsafe shell quoting or silent
argument truncation.

## Conditions, event waits and action completion

Generalize `vm_wait` from serial substrings to typed conditions:

- Operation/leader exited, or exact process group drained.
- Window created, closed, focused, minimized, maximized or configured/resized.
- Launcher request resolved/failed and its pending feedback cleared.
- Completed frame newer than a supplied epoch, with optional minimum frame count.
- Audio stream active/inactive, guest panic, or explicitly supported app readiness.

Support `all`/`any`, a bounded deadline, event cursor and optional observation on
completion/timeout. Avoid repeated full-log returns. Event buffers have sequence
numbers, generation, timestamps and overflow counters; a lost event must trigger
state reconciliation rather than a false timeout/success. Prefer guest events;
capability-labelled polling fallback is acceptable for read-only conditions.

`vm_window`, `vm_launch`, `vm_guest_run` and `vm_input` can attach a wait condition
and final observation. Return accepted actions, dispatch stages, last confirmed
effect and unsent actions on timeout/cancellation. Release held keys/buttons.
Do not replay a delivered click or launch through another backend. Revalidate
frame/window generations at dispatch, including within multi-action sequences.

Distinguish a newer frame, compositor activity and application readiness. A
browser window or a high compositor FPS does not prove a page loaded. Optional
app readiness probes must identify their evidence and scope.

Human takeover blocks new UI mutations and interrupts queued/in-progress input
at safe boundaries; read-only observation/waits remain available. Do not
silently terminate an already-started command when a client disconnects or a
wait expires. Operation cancellation and workload termination are separate.

## Profiling, audio, shutdown and panic

Add a profile capture operation that starts, waits, exports and returns a compact
summary plus raw/Speedscope references. Preserve the exact kernel/symbol identity,
sample count, duration and context-switch data. Label sampled CPU percentages
separately from wall time; allow deferred completion by operation ID.

Add audio capture-and-analysis returning activity, PCM format/frame count, RMS,
waveform reference and analysis limitations together. Zero captured frames means
an inactive/unavailable stream, not a measured duration of silent PCM. Keep host
arrival times separate from sample time. Add guest mixer/HDA queue and underrun
telemetry as a capability before claiming to identify stutters from silence.
Continue supporting capture with speaker playback disabled.

Allow checked stop plus an optional read-only offline filesystem verification
operation for the selected run's disks. Preserve the existing guest filesystem
and device flush acknowledgement, await QEMU exit and verify disks are no longer
in use before checking them. Never run fsck against a live/mounted/shared image;
never silently repair it. Report each filesystem exit status and log artifact.

After panic, keep scoped artifacts, logs, cached-frame capture and supported
QMP register/RAM reads available. Mark normal commands/UI hooks unavailable and
retain unfinished job state. Distinguish current vCPU registers from saved fault
registers. Restoring the recalled panic command pipe or full dumps is separate
work, not a prerequisite or an assumed existing capability.

## Implementation sequence and acceptance

1. Add the shared result adapter, artifact descriptors and capability handshake.
   Make reference-only image delivery the default; update skill/examples.
2. Add durable operation IDs, command output/exit jobs and typed waits; establish
   generation-bound window/client identity and the window/launcher hooks.
3. Bundle observations and action completion, then profile/audio/stop convenience
   operations over those primitives. Avoid independent bespoke polling loops.

Required tests and evidence:

| Area | Acceptance |
| --- | --- |
| Payloads | Screenshot, waveform, WAV, memory, profile and video default responses contain descriptors, no bridge-owned base64/data-URI text, and no raw bytes in structured JSON |
| Preview | Explicit preview returns one valid native image block plus matching metadata; nested/media-bearing results cannot accidentally enter text serialization |
| Artifacts | Hash/length/dimensions match files; stopped-session reads work; incomplete files, traversal and symlink escapes are rejected |
| Jobs | Exact argv/cwd/environment and desktop identity; stdout/stderr separation; exit 0/nonzero/signal; delayed descendants; output truncation and UTF-8 byte cursors |
| Recovery | Disconnect before/after dispatch, repeated request IDs, lost acknowledgements, reboot, PID/window reuse and server reconnect never duplicate effects |
| Windows | Native and Wayland focus/minimize/restore/maximize/graceful close; geometry/configure confirmation; an unrelated app and shared Wayland server survive |
| Launcher | Actual UI activation path produces pending feedback and resolves/fails correctly; stale catalog, absent executable, queue saturation and single-instance results are explicit |
| Waits | Success, timeout, panic, overflow and cancellation return truthful stages/state; waiting cannot starve takeover/control |
| Bundles | Frame/window revisions and ages are accurate; crops/scales remain usable for input; unsupported components and races are reported |
| Capture | Profile summary matches raw capture; active/inactive/silent audio cases are distinct; known injected gaps validate any later stutter telemetry |
| Stop | Flush failure prevents ordinary stop; verification waits for QEMU exit; all per-disk results are preserved without repair |
| End to end | UI-launch a real installed app, observe it, minimize/restore/maximize, quit, verify exit/task/audio cleanup; repeat with a user-started VM and after reconnect |

Measure tool round trips, wall time and observer overhead against the current
launch/inspect/close and command-completion workflows. Common short operations
should need one call; long operations should need a start and one blocking wait,
with no repeated log polling. Do not improve call count by hiding failures or
weakening the underlying evidence.

## Proposed toolkit accessibility extension

GTK and Qt can expose application controls through Linux AT-SPI over the guest
accessibility D-Bus. This is not yet implemented or verified in Brook. First probe
the guest session/accessibility bus and adapters with a GTK app and a Qt app.
References: [GTK accessibility](https://gnome.pages.gitlab.gnome.org/gtk/gtk4/section-accessibility.html),
[Qt QAccessible](https://doc.qt.io/qt-6/qaccessible.html),
[AT-SPI architecture](https://gnome.pages.gitlab.gnome.org/at-spi2-core/devel-docs/architecture.html).

Proposed tools: a bounded `vm_widgets` query scoped to a known guest app/window,
and `vm_widget_action` supporting only the object's advertised actions. Return
name, role, state, bounds, supported interfaces/actions and child references.
Text/value access must respect sensitive/password states. Bound depth, node count,
text bytes and timeout; permit filtering by role/name and incremental subtree reads.
Duplicate names require explicit disambiguation rather than activating the first.

Bind references to session, host generation, guest boot and accessibility-provider
identity. Resolve and revalidate the object/action immediately before dispatch;
report defunct/changed objects and uncertain replies without replaying actions.
Where window correlation is unavailable, report it as unknown. Keep all D-Bus
access inside the VM; never connect to the host accessibility service. Custom
controls may omit useful semantics. Retain pixel inspection to verify rendering
and use native Brook WM/launcher hooks for controls outside toolkit apps.
