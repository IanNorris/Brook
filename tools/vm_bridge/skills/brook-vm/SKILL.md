---
name: brook-vm
description: Control and inspect feature-enabled Brook QEMU VMs, including input, images, logs, CPU/context-switch profiling, OS statistics, guest launch and post-panic memory inspection. Never targets the host desktop.
---

Start with vm_sessions and vm_doctor; select an explicit live session. Attach
to user-started runs without restarting them. vm_start makes new disk clones.
Keep speaker playback off unless the user requests it.

Use vm_observe to get a full-resolution completed-frame image and frame_id.
Normal capture requires the updated kernel and bridge (capture API 2); reload
an older running MCP process after source updates. Captures lease a snapshot
and briefly pause QEMU; do not loop this tool as a video recorder. Coordinate input
must reference that frame. vm_input supports ordered key/mouse actions,
US-ASCII text and bounded waits; holds last only within a batch. An injection
acknowledgement does not prove the guest reacted: observe again or check logs.
Respect takeover. Never substitute host mouse/keyboard/clipboard APIs.

vm_guest_run uses Brook syntax, e.g. run --uid 1000 /nix/profile/bin/brook-run
/nix/bin/brook-player --autoexit /data/videos/big-buck-bunny-720p.mov (one line).
It returns a PID, not the process exit status or isolated stdout/stderr.
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
