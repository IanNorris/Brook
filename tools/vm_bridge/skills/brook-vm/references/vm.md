# Brook QEMU bridge

Use `vm_*` only for an opted-in QEMU guest. `vm_sessions` returns compact attachable runs by default; `include_stopped` opts into history and `after` follows `next_cursor`. Select the intended session and inspect `vm_doctor`. Existing sessions survive an MCP reconnect; a fresh agent does not need to recreate them.

`vm_start` uses named experiments and new disk clones with playback off. A user launch can opt in with `BROOK_VM_BRIDGE=1 BROOK_AUDIODEV=none` and the existing experiment script; `run-qemu.sh --bridge` also supports discovery. Do not create a new run or rebuild seeds to answer a question about an existing run. Observe takeover state and do not substitute host desktop/mouse/clipboard tools.

## Observe and act

Use `vm_apps`, `vm_windows`, `vm_stats` and incremental `vm_logs` when the question needs only those data. `vm_wait` can wait for a known serial substring without repeated log dumps. Stats measure guest RAM/process counts and recent compositor-loop intervals, not physical refresh or application GPU time. Optional memory census holds guest mapping locks and is not a default progress poll.

`vm_observe` captures a leased completed frame and briefly pauses QEMU. Normal capture needs the matching capture API; do not loop it as a video recorder. A selectable `include` bundle can omit `frame` and request only windows, processes, launches or errors. Bundle components are sampled over intervals rather than atomically. Keep error cursors and check partial results, truncation, rotation and identity errors.

Media defaults to local artifact references. Inspect a tiny or medium saved preview, then use `vm_frame` with its saved `frame_id` for a native-coordinate crop if text needs more detail. Reading saved frames does not capture again and works after stop; it does not make an old image current for input. Coordinate actions use the viewed derivative frame ID, whose crop/scale provenance maps to the original display. Native crop coordinates always refer to the original frame.

Use `vm_input` only when semantic app/window actions are insufficient or the task tests actual UI hitboxes. Its ordered batches support ASCII-US text, keys, pointer movements and bounded holds/waits. An injection ACK is not proof of a guest reaction. Use an appropriate bounded observation afterward. Takeover prevents later queued input but cannot recall an event already dispatched.

Pass `include_image: true` only when a native MCP preview is useful. Forward individual image blocks through the image renderer; do not stringify a preview-bearing MCP envelope, because it can contain base64. `vm_artifacts` describes saved artifacts including after exit. Keep logs, profiles, screenshots and licensed game assets under their established ignored build locations.

## Profiling and audio

`vm_profile` supports start, stop, status and export. Export after IDLE and use the run's immutable matching kernel ELF. Report producer drops/count agreement and lossy state, not just successful Speedscope conversion.

`vm_audio_capture` captures bounded guest PCM without microphone or speaker playback. `vm_audio_analyze` returns waveform/metrics; silence alone does not identify an underrun. `vm_record` is the continuous VM-only video/audio path, using the prepared recording runtime. Start is asynchronous; observe its status instead of launching another recorder. Keep speaker playback off unless requested.

## Shutdown, reset and panic

For ordinary restart/shutdown, prefer `vm_lifecycle`: resolve app save dialogs, let services/writers quiesce, and observe its retained request through status. The guest lifecycle channel is distinct from ordinary debug TCP; it can operate with networking disconnected when supported. Cancel/commit apply only to the relevant phase of the same request.

Diagnostic `vm_control` stop/reset checks guest sync first. Preserve quiesced writers and inspect the reported filesystem/device-flush guarantee. Force permits unsynced shutdown only when explicitly authorized for that run. A QMP reset ACK alone is not a verified restart: inspect changed guest boot identity, readiness and restoration of reboot policy. After pending/uncertain reset, use `vm_reset_status`, not another reset.

After panic, normal guest execution/input is unavailable. `vm_logs`, QEMU-level `vm_debug` and cached-frame observations can still provide evidence. Current vCPU registers differ from the original fault registers in crash logs. Debug memory reads are bounded RAM operations excluding MMIO; full dumps and a panic-safe guest command pipe are not implemented. Preserve crash evidence before an authorized reset.
