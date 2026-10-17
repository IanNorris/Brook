---
name: brook-vm
description: Control and inspect feature-enabled Brook QEMU VMs, including input, images, logs, CPU/context-switch profiling, OS statistics, guest launch and post-panic memory inspection. Never targets the host desktop.
---

Start with vm_sessions and vm_doctor; select an explicit live session. Attach
to user-started runs without restarting them. vm_start makes new disk clones.
Keep speaker playback off unless the user requests it.

Use vm_observe to get a full-resolution completed-frame image and frame_id.
Media delivery defaults to local artifact references with absolute path, MIME,
size and checksum. Inspect `previews.tiny` first for layout and `previews.medium` for more detail.
Use vm_frame with the saved frame_id and native x/y/width/height crop for readable
text without another capture. Optional overlay=true draws a ruler/grid labelled
in native coordinates. Keep it off for detail checks. Crop coordinates always
refer to the original display, including when starting from a preview. Saved
views work after stop; reading them never makes an old capture current for input.
Pass the viewed derivative frame_id into coordinate input for correct mapping. Set include_image:
true on vm_observe, vm_audio_analyze or vm_artifacts only when a native MCP
image preview is useful. Never serialize a preview-bearing MCP envelope into
text: image transport contains base64 even though JSON metadata does not.
vm_artifacts returns references for binary exports too, including after exit.
The same adapter handles nested media and retains coordinate provenance.
Normal capture requires the updated kernel and bridge (capture API 2); reload
an older running MCP process after source updates. Captures lease a snapshot
and briefly pause QEMU; do not loop this tool as a video recorder. Coordinate input
must reference that frame. vm_input supports ordered key/mouse actions,
US-ASCII text and bounded waits; holds last only within a batch. An injection
acknowledgement does not prove the guest reacted: observe again or check logs.
Respect takeover. Never substitute host mouse/keyboard/clipboard APIs.

Use vm_observe include=[frame,windows,processes,launches,errors] for a bundle, or
omit frame to avoid its copy/PNG/QEMU-pause cost. It returns per-component states,
intervals and boot-bound references. Snapshots are not atomic and frame/window
correspondence is unverified; inspect stale/partial states and identity_error.
Pass errors.next_cursor back as since_event_cursor for incremental diagnostics.
Serial pattern scanning is best-effort, not a lossless event stream; check dropped
bytes, rotation/reset, more and pending-line fields. Task snapshots are bounded
and report truncation. Panic frame geometry may be from an earlier guest boot and
is explicitly unverified; no ordinary guest metadata is available after panic.

Prefer vm_guest_run with argv, request_id (fresh 32-character lowercase hex),
optional cwd/env and wait_s. It defaults to Brook UID 1000, cwd/home /data/home,
and null stdin. Use real absolute executable paths. Retrieve longer operations
with vm_job and its operation_id. Preserve stdout/stderr next_offset separately;
check truncation, UTF-8 tails, read errors and EOF. Leader exit is distinct from
descendant exit and output EOF. Reconnect preserves operation identity. After
unknown delivery, inspect first; explicit identical resubmission can recover
verified unfinished staging, while vm_job itself never launches or resumes.
Changed arguments need a fresh request ID. Stop/reset invalidates active jobs;
retrieve useful output before shutdown. A cancelled wait leaves the job running.
These jobs do not exercise launcher UI or add automatic framebuffer windows.

Use vm_apps for installed-app/shortcut discovery without taking a screenshot.
Its app reference binds the published catalog generation, session and guest boot;
app IDs are distinct from a window's desktop app_id. Availability is validated at
catalog build, not freshly probed by each read. Use vm_launch with the exact app
reference and fresh request_id to exercise normal launcher admission/worker and
feedback. Wait for admitted, dispatched or window (default); a window proves
presence, not readiness. Check raw dispatch result and correlation: app_id_presence
can be an existing single-instance window and does not confirm activation. Recover
through vm_ui with the original operation ID; never retry with a new ID after an
uncertain reply. Real menu clicks remain necessary to test hitboxes; command jobs
are a separate execution path.

Prefer vm_windows and vm_window for semantic UI control. Pass the exact returned
window reference, action and fresh request_id; restore also requires an explicit
restore_target (previous, normal or maximized). Each action returns window state.
Add include=[frame,windows,processes,errors] to vm_launch/vm_window/vm_ui when
a post-wait bundle is useful. Its separate post_action_observation can fail without
invalidating action evidence; it samples later and does not prove client readiness.
Use vm_ui to recover an uncertain result; never repeat a mutation with a new ID
just because its reply was lost. Check wait_condition_met, observation_error and
current_window. Close dispatch is separate from disappearance and exact client
origin-group exit; select wait_for closed or client_group_exit when needed.
A Wayland provider is shared; connection-origin metadata is not permission to
signal it. Unknown ownership stays unknown. Host generation, guest boot epoch
and window incarnation must all match. Takeover cancels queued UI work but cannot
recall already-dispatched events or terminate started applications. Queued launches
shared with the user are preserved. Guest reboot invalidates launch history;
stopped-session replies retain cached dispatch facts without a current window.

The legacy command form still uses Brook syntax, e.g. run --uid 1000 /nix/profile/bin/brook-run
/nix/bin/brook-player --autoexit /data/videos/big-buck-bunny-720p.mov (one line).
That form returns a PID, not process exit status or isolated stdout/stderr.
vm_guest lists windows/processes. vm_stats reports RAM and recent compositor
intervals, not GPU draw time or physical display refresh.

vm_profile start captures CPU samples and context switches. Wait for status
IDLE, then export the complete profile and Speedscope artifact. Use the run's
saved kernel ELF. vm_audio_capture records bounded PCM with speaker output
disabled; vm_audio_analyze returns a waveform. Silence alone is not proof of
an underrun; sample-arrival and guest queue telemetry are not yet available.

After panic, use vm_logs, vm_debug and cached-frame vm_observe. Current vCPU
registers differ from original fault registers in crash logs. Normal guest
commands/input are unavailable after panic. Reset only when requested; it
destroys crash state. Memory export is bounded and excludes MMIO; full dumps
and the recalled kernel panic pipe are not implemented. Keep all game data,
captures, dumps and profiles under ignored build directories.

When loading a copied plugin package, set BROOK_ROOT to the Brook checkout;
direct source registration infers it from server.py's location. This plugin
does not install desktop automation backends.

Use vm_record start/status/stop for continuous VM-only video/audio, not repeated
vm_observe calls. Prepare build/vm-recording-runtime with recording-runtime.nix
before launching the VM. Start is asynchronous: poll until completed or failed.
Use BROOK_AUDIODEV=none to retain guest sound capture without speaker output.
Keep masters and timelines under ignored build directories. Timing is observed
at host transport arrival; upstream drops and exact source A/V timing are unknown.

For reset, quiesce writers first. vm_control action=reset checked-syncs, sends one
reset and polls a new boot identity; a QMP ACK alone is insufficient. Inspect
reset_delivery, restart_verified, guest_ready, vm and reboot_policy_restored
separately. Use vm_reset_status after timeout/pending/uncertain delivery; never
repeat reset just to recover a reply. Historical verification survives later
pause/exit. Missing old epoch yields running_unverified, not a verified change.
Older/custom launches without a known restoration policy refuse reset. Force
allows unsynced reset only when explicitly authorized for that disposable run.
