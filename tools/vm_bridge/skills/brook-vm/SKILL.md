---
name: brook-vm
description: Control and inspect feature-enabled Brook QEMU VMs, including input, images, logs, CPU/context-switch profiling, OS statistics, guest launch and post-panic memory inspection. Never targets the host desktop.
---

Start with vm_sessions and vm_doctor; select an explicit live session. Attach
to user-started runs without restarting them. vm_start makes new disk clones.
Keep speaker playback off unless the user requests it.

Use vm_observe to get a full-resolution completed-frame image and frame_id.
Media delivery defaults to local artifact references with absolute path, MIME,
size and checksum. Open the referenced PNG to inspect it. Set include_image:
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
