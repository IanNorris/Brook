# Direct VM video and audio recording

The bridge's `vm_record` tool records the selected VM through a private QEMU
VNC connection passed over QMP. It captures neither the host desktop nor a
microphone, opens no new listening port, and sends no input through the display
connection. Still screenshots retain the separate completed-frame lease API.

## Setup and use

Prepare the pinned host encoder before starting a VM:

```sh
nix-build tools/vm_bridge/recording-runtime.nix -o build/vm-recording-runtime
```

Launch a bridge-enabled VM with the existing experiment scripts. Use
`BROOK_AUDIODEV=none` for silent speakers with guest audio capture; `--no-audio`
removes the guest device and is unsuitable for testing guest sound. Reload an
already running MCP server to discover the new tool. Source CLI calls work
immediately:

```sh
python3 tools/vm_bridge/server.py --call vm_record --arguments '{"session":"SESSION_ID","action":"start","duration_s":30,"fps":30,"audio":true,"width":1920,"height":1080}'
python3 tools/vm_bridge/server.py --call vm_record --arguments '{"session":"SESSION_ID","action":"status"}'
python3 tools/vm_bridge/server.py --call vm_record --arguments '{"session":"SESSION_ID","action":"stop"}'
```

Start acknowledges an asynchronous worker; poll status until `complete` or
`failed`. Stop requests early finalization. One recorder may own a session at a
time. Bounds are 1–600 seconds, 1–60 requested frames per second and an even
canvas up to 4096×2160. Smaller guest modes are centered without scaling;
exceeding the selected canvas fails explicitly.

Artifacts stay under ignored `build/vm-bridge/SESSION_ID/artifacts/`: a Matroska
H.264/AAC master, frame timeline JSONL and result JSON. Incomplete output retains
its `.partial.mkv` suffix. `vm_artifacts` returns video path, size, checksum and
MIME metadata rather than embedding large video data in a tool response.

## Timing and limits

Video timestamps describe host-monotonic arrival of complete display updates.
Audio uses QEMU's PCM stream, its sample clock and a bounded receive queue;
idle periods and known missing samples are filled with silence. A long audio
arrival gap without an END event is reported as an estimated discontinuity.
The encoder holds the latest video image between updates and through stop.

Metadata reports coalesced display updates, missed encoder slots, locally
queued audio drops, late samples, silence and estimated discontinuities.
QEMU supplies no source audio timestamps or upstream drop counters: unknown
upstream losses remain unknown. These observations do not establish exact
physical A/V timing. Capture and encoding consume host CPU and memory.

CPU and GPU headless QEMU were exercised. Native SDL display arguments are
preserved by tested launcher code, but concurrent native-window recording,
boot/mode transitions, 4K, long captures and reset-during-live-recording still
need live acceptance. No final showcase production has begun.

## Acceptance on 17 October 2026

All 70 bridge tests passed using the pinned PyAV runtime. Coverage includes
fragmented RFB messages, bounds, real file-descriptor transfer, audio timelines,
stale worker ownership, generation changes and actual codec finalization at
1, 2 and 30 FPS. Live tests covered duplicate-start refusal, killed-worker
recovery, early stop and a deliberately injected 1.2-second encoder stall.

A 20-second GPU recording captured Brook Player playing the 1280×720 Big Buck
Bunny source in an 800×600 window on a native 1920×1080 desktop. The decoded
master contained 513 video frames, lasted 20.021 seconds including AAC padding,
and had no local audio queue drops, late samples or estimated discontinuities.
There were 44 coalesced display updates and 12 missed encoder slots. Guest
playback reported 24 FPS. Host encoder cost was 18.55 CPU seconds over the
20-second take, with about 163 MiB peak resident memory; the file was 17 MiB.

The player closed through its native close button and returned to ten tasks.
A separate two-second recording decoded to zero audio amplitude after close.
Checked shutdown and all three offline filesystem checks passed. CPU and GPU
marker fixtures also produced video and audio; observed onset differences
varied, including a roughly 260 ms CPU case, so one-frame synchronization is
not an accepted guarantee.

Evidence, frozen artifacts and harnesses are retained locally under
`build/workloads/vm-recording-20261017/`. The player session was
`f146cac24aa5496995a094ab25c710d4`, recording
`6c677de42ccc457cabc5b9987bc44a63`. Movie and recording media are not tracked.
Big Buck Bunny: (c) copyright 2008, Blender Foundation / www.bigbuckbunny.org,
[CC BY 3.0](https://peach.blender.org/about/).
