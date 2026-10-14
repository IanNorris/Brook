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
| `vm_control` | `pause`, `resume`, `reset`, `stop`, `takeover`, `release`, or controlled `panic`. Reset changes the generation and invalidates frames. |
| `vm_observe` | Return PNG and frame ID, native geometry, crop origin and scale. Optional native `crop` and `max_width`; GPU readback plus brief VM pause. |
| `vm_input` | Ordered key/chord/text/pointer/scroll/relative/hold/wait batches. Coordinates require the current `frame_id`; crop/scale map back to native coordinates. US ASCII text only; holds release at batch end/cancel. Observe to verify reaction. |
| `vm_logs`, `vm_wait` | Bounded serial/QEMU byte cursors or bounded serial-substring waits. Logs survive VM exit. |
| `vm_guest_run` | Launch a Brook `run /absolute/guest/path args` command, optionally `--uid 1000`. Return PID; no process completion or isolated stdout/stderr yet. Entire framed command fits 220 bytes. |
| `vm_guest` | Existing window/process listings and focus by window index (the `pid` argument). |
| `vm_stats` | Usable RAM/free/used and process counts; p50/p90/p95/max of the recent 256 compositor-loop intervals using the millisecond guest clock. Not app GPU or physical scanout timings. |
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
python3 -m unittest discover -s tools/vm_bridge -p test_bridge.py -v
```

Twelve bridge contract tests cover fragmented tokenized replies, stdio MCP,
PNG pixels, cancellation cleanup, input preflight, crop-coordinate mapping,
reset/panic history, RAM-map selection and resume after failed memory reads.
The existing twelve host tests pass, and the changed kernel/boot image builds.

Live validation on 14 October 2026 attached to a user-started CPU-rendered VM
and an agent-started Intel virgl GL3 VM. Captured native 1920×1080 frames and
scaled/cropped previews; launched a guest app; exported a profile with 404 CPU
samples and 1,391 context switches; recorded silent-output PCM and waveform;
queried OS stats. A controlled panic preserved a readable panic screenshot,
registers and bounded physical RAM inspection. These checks validate the
bridge, not the deferred GL3 rendering, ext2 or audio-stutter fixes.

See [VM design](../../docs/vm-control-mcp.md),
[backlog](../../docs/backlog.md) and [month roadmap](../../docs/roadmap.md).
